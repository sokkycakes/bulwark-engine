#!/bin/sh
# Builds the csnz module and publishes it to a SpacetimeDB server.
#   ./publish.sh <database-name> [server]
# server is a `spacetime` CLI server nickname or URL (default: maincloud).
# Needs the Rust wasm32 target (rustup target add wasm32-unknown-unknown)
# and the spacetime CLI, logged in as the database's owner (spacetime login).
set -e
DB="${1:?usage: publish.sh <database-name> [server]}"
SERVER="${2:-maincloud}"
cd "$(dirname "$0")"
cargo build --release --target wasm32-unknown-unknown
spacetime publish --server "$SERVER" --bin-path target/wasm32-unknown-unknown/release/csnz_spacetimedb.wasm "$DB"
