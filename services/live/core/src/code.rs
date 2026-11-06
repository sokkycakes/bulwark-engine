//! Six-character room codes.

/// Letters and digits that can't be mistaken for each other (no I, O, 0, 1).
pub const ALPHABET: &[u8; 32] = b"ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
pub const LEN: usize = 6;

/// Makes a code from 30 random bits (five bits per character).
pub fn from_bits(bits: u64) -> String {
    (0..LEN)
        .map(|i| ALPHABET[((bits >> (5 * i)) & 31) as usize] as char)
        .collect()
}

/// Turns what a player typed into a code, or `None` if it can't be one.
/// Case, spaces and dashes don't matter.
pub fn normalize(typed: &str) -> Option<String> {
    let s: String = typed
        .chars()
        .filter(|c| !c.is_whitespace() && *c != '-')
        .map(|c| c.to_ascii_uppercase())
        .collect();
    if s.len() == LEN && s.bytes().all(|b| ALPHABET.contains(&b)) {
        Some(s)
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn codes_round_trip() {
        for bits in [0u64, 1, 0x3fff_ffff, 0x1234_5678, u64::MAX] {
            let c = from_bits(bits);
            assert_eq!(c.len(), LEN);
            assert_eq!(normalize(&c.to_lowercase()), Some(c.clone()));
        }
        assert_eq!(normalize("abc-def"), Some("ABCDEF".into()));
        assert_eq!(normalize("ABCDE0"), None);
        assert_eq!(normalize("ABCDEFG"), None);
        assert_eq!(normalize("ABC"), None);
    }
}
