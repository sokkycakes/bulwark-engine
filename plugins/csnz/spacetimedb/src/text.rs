//! The line formats the CSNZ rules QC already writes to data/csnz/players/,
//! read and written here so the QC can send and receive the same text it
//! keeps in its local files. Every line is "key value...", split on spaces.
//! Numbers come from QC's %g, so they may be in exponent form (1e+06).

/// One parsed line: its key and the rest of the line after one space.
pub fn lines(text: &str) -> impl Iterator<Item = (&str, &str)> {
    text.lines().filter_map(|l| {
        let l = l.trim_end_matches('\r');
        let (key, rest) = l.split_once(' ')?;
        if key.is_empty() {
            None
        } else {
            Some((key, rest))
        }
    })
}

pub fn num(s: &str) -> f64 {
    s.trim().parse::<f64>().ok().filter(|v| v.is_finite()).unwrap_or(0.0)
}

/// QC prints floats with %g; whole numbers print without a fraction.
pub fn g(v: f64) -> String {
    if v == v.trunc() && v.abs() < 1e15 {
        format!("{}", v as i64)
    } else {
        format!("{v}")
    }
}

#[derive(Debug, Default, Clone, PartialEq)]
pub struct Progression {
    pub level: f64,
    pub xp: f64,
    pub xp_total: f64,
    pub rank: f64,
    pub kills: f64,
    pub deaths: f64,
    pub wins: f64,
    pub matches: f64,
    pub playtime: f64,
    pub money: f64,
}

pub const PROGRESSION_KEYS: [&str; 10] =
    ["level", "xp", "xp_total", "rank", "kills", "deaths", "wins", "matches", "playtime", "money"];

impl Progression {
    pub fn parse(text: &str) -> Self {
        let mut p = Progression { level: 1.0, money: 10000.0, ..Default::default() };
        for (key, val) in lines(text) {
            let v = num(val.split(' ').next().unwrap_or(""));
            match key {
                "level" => p.level = v,
                "xp" => p.xp = v,
                "xp_total" => p.xp_total = v,
                "rank" => p.rank = v,
                "kills" => p.kills = v,
                "deaths" => p.deaths = v,
                "wins" => p.wins = v,
                "matches" => p.matches = v,
                "playtime" => p.playtime = v,
                "money" => p.money = v,
                _ => {}
            }
        }
        p
    }

    pub fn values(&self) -> [f64; 10] {
        [
            self.level,
            self.xp,
            self.xp_total,
            self.rank,
            self.kills,
            self.deaths,
            self.wins,
            self.matches,
            self.playtime,
            self.money,
        ]
    }

    pub fn write(&self) -> String {
        PROGRESSION_KEYS
            .iter()
            .zip(self.values())
            .map(|(k, v)| format!("{k} {}\n", g(v)))
            .collect()
    }
}

#[derive(Debug, Default, Clone, PartialEq)]
pub struct Inventory {
    pub items: Vec<String>,
    pub character: String,
    pub primary: String,
    pub secondary: String,
}

impl Inventory {
    pub fn parse(text: &str) -> Self {
        let mut inv = Inventory::default();
        for (key, val) in lines(text) {
            match key {
                "items" => {
                    inv.items.clear();
                    for item in val.split(' ').filter(|s| !s.is_empty()) {
                        if !inv.items.iter().any(|i| i == item) {
                            inv.items.push(item.to_string());
                        }
                    }
                }
                "char" => inv.character = val.trim().to_string(),
                "primary" => inv.primary = val.trim().to_string(),
                "secondary" => inv.secondary = val.trim().to_string(),
                _ => {}
            }
        }
        inv
    }

    pub fn write(&self) -> String {
        format!(
            "items {}\nchar {}\nprimary {}\nsecondary {}\n",
            self.items.join(" "),
            self.character,
            self.primary,
            self.secondary
        )
    }
}

#[derive(Debug, Default, Clone, PartialEq)]
pub struct Achievements {
    /// (achievement id, progress, completed)
    pub ach: Vec<(u32, f64, bool)>,
    /// cumulative counters such as "kills" or "zombie_kills", in file order
    pub counters: Vec<(String, f64)>,
}

