//! The lobby: servers, channels and rooms the way the CSNZ client lists them,
//! modelled on CSNZ_Server (src/channel, src/room). A channel server holds
//! channels; a channel's lobby lists rooms. Here a room is a Bulwark game
//! server that registers itself in a channel (room_heartbeat every 30 s,
//! dropped after 120 s without one); joining a room connects to its address.
//!
//! The lobby starts with one channel server and one channel, as CSNZ_Server
//! does by default, named the way it names them ("<name> [1 / 1]",
//! "<name> 1-1"). Operators add more with add_channel_server / add_channel.
//! list_channels and list_rooms need no token, so the game's menu can call
//! them.
//!
//! Original code, ISC licence.

use spacetimedb::{procedure, reducer, table, Identity, ProcedureContext, ReducerContext, Table, Timestamp};

use crate::require_operator;

const DEFAULT_SERVER: &str = "Bulwark";
/// CSNZ_Server's ServerConfig maxPlayers default
const DEFAULT_CHANNEL_PLAYERS: u32 = 600;

#[table(accessor = lobby_server, public)]
pub struct LobbyServer {
    #[primary_key]
    #[auto_inc]
    pub id: u32,
    pub name: String,
}

#[table(accessor = channel, public)]
pub struct Channel {
    #[primary_key]
    #[auto_inc]
    pub id: u32,
    #[index(btree)]
    pub server_id: u32,
    /// 1, 2, ... within its server
    pub index: u32,
    pub name: String,
    pub max_players: u32,
}

#[table(accessor = room, public)]
pub struct Room {
    /// "host:port" as players connect to it
    #[primary_key]
    pub address: String,
    #[index(btree)]
    pub channel_id: u32,
    /// the room number shown in the list, the lowest free one in the channel
    pub number: u32,
    pub title: String,
    pub mode: u32,
    pub map: String,
    pub players: u32,
    pub max_players: u32,
    pub password: bool,
    pub playing: bool,
    pub version: String,
    pub opened: Timestamp,
    pub last_heartbeat: Timestamp,
    pub operator: Identity,
}

fn clean(s: &str) -> String {
    s.replace(['\t', '\n', '\r'], " ")
}

fn server_title(name: &str, index: usize, total: usize) -> String {
    format!("{name} [{index} / {total}]")
}

fn add_server_row(db: &spacetimedb::Local, name: &str, channels: u32) {
    let server = db.lobby_server().insert(LobbyServer { id: 0, name: name.to_string() });
    let index = db.lobby_server().iter().count();
    for c in 1..=channels.max(1) {
        db.channel().insert(Channel {
            id: 0,
            server_id: server.id,
            index: c,
            name: format!("{name} {index}-{c}"),
            max_players: DEFAULT_CHANNEL_PLAYERS,
        });
    }
}

/// Adds a channel server with this many channels.
#[reducer]
pub fn add_channel_server(ctx: &ReducerContext, name: String, channels: u32) -> Result<(), String> {
    require_operator(ctx)?;
    if name.trim().is_empty() || name.len() > 64 || channels > 32 {
        return Err("bad channel server".into());
    }
    add_server_row(&ctx.db, &clean(name.trim()), channels);
    Ok(())
}

/// Adds a channel to a channel server. An empty name takes the
/// CSNZ_Server form "<server> <server index>-<channel index>".
#[reducer]
pub fn add_channel(ctx: &ReducerContext, server_id: u32, name: String, max_players: u32) -> Result<(), String> {
    require_operator(ctx)?;
    let server = ctx.db.lobby_server().id().find(server_id).ok_or("no such channel server")?;
    let index = ctx.db.channel().server_id().filter(server_id).map(|c| c.index).max().unwrap_or(0) + 1;
    let position = ctx.db.lobby_server().iter().filter(|s| s.id <= server_id).count();
    let name = if name.trim().is_empty() { format!("{} {position}-{index}", server.name) } else { clean(name.trim()) };
    ctx.db.channel().insert(Channel {
        id: 0,
        server_id,
        index,
        name,
        max_players: if max_players == 0 { DEFAULT_CHANNEL_PLAYERS } else { max_players },
    });
    Ok(())
}

