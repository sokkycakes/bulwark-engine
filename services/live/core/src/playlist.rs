//! Playlists: a queue with its team shape and how the search widens.

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
        Ok(self)
    }

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
    /// Stiletto's duel and 2v2.
    pub fn defaults() -> Vec<Self> {
        Self::list_from_json(DEFAULT_PLAYLISTS).expect("built-in playlists")
    }
}

pub const DEFAULT_PLAYLISTS: &str = r#"[
  { "id": "duel", "name": "Duel", "team_size": 1, "teams": 2,
    "stages": [
      { "after": 0,  "seniority_ratio": 1.5, "max_rtt_sum": 150 },
      { "after": 15, "seniority_ratio": 3.0, "max_rtt_sum": 250 },
      { "after": 40, "seniority_ratio": 0,   "max_rtt_sum": 400 },
      { "after": 60, "seniority_ratio": 0,   "max_rtt_sum": 0 }
    ],
    "avoid_rematch_minutes": 10 },
  { "id": "2v2", "name": "2v2", "team_size": 2, "teams": 2,
    "stages": [
      { "after": 0,  "seniority_ratio": 1.5, "max_rtt_sum": 150 },
      { "after": 20, "seniority_ratio": 3.0, "max_rtt_sum": 250 },
      { "after": 45, "seniority_ratio": 0,   "max_rtt_sum": 400 },
      { "after": 75, "seniority_ratio": 0,   "max_rtt_sum": 0 }
    ],
    "avoid_rematch_minutes": 10 }
]"#;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_parse_and_stage() {
        let p = Playlist::defaults();
        assert_eq!(p.len(), 2);
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
    fn rejects_bad() {
        assert!(Playlist::from_json(r#"{"id":"x","team_size":9,"teams":2,"stages":[]}"#).is_err());
        assert!(Playlist::from_json(r#"{"id":"x","team_size":1,"teams":2,"stages":[{"after":0,"seniority_ratio":0.5}]}"#).is_err());
        let p = Playlist::from_json(r#"{"id":"x","team_size":1,"teams":2,"stages":[]}"#).unwrap();
        assert_eq!(p.stages.len(), 1);
    }
}
