# Bulwark Live servers

Rendezvous, relay and matchmaking for Bulwark games. The spec is
[docs/spec/live.md](../../docs/spec/live.md).

- `core/`: rules shared by both forms (room codes, party rules, playlists,
  the matcher, the wire format).
- `server/`: `bwlive-server`, the standalone server.
- `stdb/`: the `bwlive` SpacetimeDB module.

## Standalone

    cargo build --release
    target/release/bwlive-server --bind 0.0.0.0:27950 --http 127.0.0.1:27951 \
        [--playlists playlists.json] [--max-players 4096] [--relay-kbps 64]

It uses one UDP port. The status page is JSON and should be kept on
localhost or behind a proxy. Without `--playlists` it serves Stiletto's
`duel` and `2v2` and the co-op mod's `campaign`, `firefight` and `horde`
(see "Game modes in the hopper" in docs/spec/live.md). Players run `live_connect host` in the game.

## SpacetimeDB

    cd stdb && ./publish.sh <database> [server]
    bwlive-server --relay-only --bind 0.0.0.0:27950

The module keeps parties, queues and matches, and runs the matcher every
second. The relay-only server gives players their id and public address,
and carries traffic when punching fails.

The engine's built-in client speaks only the standalone protocol. A client
for the SpacetimeDB form needs to:

1. Say `hello` to the relay.
2. Call the module's `hello` reducer with the relay's id and address.
3. Poll the `state` procedure.

This form hasn't run against a real SpacetimeDB yet. One thing still has
to be checked there: who `init`'s sender is (it becomes the first
operator).

Known limit: the module trusts the `relay_id` a client reports. It refuses
an id that another live player already holds, but a signed `welcome` from
the relay would close this properly.
