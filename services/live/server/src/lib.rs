//! bwlive-server: the standalone Bulwark Live server (docs/spec/live.md).
//! `Live` holds all state and turns datagrams into datagrams, so it can be
//! tested without sockets; `main.rs` owns the socket.

pub mod live;
pub use live::{Config, Live, Out};
