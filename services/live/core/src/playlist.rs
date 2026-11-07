//! Playlists: a queue with its team shape and how the search widens.

use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Stage {
    /// Seconds a ticket has waited before this stage applies.
    pub after: u32,
    /// Larger seniority / smaller seniority must be at most this. 0 = any.
    #[serde(default)]
    pub seniority_ratio: f64,
    /// Sum of the two parties' round-trip times in ms. 0 = any.
    #[serde(default)]
    pub max_rtt_sum: u32,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Playlist {
    pub id: String,
    #[serde(default)]
    pub name: String,
    /// Only players of this game see the playlist. Empty = any game.
    #[serde(default)]
    pub game: String,
    pub team_size: u8,
    pub teams: u8,
    pub stages: Vec<Stage>,
    #[serde(default)]
    pub avoid_rematch_minutes: u32,
    /// With this below `teams * team_size`, a ticket past the last stage
    /// may start a match with fewer players. 0 = always full.
    #[serde(default)]
    pub min_players: u8,
    /// Game modes in the hopper. Each match picks one mode (by weight) and
    /// one of its maps; the host applies the mode's settings and loads the
    /// map. Empty = the host decides (Stiletto's duel).
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub modes: Vec<Mode>,
    /// Matches stay open after they start: players who queue later drop
    /// into one with room before a new match is made. One team only.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub drop_in: bool,
}

/// A game mode within a playlist, such as Firefight or Horde.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Mode {
    pub id: String,
    #[serde(default)]
    pub name: String,
    /// Maps this mode is played on. Empty = the host's own default map.
    #[serde(default)]
    pub maps: Vec<String>,
    /// Console variables the host sets before loading the map.
    #[serde(default)]
    pub settings: BTreeMap<String, String>,
    /// How often this mode comes up relative to the others. 0 = never.
    #[serde(default = "one")]
    pub weight: u32,
}

fn one() -> u32 {
    1
}

/// What a match will be played as.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Pick {
    pub mode: String,
    pub map: String,
    pub settings: Vec<(String, String)>,
}

pub const MAX_MODES: usize = 16;
pub const MAX_MAPS: usize = 32;
/// Encoded size of one mode's settings (u8 length + bytes, twice per
/// setting), so a full 16-seat match message still fits in a datagram.
pub const MAX_SETTINGS_BYTES: usize = 360;

fn ident_ok(s: &str, max: usize, extra: &[u8]) -> bool {
    !s.is_empty()
        && s.len() <= max
        && s.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_' || extra.contains(&c))
}

/// A setting's value may hold any printable ASCII except what would let it
/// break out of a quoted console argument.
fn value_ok(s: &str) -> bool {
    s.len() <= 64 && s.bytes().all(|c| (0x20..0x7f).contains(&c) && c != b'"' && c != b';' && c != b'\\')
}

impl Mode {
    fn validate(&mut self, playlist: &str) -> Result<(), String> {
        if !ident_ok(&self.id, 32, b"-") {
            return Err(format!("{playlist}: mode id must be 1-32 letters, digits, _ or -"));
        }
        if self.name.is_empty() {
            self.name = self.id.clone();
        }
        if self.name.len() > 48 {
            return Err(format!("{playlist}/{}: name is over 48 bytes", self.id));
        }
        if self.maps.len() > MAX_MAPS {
            return Err(format!("{playlist}/{}: at most {MAX_MAPS} maps", self.id));
        }
        for m in &self.maps {
            if !ident_ok(m, 64, b"-./") || m.contains("..") || m.starts_with('/') {
                return Err(format!("{playlist}/{}: bad map name {m:?}", self.id));
            }
        }
        let mut bytes = 0;
        for (k, v) in &self.settings {
            if !ident_ok(k, 48, b"") {
                return Err(format!("{playlist}/{}: bad setting name {k:?}", self.id));
            }
            let low = k.to_ascii_lowercase();
            if low.starts_with("live_") || low.starts_with("rcon") || low.contains("password") {
                return Err(format!("{playlist}/{}: {k} can't be set by a playlist", self.id));
            }
            if !value_ok(v) {
                return Err(format!("{playlist}/{}: bad value for {k}", self.id));
            }
            bytes += 2 + k.len() + v.len();
        }
        if bytes > MAX_SETTINGS_BYTES {
            return Err(format!("{playlist}/{}: settings are over {MAX_SETTINGS_BYTES} bytes", self.id));
        }
        Ok(())
    }
}