/// A game server's room in a channel; sent every 30 s while it runs.
#[reducer]
#[allow(clippy::too_many_arguments)]
pub fn room_heartbeat(
    ctx: &ReducerContext,
    channel_id: u32,
    address: String,
    title: String,
    mode: u32,
    map: String,
    players: u32,
    max_players: u32,
    password: bool,
    playing: bool,
    version: String,
) -> Result<(), String> {
    require_operator(ctx)?;
    if address.is_empty() || address.len() > 128 {
        return Err("bad room address".into());
    }
    if ctx.db.channel().id().find(channel_id).is_none() {
        return Err(format!("no channel {channel_id}"));
    }
    let old = ctx.db.room().address().find(address.clone());
    if let Some(r) = &old {
        if r.operator != ctx.sender() {
            return Err("another operator owns that address".into());
        }
    }
    let (number, opened) = match &old {
        Some(r) if r.channel_id == channel_id => (r.number, r.opened),
        _ => {
            let used: Vec<u32> = ctx.db.room().channel_id().filter(channel_id).map(|r| r.number).collect();
            ((1..).find(|n| !used.contains(n)).unwrap_or(1), ctx.timestamp)
        }
    };
    let row = Room {
        address,
        channel_id,
        number,
        title: clean(&title),
        mode,
        map: clean(&map),
        players,
        max_players,
        password,
        playing,
        version: clean(&version),
        opened,
        last_heartbeat: ctx.timestamp,
        operator: ctx.sender(),
    };
    if old.is_some() {
        ctx.db.room().address().update(row);
    } else {
        ctx.db.room().insert(row);
    }
    Ok(())
}

#[reducer]
pub fn room_close(ctx: &ReducerContext, address: String) -> Result<(), String> {
    require_operator(ctx)?;
    if let Some(r) = ctx.db.room().address().find(address.clone()) {
        if r.operator != ctx.sender() {
            return Err("another operator owns that address".into());
        }
        ctx.db.room().address().delete(address);
    }
    Ok(())
}

pub fn drop_stale_rooms(ctx: &ReducerContext, cutoff: Timestamp) {
    let stale: Vec<String> = ctx.db.room().iter().filter(|r| r.last_heartbeat < cutoff).map(|r| r.address).collect();
    for a in stale {
        ctx.db.room().address().delete(a);
    }
}

/// Every channel, one per line:
/// server id TAB server title TAB channel id TAB channel name TAB max players TAB players TAB rooms.
/// Players counts the players in the channel's rooms. Creates the default
/// lobby on first use.
#[procedure]
pub fn list_channels(ctx: &mut ProcedureContext) -> Result<String, String> {
    ctx.try_with_tx(|tx| {
        if tx.db.lobby_server().iter().next().is_none() {
            add_server_row(&tx.db, DEFAULT_SERVER, 1);
        }
        let mut servers: Vec<LobbyServer> = tx.db.lobby_server().iter().collect();
        servers.sort_by_key(|s| s.id);
        let total = servers.len();
        let mut out = String::new();
        for (i, s) in servers.iter().enumerate() {
            let mut channels: Vec<Channel> = tx.db.channel().server_id().filter(s.id).collect();
            channels.sort_by_key(|c| c.index);
            for c in channels {
                let rooms: Vec<Room> = tx.db.room().channel_id().filter(c.id).collect();
                let players: u32 = rooms.iter().map(|r| r.players).sum();
                out += &format!(
                    "{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
                    s.id,
                    server_title(&s.name, i + 1, total),
                    c.id,
                    c.name,
                    c.max_players,
                    players,
                    rooms.len()
                );
            }
        }
        Ok(out)
    })
}

/// A channel's rooms, one per line, in the menu's room fields:
/// number TAB title TAB mode TAB map TAB players TAB max players TAB password (0/1) TAB playing (0/1) TAB address.
#[procedure]
pub fn list_rooms(ctx: &mut ProcedureContext, channel_id: u32) -> Result<String, String> {
    ctx.try_with_tx(|tx| {
        let mut rooms: Vec<Room> = tx.db.room().channel_id().filter(channel_id).collect();
        rooms.sort_by_key(|r| r.number);
        Ok(rooms
            .iter()
            .map(|r| {
                format!(
                    "{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
                    r.number,
                    r.title,
                    r.mode,
                    r.map,
                    r.players,
                    r.max_players,
                    r.password as u8,
                    r.playing as u8,
                    r.address
                )
            })
            .collect())
    })
}
