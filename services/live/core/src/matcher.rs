//! The matcher: puts waiting parties together into matches.
//!
//! It is a pure function so the standalone server and the SpacetimeDB module
//! run exactly the same code. Rules are in docs/spec/live.md ("Matchmaking").

use crate::playlist::Playlist;

/// A party waiting in one playlist.
#[derive(Clone, Debug)]
pub struct Ticket {
    /// The caller's own key for the ticket (party id, row id...).
    pub key: u64,
    /// Members, leader first.
    pub members: Vec<u64>,
    pub seniority: u32,
    /// The party's round-trip time to the server (slowest member), in ms.
    pub rtt_ms: u32,
    /// Seconds waited.
    pub waited: u32,
}

/// A match the matcher made.
#[derive(Clone, Debug, PartialEq)]
pub struct Formed {
    /// Indices into the ticket slice that was passed in.
    pub tickets: Vec<usize>,
    /// Every member with their team (0-based).
    pub seats: Vec<(u64, u8)>,
    /// The member who should host: the leader of the longest-waiting party.
    pub host: u64,
}

/// How many candidate groups the search may look at per anchor ticket, so
/// a big queue can't stall the tick.
const SEARCH_BUDGET: usize = 4000;
/// How many candidates per anchor are considered, best first.
const CANDIDATES: usize = 32;

fn stricter(a: f64, b: f64) -> f64 {
    match (a > 0.0, b > 0.0) {
        (true, true) => a.min(b),
        (true, false) => a,
        (false, true) => b,
        _ => 0.0,
    }
}

fn stricter_u(a: u32, b: u32) -> u32 {
    stricter(a as f64, b as f64) as u32
}

/// Whether two tickets may meet. `recent(x, y)` says whether players x and
/// y were paired recently (within the playlist's rematch window).
pub fn compatible(
    pl: &Playlist,
    a: &Ticket,
    b: &Ticket,
    recent: &dyn Fn(u64, u64) -> bool,
) -> bool {
    let sa = &pl.stages[pl.stage_index(a.waited)];
    let sb = &pl.stages[pl.stage_index(b.waited)];

    let ratio = stricter(sa.seniority_ratio, sb.seniority_ratio);
    if ratio > 0.0 {
        let x = a.seniority.max(1) as f64;
        let y = b.seniority.max(1) as f64;
        if x.max(y) / x.min(y) > ratio {
            return false;
        }
    }

    let rtt = stricter_u(sa.max_rtt_sum, sb.max_rtt_sum);
    if rtt > 0 && a.rtt_ms.saturating_add(b.rtt_ms) > rtt {
        return false;
    }

    if pl.avoid_rematch_minutes > 0 && !pl.past_last(a.waited) && !pl.past_last(b.waited) {
        for &x in &a.members {
            for &y in &b.members {
                if recent(x, y) {
                    return false;
                }
            }
        }
    }
    true
}

/// Puts party sizes into `teams` teams of at most `size` without splitting
/// a party. Returns the team of each party, or None. Largest parties first,
/// each into the emptiest team it fits, with backtracking.
pub fn pack(sizes: &[usize], teams: usize, size: usize) -> Option<Vec<u8>> {
    let mut order: Vec<usize> = (0..sizes.len()).collect();
    order.sort_by(|&a, &b| sizes[b].cmp(&sizes[a]).then(a.cmp(&b)));
    let mut fill = vec![0usize; teams];
    let mut out = vec![0u8; sizes.len()];
    fn go(
        i: usize,
        order: &[usize],
        sizes: &[usize],
        fill: &mut [usize],
        out: &mut [u8],
        size: usize,
    ) -> bool {
        if i == order.len() {
            return true;
        }
        let p = order[i];
        let mut tried = Vec::new();
        let mut teams: Vec<usize> = (0..fill.len()).collect();
        teams.sort_by_key(|&t| (fill[t], t));
        for t in teams {
            // Two empty-equal teams are the same choice; try one of them.
            if tried.contains(&fill[t]) {
                continue;
            }
            tried.push(fill[t]);
            if fill[t] + sizes[p] <= size {
                fill[t] += sizes[p];
                out[p] = t as u8;
                if go(i + 1, order, sizes, fill, out, size) {
                    return true;
                }
                fill[t] -= sizes[p];
            }
        }
        false
    }
    if go(0, &order, sizes, &mut fill, &mut out, size) {
        Some(out)
    } else {
        None
    }
}

struct Search<'a> {
    pl: &'a Playlist,
    tickets: &'a [Ticket],
    cands: Vec<usize>,
    ok: Vec<Vec<bool>>, // ok[i][j]: cands i and j may meet
    budget: usize,
    target: usize,
}