impl Playlist {
    pub fn players(&self) -> usize {
        self.teams as usize * self.team_size as usize
    }

    /// Index of the stage a ticket that has waited `waited` seconds is in.
    pub fn stage_index(&self, waited: u32) -> usize {
        self.stages
            .iter()
            .rposition(|s| s.after <= waited)
            .unwrap_or(0)
    }

    /// True once a ticket has waited into the last stage.
    pub fn past_last(&self, waited: u32) -> bool {
        match self.stages.last() {
            Some(s) => waited >= s.after,
            None => true,
        }
    }

    /// Checks the rules and puts the stages in order.
    pub fn validate(mut self) -> Result<Self, String> {
        if self.id.is_empty() || self.id.len() > 32 {
            return Err("playlist id must be 1-32 bytes".into());
        }
        if self.teams == 0 || self.team_size == 0 || self.players() > 16 {
            return Err(format!("{}: teams × team_size must be 1-16", self.id));
        }
        if self.stages.is_empty() {
            self.stages.push(Stage { after: 0, seniority_ratio: 0.0, max_rtt_sum: 0 });
        }
        self.stages.sort_by_key(|s| s.after);
        for s in &self.stages {
            if !(s.seniority_ratio == 0.0 || s.seniority_ratio >= 1.0) {
                return Err(format!("{}: seniority_ratio must be 0 or at least 1", self.id));
            }
        }
        if self.name.is_empty() {
            self.name = self.id.clone();
        }
        if self.drop_in && self.teams != 1 {
            return Err(format!("{}: drop_in needs one team", self.id));
        }
        if self.modes.len() > MAX_MODES {
            return Err(format!("{}: at most {MAX_MODES} modes", self.id));
        }
        let id = self.id.clone();
        for m in &mut self.modes {
            m.validate(&id)?;
        }
        for (i, m) in self.modes.iter().enumerate() {
            if self.modes[..i].iter().any(|o| o.id == m.id) {
                return Err(format!("{}: mode {} is listed twice", self.id, m.id));
            }
        }
        if !self.modes.is_empty() && self.modes.iter().all(|m| m.weight == 0) {
            return Err(format!("{}: every mode has weight 0", self.id));
        }
        Ok(self)
    }

    /// Picks the mode and map for a new match. `roll` is any random number;
    /// `last` is the previous match's (mode, map) on this playlist, which is
    /// not repeated while there's anything else to play.
    pub fn pick(&self, roll: u64, last: Option<(&str, &str)>) -> Pick {
        // every (mode, map) choice that isn't a repeat
        let fresh = |m: &Mode, map: &str| last.map_or(true, |(lm, lmap)| !(m.id == lm && map == lmap));
        let maps_of = |m: &Mode, avoid: bool| -> Vec<String> {
            let all: Vec<String> = if m.maps.is_empty() { vec![String::new()] } else { m.maps.clone() };
            if avoid {
                all.into_iter().filter(|map| fresh(m, map)).collect()
            } else {
                all
            }
        };
        let live: Vec<&Mode> = self.modes.iter().filter(|m| m.weight > 0).collect();
        if live.is_empty() {
            return Pick::default();
        }
        let avoid = live.iter().any(|m| !maps_of(m, true).is_empty())
            && live.iter().map(|m| maps_of(m, false).len()).sum::<usize>() > 1;
        let options: Vec<(&Mode, Vec<String>)> = live
            .into_iter()
            .map(|m| (m, maps_of(m, avoid)))
            .filter(|(_, maps)| !maps.is_empty())
            .collect();
        let total: u64 = options.iter().map(|(m, _)| m.weight as u64).sum();
        let r1 = mix(roll);
        let mut at = r1 % total;
        let mut chosen = &options[0];
        for o in &options {
            if at < o.0.weight as u64 {
                chosen = o;
                break;
            }
            at -= o.0.weight as u64;
        }
        let (mode, maps) = chosen;
        let map = maps[(mix(r1) % maps.len() as u64) as usize].clone();
        Pick {
            mode: mode.id.clone(),
            map,
            settings: mode.settings.iter().map(|(k, v)| (k.clone(), v.clone())).collect(),
        }
    }

    pub fn mode(&self, id: &str) -> Option<&Mode> {
        self.modes.iter().find(|m| m.id == id)
    }
}

