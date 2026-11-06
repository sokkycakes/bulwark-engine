//! Party rules both forms share.

pub const MIN_MEMBERS: u8 = 2;
pub const MAX_MEMBERS: u8 = 16;
/// Flag bit 0: anyone with the code may join.
pub const FLAG_JOINABLE: u8 = 1;
pub const MAX_INFO: usize = 255;
pub const MAX_NAME: usize = 32;
pub const MAX_STATUS: usize = 64;

/// Clamps a requested party size to what's allowed.
pub fn clamp_max(requested: u8) -> u8 {
    requested.clamp(MIN_MEMBERS, MAX_MEMBERS)
}

/// The new leader when the leader leaves: the member who joined first.
/// `members` is `(player, joined order)`; the leaving player is already gone.
pub fn next_leader<P: Copy>(members: &[(P, u64)]) -> Option<P> {
    members.iter().min_by_key(|(_, order)| *order).map(|(p, _)| *p)
}

/// Cuts a string to `max` bytes on a character boundary.
pub fn clip(s: &str, max: usize) -> String {
    if s.len() <= max {
        return s.to_string();
    }
    let mut end = max;
    while !s.is_char_boundary(end) {
        end -= 1;
    }
    s[..end].to_string()
}

/// A display name: control characters removed, clipped, never empty.
pub fn clean_name(s: &str) -> String {
    let s: String = s.chars().filter(|c| !c.is_control()).collect();
    let s = clip(s.trim(), MAX_NAME);
    if s.is_empty() {
        "player".into()
    } else {
        s
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rules() {
        assert_eq!(clamp_max(0), 2);
        assert_eq!(clamp_max(40), 16);
        assert_eq!(next_leader(&[(7u32, 5), (3, 2), (9, 8)]), Some(3));
        assert_eq!(next_leader::<u32>(&[]), None);
        assert_eq!(clip("héllo", 2), "h");
        assert_eq!(clean_name(" \x07 "), "player");
    }
}
