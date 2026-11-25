//! Bulwark Live rules shared by `bwlive-server` and the `bwlive` SpacetimeDB
//! module: room codes, party rules, playlists and the matcher, plus the UDP
//! wire format (docs/spec/live.md). Original code, ISC licence.

pub mod code;
pub mod matcher;
pub mod party;
pub mod playlist;
pub mod records;
pub mod wire;

pub use matcher::{drop_in, match_tickets, Formed, Open, Ticket};
pub use playlist::{Mode, Pick, Playlist, Stage};
