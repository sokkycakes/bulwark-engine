//! Fighter records: what the server knows about each player's finished sets,
//! shown on the fighter card before and after a set (docs/spec/live.md,
//! "Fighter records"). Nothing here is a skill rating: these are counts, and
//! the card shows them as they are. Seniority for the queue is the number of
//! distinct days with a completed set, which no amount of grinding can raise
//! faster than the calendar.

use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet, HashMap};

/// A seat's outcome in a set report.
pub mod outcome {
    pub const WIN: u8 = 0;
    pub const LOSS: u8 = 1;
    /// Left the set while it was running (menu, or the game closed cleanly).
    pub const QUIT: u8 = 2;
    /// Dropped by a timeout while the set was running.
    pub const DISCONNECT: u8 = 3;
}

/// A set report's result.
pub mod result {
    /// Played to the end.
    pub const COMPLETE: u8 = 0;
    /// Someone left; the other side won by forfeit.
    pub const FORFEIT: u8 = 1;
}

/// Last results kept for the form strip.
pub const FORM_LEN: usize = 10;
/// Heroes listed on a card.
pub const CARD_HEROES: usize = 3;

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct Record {
    pub name: String,
    /// Sets this player was seated in, however they ended.
    pub sets: u32,
    /// Sets they stayed in to the end (their own or a forfeit win).
    pub completed: u32,
    pub wins: u32,
    pub losses: u32,
    pub quits: u32,
    /// Quits while their side was behind.
    pub quits_behind: u32,
    pub disconnects: u32,
    /// Sets that were a rematch of the one before ("runbacks").
    pub runbacks: u32,
    /// Sets won because the other side left.
    pub forfeit_wins: u32,
    /// Days (since 1970-01-01, UTC) with a set they stayed in to the end.
    pub days: BTreeSet<u32>,
    /// Newest last: W, L, Q (quit) or D (disconnect).
    pub form: String,
    pub heroes: BTreeMap<String, u32>,
}

/// One seat of a set report, as the host sends it.
#[derive(Clone, Debug, PartialEq)]
pub struct SeatResult {
    pub outcome: u8,
    pub behind: bool,
    pub hero: String,
    pub runback: bool,
}

impl Record {
    pub fn seniority(&self) -> u32 {
        self.days.len() as u32
    }

    pub fn apply(&mut self, name: &str, r: &SeatResult, set_result: u8, day: u32) {
        if !name.is_empty() {
            self.name = name.chars().take(32).collect();
        }
        self.sets += 1;
        let ch = match r.outcome {
            outcome::WIN => {
                self.wins += 1;
                if set_result == result::FORFEIT {
                    self.forfeit_wins += 1;
                }
                'W'
            }
            outcome::LOSS => {
                self.losses += 1;
                'L'
            }
            outcome::QUIT => {
                self.quits += 1;
                if r.behind {
                    self.quits_behind += 1;
                }
                'Q'
            }
            _ => {
                self.disconnects += 1;
                'D'
            }
        };
        if r.outcome <= outcome::LOSS {
            self.completed += 1;
            self.days.insert(day);
        }
        if r.runback {
            self.runbacks += 1;
        }
        self.form.push(ch);
        let n = self.form.chars().count();
        if n > FORM_LEN {
            self.form = self.form.chars().skip(n - FORM_LEN).collect();
        }
        if !r.hero.is_empty() && r.hero.len() <= 32 && r.hero.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_') {
            *self.heroes.entry(r.hero.clone()).or_insert(0) += 1;
        }
    }

    /// The card as one JSON object. `id` is the player's current id.
    pub fn card_json(&self, id: u32, name: &str) -> String {
        let mut heroes: Vec<(&String, &u32)> = self.heroes.iter().collect();
        heroes.sort_by(|a, b| b.1.cmp(a.1).then(a.0.cmp(b.0)));
        let heroes: Vec<serde_json::Value> = heroes
            .into_iter()
            .take(CARD_HEROES)
            .map(|(h, n)| serde_json::json!({ "id": h, "sets": n }))
            .collect();
        let pct = |n: u32| if self.sets == 0 { 100 } else { (n * 100 + self.sets / 2) / self.sets };
        serde_json::json!({
            "id": id,
            "name": if name.is_empty() { self.name.as_str() } else { name },
            "sets": self.sets,
            "completed": self.completed,
            "completion": pct(self.completed),
            "wins": self.wins,
            "losses": self.losses,
            "quits": self.quits,
            "quits_behind": self.quits_behind,
            "disconnects": self.disconnects,
            "runbacks": self.runbacks,
            "forfeit_wins": self.forfeit_wins,
            "active_days": self.seniority(),
            "form": self.form,
            "heroes": heroes,
        })
        .to_string()
    }
}

