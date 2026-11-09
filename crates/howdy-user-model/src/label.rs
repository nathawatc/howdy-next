//! Model label rules, shared by the codec and the CLI.

use crate::MAX_LABEL_BYTES;

/// Whether `label` is a valid model label: 1 to [`MAX_LABEL_BYTES`] bytes of UTF-8 with no `/`,
/// `\`, or control character (C0, DEL, or C1).
#[must_use]
pub fn is_valid_label(label: &[u8]) -> bool {
    if label.is_empty() || label.len() > MAX_LABEL_BYTES {
        return false;
    }
    let Ok(text) = std::str::from_utf8(label) else {
        return false;
    };
    !text
        .chars()
        .any(|c| c == '/' || c == '\\' || c.is_control())
}

/// Length of the longest prefix of `text` that is at most [`MAX_LABEL_BYTES`] bytes and does not
/// split a UTF-8 sequence. It only cuts; the result still has to pass [`is_valid_label`].
#[must_use]
pub fn label_prefix_len(text: &[u8]) -> usize {
    if text.len() <= MAX_LABEL_BYTES {
        return text.len();
    }
    let mut cut = MAX_LABEL_BYTES;
    while cut > 0 && text[cut] & 0xC0 == 0x80 {
        cut -= 1;
    }
    cut
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accepts_plain_and_multibyte_labels() {
        assert!(is_valid_label(b"Model #0"));
        assert!(is_valid_label("Café 🙂".as_bytes()));
        assert!(is_valid_label(&[b'x'; MAX_LABEL_BYTES]));
    }

    #[test]
    fn rejects_bad_labels() {
        assert!(!is_valid_label(b""));
        assert!(!is_valid_label(&[b'x'; MAX_LABEL_BYTES + 1]));
        assert!(!is_valid_label(b"a/b"));
        assert!(!is_valid_label(b"a\\b"));
        assert!(!is_valid_label(b"a\nb"));
        assert!(!is_valid_label(b"a\x7fb"));
        assert!(!is_valid_label("a\u{9b}b".as_bytes()), "C1 control");
        assert!(!is_valid_label(b"\xC0\x80"), "overlong");
        assert!(!is_valid_label(b"\xED\xA0\x80"), "surrogate");
        assert!(!is_valid_label(b"\xF4\x90\x80\x80"), "above U+10FFFF");
        assert!(!is_valid_label(b"\xE2\x82"), "truncated sequence");
    }

    #[test]
    fn prefix_keeps_short_text() {
        assert_eq!(label_prefix_len(b"short"), 5);
        assert_eq!(label_prefix_len(&[b'x'; MAX_LABEL_BYTES]), MAX_LABEL_BYTES);
    }

    #[test]
    fn prefix_cuts_on_code_point_boundary() {
        // 23 ASCII bytes, then a 4-byte emoji straddling the limit.
        let text = format!("{}🙂tail", "x".repeat(MAX_LABEL_BYTES - 1));
        let cut = label_prefix_len(text.as_bytes());
        assert_eq!(cut, MAX_LABEL_BYTES - 1);
        assert!(is_valid_label(&text.as_bytes()[..cut]));

        let ascii = "y".repeat(MAX_LABEL_BYTES + 5);
        assert_eq!(label_prefix_len(ascii.as_bytes()), MAX_LABEL_BYTES);
    }

    #[test]
    fn prefix_of_only_continuation_bytes_is_empty() {
        assert_eq!(label_prefix_len(&[0x80; MAX_LABEL_BYTES + 1]), 0);
    }
}
