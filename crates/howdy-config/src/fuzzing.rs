//! Properties shared by the `cargo fuzz` targets and the corpus replay test.

use crate::{EditValue, Scalar, parse, set};

/// Parsing arbitrary bytes never panics.
pub fn parse_bytes(data: &[u8]) {
    if let Ok(content) = std::str::from_utf8(data) {
        let _ = parse(content);
    }
}

/// Every successful edit yields a document that parses and holds the written value.
///
/// # Panics
///
/// Panics when an edit succeeds but its result does not satisfy that property.
pub fn set_round_trips(data: &[u8]) {
    let Ok(content) = std::str::from_utf8(data) else {
        return;
    };
    let edits = [
        (EditValue::Bool(true), Scalar::Bool(true)),
        (EditValue::Integer(-7), Scalar::Integer(-7)),
        (EditValue::Float(0.3), Scalar::Float(0.3)),
        (
            EditValue::String("a\"b\\c#e 'q'"),
            Scalar::String("a\"b\\c#e 'q'".to_owned()),
        ),
    ];
    for (edit, expected) in edits {
        let Ok(updated) = set(content, "fuzz", "key", edit) else {
            continue;
        };
        let document = parse(&updated).expect("edited document parses");
        let value = document
            .entries
            .iter()
            .find(|entry| document.tables[entry.table].name == "fuzz" && entry.key == "key")
            .map(|entry| &entry.value);
        assert_eq!(value, Some(&expected), "edited value round-trips");
    }
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::path::Path;

    /// Replays the `cargo fuzz` seed corpus on stable, so `ctest` covers it without nightly.
    #[test]
    fn corpus_satisfies_fuzz_properties() {
        let corpus = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../fuzz/corpus/config");
        let mut replayed = 0;
        for entry in fs::read_dir(&corpus).expect("corpus directory exists") {
            let data = fs::read(entry.expect("corpus entry").path()).expect("corpus file");
            super::parse_bytes(&data);
            super::set_round_trips(&data);
            replayed += 1;
        }
        assert!(replayed > 0, "corpus is not empty");
    }
}