/// Every player's record, keyed by "token" or "token:slot" for a
/// split-screen guest (guests have no token of their own).
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct Records {
    pub players: HashMap<String, Record>,
}

pub fn key(token: u64, slot: u8) -> String {
    if slot == 0 {
        format!("{token:016x}")
    } else {
        format!("{token:016x}:{slot}")
    }
}

/// Days since 1970-01-01 for a Unix time in seconds.
pub fn day_of(unix_secs: u64) -> u32 {
    (unix_secs / 86_400) as u32
}

impl Records {
    pub fn get(&self, k: &str) -> Option<&Record> {
        self.players.get(k)
    }
    pub fn apply(&mut self, k: &str, name: &str, r: &SeatResult, set_result: u8, day: u32) {
        self.players.entry(k.to_string()).or_default().apply(name, r, set_result, day);
    }
    pub fn seniority(&self, k: &str) -> u32 {
        self.players.get(k).map(|r| r.seniority()).unwrap_or(0)
    }
    pub fn to_json(&self) -> String {
        serde_json::to_string(self).unwrap_or_default()
    }
    pub fn from_json(s: &str) -> Result<Records, String> {
        serde_json::from_str(s).map_err(|e| e.to_string())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn seat(outcome: u8, behind: bool) -> SeatResult {
        SeatResult { outcome, behind, hero: "hero_juliet".into(), runback: false }
    }

    #[test]
    fn counts_and_form() {
        let mut r = Record::default();
        for i in 0..12 {
            r.apply("ann", &seat(if i % 3 == 0 { outcome::LOSS } else { outcome::WIN }, false), result::COMPLETE, 100 + i / 4);
        }
        assert_eq!(r.sets, 12);
        assert_eq!(r.wins, 8);
        assert_eq!(r.losses, 4);
        assert_eq!(r.completed, 12);
        assert_eq!(r.seniority(), 3);
        assert_eq!(r.form.len(), FORM_LEN);
        assert_eq!(r.heroes["hero_juliet"], 12);
    }

    #[test]
    fn quits_are_not_completions_and_add_no_days() {
        let mut r = Record::default();
        r.apply("bo", &seat(outcome::QUIT, true), result::FORFEIT, 7);
        r.apply("bo", &seat(outcome::DISCONNECT, false), result::FORFEIT, 8);
        assert_eq!((r.quits, r.quits_behind, r.disconnects, r.completed), (1, 1, 1, 0));
        assert_eq!(r.seniority(), 0);
        assert_eq!(r.form, "QD");
        let card: serde_json::Value = serde_json::from_str(&r.card_json(5, "")).unwrap();
        assert_eq!(card["completion"], 0);
        assert_eq!(card["name"], "bo");
    }

    #[test]
    fn round_trips_as_json() {
        let mut all = Records::default();
        all.apply(&key(0xabc, 0), "ann", &seat(outcome::WIN, false), result::FORFEIT, 3);
        let back = Records::from_json(&all.to_json()).unwrap();
        assert_eq!(back.get(&key(0xabc, 0)).unwrap().forfeit_wins, 1);
        assert_eq!(key(1, 2), "0000000000000001:2");
    }
}