/// splitmix64, so nearby rolls (match ids) still spread out.
fn mix(x: u64) -> u64 {
    let mut z = x.wrapping_add(0x9e37_79b9_7f4a_7c15);
    z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    z ^ (z >> 31)
}

impl Playlist {

    pub fn from_json(json: &str) -> Result<Self, String> {
        serde_json::from_str::<Playlist>(json)
            .map_err(|e| e.to_string())?
            .validate()
    }

    /// A file holds either one playlist or a list of them.
    pub fn list_from_json(json: &str) -> Result<Vec<Self>, String> {
        let v: serde_json::Value = serde_json::from_str(json).map_err(|e| e.to_string())?;
        let items = match v {
            serde_json::Value::Array(a) => a,
            other => vec![other],
        };
        items
            .into_iter()
            .map(|i| {
                serde_json::from_value::<Playlist>(i)
                    .map_err(|e| e.to_string())
                    .and_then(Playlist::validate)
            })
            .collect()
    }

    /// The playlists a server starts with when it isn't given any:
    /// Stiletto's duel and 2v2, and the co-op mod's Campaign, Firefight
    /// and Horde.
    pub fn defaults() -> Vec<Self> {
        Self::list_from_json(DEFAULT_PLAYLISTS).expect("built-in playlists")
    }
}