impl Achievements {
    pub fn parse(text: &str) -> Self {
        let mut a = Achievements::default();
        for (key, val) in lines(text) {
            if key == "ach" {
                let f: Vec<&str> = val.split(' ').filter(|s| !s.is_empty()).collect();
                if f.len() >= 3 {
                    let id = num(f[0]);
                    if id >= 0.0 && id < 1024.0 {
                        let id = id as u32;
                        a.ach.retain(|(i, _, _)| *i != id);
                        a.ach.push((id, num(f[1]), num(f[2]) != 0.0));
                    }
                }
            } else {
                a.counters.retain(|(k, _)| k != key);
                a.counters.push((key.to_string(), num(val.split(' ').next().unwrap_or(""))));
            }
        }
        a.ach.sort_by_key(|(i, _, _)| *i);
        a
    }

    pub fn write(&self) -> String {
        let mut s = String::new();
        for (id, progress, completed) in &self.ach {
            s += &format!("ach {id} {} {}\n", g(*progress), if *completed { 1 } else { 0 });
        }
        for (k, v) in &self.counters {
            s += &format!("{k} {}\n", g(*v));
        }
        s
    }
}

/// A finished match's players, one per line: key TAB team TAB kills TAB deaths TAB won(0/1).
/// Tabs because a key (today a player name) can hold spaces.
#[derive(Debug, Clone, PartialEq)]
pub struct MatchPlayer {
    pub key: String,
    pub team: u32,
    pub kills: f64,
    pub deaths: f64,
    pub won: bool,
}

pub fn match_players(text: &str) -> Vec<MatchPlayer> {
    text.lines()
        .filter_map(|l| {
            let f: Vec<&str> = l.trim_end_matches('\r').split('\t').collect();
            if f.len() < 5 || f[0].is_empty() {
                return None;
            }
            Some(MatchPlayer {
                key: f[0].to_string(),
                team: num(f[1]).max(0.0) as u32,
                kills: num(f[2]),
                deaths: num(f[3]),
                won: num(f[4]) != 0.0,
            })
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn progression_round_trip() {
        let src = "level 7\nxp 1500\nxp_total 1e+06\nrank 3\nkills 42\ndeaths 9\nwins 4\nmatches 11\nplaytime 3600.5\nmoney 25000\n";
        let p = Progression::parse(src);
        assert_eq!(p.level, 7.0);
        assert_eq!(p.xp_total, 1_000_000.0);
        assert_eq!(p.playtime, 3600.5);
        assert_eq!(Progression::parse(&p.write()), p);
    }

    #[test]
    fn progression_defaults_match_qc() {
        let p = Progression::parse("");
        assert_eq!((p.level, p.money), (1.0, 10000.0));
    }

    #[test]
    fn inventory_round_trip() {
        let src = "items weapon_knife weapon_usp weapon_knife csm_char_phoenix\nchar csm_char_phoenix\nprimary weapon_m4a1\nsecondary weapon_usp\n";
        let inv = Inventory::parse(src);
        assert_eq!(inv.items, ["weapon_knife", "weapon_usp", "csm_char_phoenix"]);
        assert_eq!(inv.character, "csm_char_phoenix");
        assert_eq!(Inventory::parse(&inv.write()), inv);
    }

    #[test]
    fn achievements_round_trip() {
        let src = "ach 0 1 1\nach 1 37 0\nkills 120\nzombie_kills 5\ndamage_dealt 123456\n";
        let a = Achievements::parse(src);
        assert_eq!(a.ach, [(0, 1.0, true), (1, 37.0, false)]);
        assert_eq!(a.counters[2], ("damage_dealt".to_string(), 123456.0));
        assert_eq!(a.write(), src);
    }

    #[test]
    fn match_players_skip_damaged_lines() {
        let m = match_players("sokky the great\t1\t12\t3\t1\nbroken line\n\t2\t0\t0\t0\nbot\t2\t1\t9\t0\n");
        assert_eq!(m.len(), 2);
        assert_eq!(m[0].key, "sokky the great");
        assert!(m[0].won && !m[1].won);
    }
}
