use std::ops::Range;

/// Converts byte offsets to 1-based line and column (in characters).
///
/// Line starts are indexed once, so finding a line is a binary search whatever order offsets are
/// asked in. Columns are counted from a cursor that moves forward within the current line, which
/// keeps many lookups on one long line (an inline table) linear too.
pub(crate) struct Positions<'a> {
    content: &'a str,
    line_starts: Vec<usize>,
    /// The line index, byte offset, and column of the previous answer.
    cursor: (usize, usize, usize),
}

impl<'a> Positions<'a> {
    pub(crate) fn new(content: &'a str) -> Self {
        let line_starts = std::iter::once(0)
            .chain(content.match_indices('\n').map(|(newline, _)| newline + 1))
            .collect();
        Self {
            content,
            line_starts,
            cursor: (0, 0, 1),
        }
    }

    /// The position of `span`'s start, or `(0, 0)` without a span.
    pub(crate) fn at(&mut self, span: Option<Range<usize>>) -> (usize, usize) {
        let Some(span) = span else {
            return (0, 0);
        };
        let target = floor_char_boundary(self.content, span.start);
        // line_starts[0] is 0, so at least one start is <= target.
        let line = self.line_starts.partition_point(|&start| start <= target) - 1;
        let (cursor_line, cursor_offset, cursor_column) = self.cursor;
        let (from, column) = if cursor_line == line && cursor_offset <= target {
            (cursor_offset, cursor_column)
        } else {
            (self.line_starts[line], 1)
        };
        let column = column + self.content[from..target].chars().count();
        self.cursor = (line, target, column);
        (line + 1, column)
    }
}

/// The position of one span; for single lookups such as an error location.
pub(crate) fn line_column(content: &str, span: Option<Range<usize>>) -> (usize, usize) {
    Positions::new(content).at(span)
}

fn floor_char_boundary(content: &str, offset: usize) -> usize {
    let mut offset = offset.min(content.len());
    while !content.is_char_boundary(offset) {
        offset -= 1;
    }
    offset
}

#[cfg(test)]
mod tests {
    use super::{Positions, line_column};

    #[test]
    fn counts_lines_and_characters() {
        let content = "a = 1\nñb = 2\n";
        assert_eq!(line_column(content, Some(0..1)), (1, 1));
        assert_eq!(line_column(content, Some(6..7)), (2, 1));
        // "ñ" is two bytes but one column.
        assert_eq!(line_column(content, Some(8..9)), (2, 2));
        assert_eq!(line_column(content, None), (0, 0));
    }

    #[test]
    fn clamps_out_of_range_offsets() {
        assert_eq!(line_column("ab", Some(10..11)), (1, 3));
        assert_eq!(line_column("ñ", Some(1..2)), (1, 1));
    }

    #[test]
    fn interleaved_lookups_stay_linear() {
        // Alternating between the first and second half rescanned half the document per lookup
        // with a forward-only cursor.
        let lines = 200_000;
        let content = "k = 1\n".repeat(lines);
        let mut positions = Positions::new(&content);
        let started = std::time::Instant::now();
        for line in 0..lines / 2 {
            for line in [line, line + lines / 2] {
                assert_eq!(positions.at(Some(line * 6..line * 6 + 1)), (line + 1, 1));
            }
        }
        assert!(started.elapsed() < std::time::Duration::from_secs(5));
    }

    #[test]
    fn long_line_lookups_move_forward() {
        let content = format!("t = {{{}}}", "a = 1, ".repeat(100_000));
        let mut positions = Positions::new(&content);
        let started = std::time::Instant::now();
        for entry in 0..100_000 {
            let offset = 5 + entry * 7;
            assert_eq!(positions.at(Some(offset..offset + 1)), (1, offset + 1));
        }
        assert!(started.elapsed() < std::time::Duration::from_secs(5));
    }

    #[test]
    fn cursor_matches_single_lookups_in_any_order() {
        let content = "[core]\nx = 1\n\n[video]\nñy = 2\nz = 3";
        let offsets = [1, 7, 15, 30, 22, 7, 0, content.len()];
        let mut positions = Positions::new(content);
        for offset in offsets {
            assert_eq!(
                positions.at(Some(offset..offset + 1)),
                line_column(content, Some(offset..offset + 1)),
                "offset {offset}"
            );
        }
    }
}