pub const DEFAULT_PLAYLISTS: &str = r#"[
  { "id": "duel", "name": "Duel", "game": "stiletto", "team_size": 1, "teams": 2,
    "stages": [
      { "after": 0,  "seniority_ratio": 1.5, "max_rtt_sum": 150 },
      { "after": 15, "seniority_ratio": 3.0, "max_rtt_sum": 250 },
      { "after": 40, "seniority_ratio": 0,   "max_rtt_sum": 400 },
      { "after": 60, "seniority_ratio": 0,   "max_rtt_sum": 0 }
    ],
    "avoid_rematch_minutes": 10 },
  { "id": "2v2", "name": "2v2", "game": "stiletto", "team_size": 2, "teams": 2,
    "stages": [
      { "after": 0,  "seniority_ratio": 1.5, "max_rtt_sum": 150 },
      { "after": 20, "seniority_ratio": 3.0, "max_rtt_sum": 250 },
      { "after": 45, "seniority_ratio": 0,   "max_rtt_sum": 400 },
      { "after": 75, "seniority_ratio": 0,   "max_rtt_sum": 0 }
    ],
    "avoid_rematch_minutes": 10 },

  { "id": "campaign", "name": "Campaign Co-op", "game": "coop", "team_size": 4, "teams": 1,
    "stages": [
      { "after": 0,  "max_rtt_sum": 150 },
      { "after": 20, "max_rtt_sum": 300 },
      { "after": 45 }
    ],
    "min_players": 2,
    "modes": [
      { "id": "campaign", "name": "Campaign",
        "maps": ["c1a1", "c1a3", "c2a1", "c2a3", "c2a5", "c3a1"],
        "settings": { "coop_waves": "0", "coop_respawnRule": "timer" } }
    ] },
  { "id": "firefight", "name": "Firefight", "game": "coop", "team_size": 4, "teams": 1,
    "stages": [
      { "after": 0,  "max_rtt_sum": 150 },
      { "after": 20, "max_rtt_sum": 300 },
      { "after": 45 }
    ],
    "min_players": 2,
    "modes": [
      { "id": "firefight", "name": "Firefight", "maps": ["coop_arena"], "weight": 3,
        "settings": { "coop_waves": "1", "coop_respawnRule": "lives", "coop_lives": "7",
                      "coop_livesPerWave": "1", "coop_waveDelay": "10" } },
      { "id": "grunt_rush", "name": "Grunt Rush", "maps": ["coop_arena"],
        "settings": { "coop_waves": "1", "coop_respawnRule": "lives", "coop_lives": "7",
                      "coop_livesPerWave": "1", "coop_waveDelay": "10",
                      "coop_waveMonsters": "monster_human_grunt" } }
    ] },
  { "id": "horde", "name": "Horde", "game": "coop", "team_size": 8, "teams": 1,
    "stages": [
      { "after": 0, "max_rtt_sum": 150 },
      { "after": 8 }
    ],
    "min_players": 1,
    "drop_in": true,
    "modes": [
      { "id": "horde", "name": "Horde", "maps": ["coop_arena"], "weight": 2,
        "settings": { "coop_waves": "1", "coop_respawnRule": "wave", "coop_waveDelay": "30",
                      "coop_wavePerWave": "4", "coop_survivorFill": "4" } },
      { "id": "horde_lives", "name": "Horde: Lives", "maps": ["coop_arena"],
        "settings": { "coop_waves": "1", "coop_respawnRule": "lives", "coop_lives": "5",
                      "coop_livesPerWave": "1", "coop_waveDelay": "30",
                      "coop_wavePerWave": "4", "coop_survivorFill": "4" } }
    ] }
]"#;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_parse_and_stage() {
        let p = Playlist::defaults();
        assert_eq!(p.len(), 5);
        let duel = &p[0];
        assert_eq!(duel.players(), 2);
        assert_eq!(duel.stage_index(0), 0);
        assert_eq!(duel.stage_index(14), 0);
        assert_eq!(duel.stage_index(15), 1);
        assert_eq!(duel.stage_index(1000), 3);
        assert!(!duel.past_last(59));
        assert!(duel.past_last(60));
    }

    #[test]
    fn coop_playlists() {
        let all = Playlist::defaults();
        let ff = all.iter().find(|p| p.id == "firefight").unwrap();
        assert_eq!((ff.teams, ff.team_size, ff.game.as_str()), (1, 4, "coop"));
        assert_eq!(ff.modes.len(), 2);
        assert_eq!(ff.modes[1].weight, 1);
        let duel = all.iter().find(|p| p.id == "duel").unwrap();
        assert_eq!(duel.pick(7, None), Pick::default());
    }

    #[test]
    fn pick_weights_and_no_repeat() {
        let pl = Playlist::from_json(r#"{"id":"x","team_size":4,"teams":1,"stages":[],"modes":[
            {"id":"a","maps":["m1","m2"],"weight":3,"settings":{"k":"1"}},
            {"id":"b","maps":["m1"]},
            {"id":"off","maps":["m9"],"weight":0}]}"#).unwrap();
        let mut seen = std::collections::HashMap::new();
        for roll in 0..4000u64 {
            let p = pl.pick(roll, None);
            assert_ne!(p.mode, "off");
            *seen.entry((p.mode.clone(), p.map.clone())).or_insert(0) += 1;
            if p.mode == "a" {
                assert_eq!(p.settings, vec![("k".to_string(), "1".to_string())]);
            }
        }
        let a: i32 = seen.iter().filter(|(k, _)| k.0 == "a").map(|(_, v)| *v).sum();
        assert!(a > 2700 && a < 3300, "mode a picked {a} of 4000");
        for roll in 0..500u64 {
            let p = pl.pick(roll, Some(("a", "m1")));
            assert!(!(p.mode == "a" && p.map == "m1"));
        }
        // a single choice is repeated rather than nothing played
        let one = Playlist::from_json(r#"{"id":"y","team_size":1,"teams":1,"stages":[],"modes":[{"id":"a","maps":["m"]}]}"#).unwrap();
        assert_eq!(one.pick(1, Some(("a", "m"))).map, "m");
    }

    #[test]
    fn rejects_bad_modes() {
        let bad = [
            r#"[{"id":"m","maps":["../x"]}]"#,
            r#"[{"id":"m","settings":{"rcon password":"1"}}]"#,
            r#"[{"id":"m","settings":{"k":"1; quit"}}]"#,
            r#"[{"id":"m","settings":{"sv_password":"x"}}]"#,
            r#"[{"id":"m","settings":{"live_server":"evil"}}]"#,
            r#"[{"id":"m","settings":{"k":"\"x"}}]"#,
            r#"[{"id":"m"},{"id":"m"}]"#,
            r#"[{"id":"m","weight":0}]"#,
            r#"[{"id":"has space"}]"#,
        ];
        for modes in bad {
            let json = format!(r#"{{"id":"x","team_size":1,"teams":1,"stages":[],"modes":{modes}}}"#);
            assert!(Playlist::from_json(&json).is_err(), "accepted {modes}");
        }
    }

    #[test]
    fn rejects_bad() {
        assert!(Playlist::from_json(r#"{"id":"x","team_size":9,"teams":2,"stages":[]}"#).is_err());
        assert!(Playlist::from_json(r#"{"id":"x","team_size":1,"teams":2,"stages":[{"after":0,"seniority_ratio":0.5}]}"#).is_err());
        let p = Playlist::from_json(r#"{"id":"x","team_size":1,"teams":2,"stages":[]}"#).unwrap();
        assert_eq!(p.stages.len(), 1);
    }
}