impl Search<'_> {
    /// Depth-first over candidates in preference order; the first group that
    /// exactly fills the match (and packs into teams) wins.
    fn go(&mut self, start: usize, group: &mut Vec<usize>, players: usize) -> bool {
        if players == self.target {
            let sizes: Vec<usize> = group.iter().map(|&g| self.tickets[self.cands[g]].members.len()).collect();
            return pack(&sizes, self.pl.teams as usize, self.pl.team_size as usize).is_some();
        }
        for c in start..self.cands.len() {
            if self.budget == 0 {
                return false;
            }
            self.budget -= 1;
            let n = self.tickets[self.cands[c]].members.len();
            if players + n > self.target || !group.iter().all(|&g| self.ok[g][c]) {
                continue;
            }
            group.push(c);
            if self.go(c + 1, group, players + n) {
                return true;
            }
            group.pop();
        }
        false
    }
}

/// Runs one matcher pass over a playlist's tickets.
pub fn match_tickets(
    pl: &Playlist,
    tickets: &[Ticket],
    recent: &dyn Fn(u64, u64) -> bool,
) -> Vec<Formed> {
    let full = pl.players();
    let mut order: Vec<usize> = (0..tickets.len())
        .filter(|&i| !tickets[i].members.is_empty() && tickets[i].members.len() <= pl.team_size as usize)
        .collect();
    order.sort_by(|&a, &b| {
        tickets[b].waited.cmp(&tickets[a].waited).then(tickets[a].key.cmp(&tickets[b].key))
    });
    let mut used = vec![false; tickets.len()];
    let mut out = Vec::new();

    for &anchor in &order {
        if used[anchor] {
            continue;
        }
        let a = &tickets[anchor];
        let la = (a.seniority.max(1) as f64).ln();
        let mut cands: Vec<usize> = order
            .iter()
            .copied()
            .filter(|&c| c != anchor && !used[c] && compatible(pl, a, &tickets[c], recent))
            .collect();
        // Closest seniority first, then longest waiting.
        cands.sort_by(|&x, &y| {
            let dx = ((tickets[x].seniority.max(1) as f64).ln() - la).abs();
            let dy = ((tickets[y].seniority.max(1) as f64).ln() - la).abs();
            dx.partial_cmp(&dy)
                .unwrap()
                .then(tickets[y].waited.cmp(&tickets[x].waited))
                .then(tickets[x].key.cmp(&tickets[y].key))
        });
        cands.truncate(CANDIDATES - 1);
        cands.insert(0, anchor);
        let n = cands.len();
        let mut ok = vec![vec![true; n]; n];
        for i in 0..n {
            for j in (i + 1)..n {
                let v = compatible(pl, &tickets[cands[i]], &tickets[cands[j]], recent);
                ok[i][j] = v;
                ok[j][i] = v;
            }
        }

        // Full match first; then, for a ticket past the last stage, the
        // largest short match allowed by min_players.
        let mut targets = vec![full];
        if pl.min_players > 0 && (pl.min_players as usize) < full && pl.past_last(a.waited) {
            targets.extend((pl.min_players as usize..full).rev());
        }
        let mut found = None;
        for target in targets {
            let mut s = Search { pl, tickets, cands: cands.clone(), ok: ok.clone(), budget: SEARCH_BUDGET, target };
            let mut group = vec![0usize];
            if s.go(1, &mut group, a.members.len()) {
                found = Some(group.iter().map(|&g| cands[g]).collect::<Vec<_>>());
                break;
            }
        }
        let Some(group) = found else { continue };

        let sizes: Vec<usize> = group.iter().map(|&t| tickets[t].members.len()).collect();
        let teams = pack(&sizes, pl.teams as usize, pl.team_size as usize).expect("packed in search");
        let mut seats = Vec::new();
        for (k, &t) in group.iter().enumerate() {
            used[t] = true;
            for &m in &tickets[t].members {
                seats.push((m, teams[k]));
            }
        }
        out.push(Formed { tickets: group, seats, host: a.members[0] });
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn t(key: u64, members: &[u64], seniority: u32, rtt: u32, waited: u32) -> Ticket {
        Ticket { key, members: members.to_vec(), seniority, rtt_ms: rtt, waited }
    }
    fn never(_: u64, _: u64) -> bool {
        false
    }
    fn duel() -> Playlist {
        Playlist::defaults().remove(0)
    }
    fn twos() -> Playlist {
        Playlist::defaults().remove(1)
    }

    #[test]
    fn duel_pairs_close_seniority_first() {
        let pl = duel();
        let ts = vec![t(1, &[1], 10, 20, 5), t(2, &[2], 100, 20, 1), t(3, &[3], 12, 20, 2)];
        let m = match_tickets(&pl, &ts, &never);
        assert_eq!(m.len(), 1);
        assert_eq!(m[0].tickets, vec![0, 2]);
        assert_eq!(m[0].host, 1);
        assert_eq!(m[0].seats, vec![(1, 0), (3, 1)]);
    }

    #[test]
    fn stages_widen_with_time() {
        let pl = duel();
        // 10 vs 25 is 2.5×: not at stage 0 (1.5), fine at stage 1 (3.0),
        // but both tickets must be in stage 1 (the stricter applies).
        let mut ts = vec![t(1, &[1], 10, 20, 16), t(2, &[2], 25, 20, 3)];
        assert!(match_tickets(&pl, &ts, &never).is_empty());
        ts[1].waited = 15;
        assert_eq!(match_tickets(&pl, &ts, &never).len(), 1);
        // Seniority 1 vs 500 only meets at stage 2 (any).
        let ts = vec![t(1, &[1], 1, 20, 40), t(2, &[2], 500, 20, 41)];
        assert_eq!(match_tickets(&pl, &ts, &never).len(), 1);
    }

    #[test]
    fn rtt_limit() {
        let pl = duel();
        let ts = vec![t(1, &[1], 5, 100, 0), t(2, &[2], 5, 100, 0)];
        assert!(match_tickets(&pl, &ts, &never).is_empty());
        let ts = vec![t(1, &[1], 5, 100, 20), t(2, &[2], 5, 100, 20)];
        assert_eq!(match_tickets(&pl, &ts, &never).len(), 1);
    }

    #[test]
    fn rematch_avoided_until_last_stage() {
        let pl = duel();
        let recent = |x: u64, y: u64| (x, y) == (1, 2) || (x, y) == (2, 1);
        let ts = vec![t(1, &[1], 5, 10, 30), t(2, &[2], 5, 10, 30)];
        assert!(match_tickets(&pl, &ts, &recent).is_empty());
        let ts = vec![t(1, &[1], 5, 10, 60), t(2, &[2], 5, 10, 3)];
        assert_eq!(match_tickets(&pl, &ts, &recent).len(), 1);
    }

    #[test]
    fn twos_never_split_parties() {
        let pl = twos();
        // A duo and two solos make a 2v2: duo on one team, solos on the other.
        let ts = vec![t(1, &[1, 2], 8, 10, 9), t(2, &[3], 8, 10, 5), t(3, &[4], 8, 10, 1)];
        let m = match_tickets(&pl, &ts, &never);
        assert_eq!(m.len(), 1);
        let team_of = |p| m[0].seats.iter().find(|s| s.0 == p).unwrap().1;
        assert_eq!(team_of(1), team_of(2));
        assert_eq!(team_of(3), team_of(4));
        assert_ne!(team_of(1), team_of(3));
    }

    #[test]
    fn search_backtracks() {
        let pl = twos();
        // Duo anchor; the closest candidate is a solo, but only duo+duo fills.
        let ts = vec![t(1, &[1, 2], 8, 10, 9), t(2, &[3], 8, 10, 5), t(3, &[4, 5], 9, 10, 1)];
        let m = match_tickets(&pl, &ts, &never);
        assert_eq!(m.len(), 1);
        assert_eq!(m[0].tickets, vec![0, 2]);
    }

    #[test]
    fn min_players_after_last_stage() {
        let mut pl = twos();
        pl.min_players = 3;
        let mut ts = vec![t(1, &[1, 2], 8, 10, 30), t(2, &[3], 8, 10, 30)];
        assert!(match_tickets(&pl, &ts, &never).is_empty());
        ts[0].waited = 80;
        let m = match_tickets(&pl, &ts, &never);
        assert_eq!(m.len(), 1);
        assert_eq!(m[0].seats.len(), 3);
    }

    #[test]
    fn many_tickets_make_many_matches() {
        let pl = duel();
        let ts: Vec<Ticket> = (0..100).map(|i| t(i, &[i], 5, 10, (i % 7) as u32)).collect();
        let m = match_tickets(&pl, &ts, &never);
        assert_eq!(m.len(), 50);
    }

    #[test]
    fn pack_rules() {
        assert_eq!(pack(&[2, 1, 1], 2, 2), Some(vec![0, 1, 1]));
        assert_eq!(pack(&[3, 1], 2, 2), None);
        assert!(pack(&[2, 2, 1, 1, 1, 1], 2, 4).is_some());
    }
}
