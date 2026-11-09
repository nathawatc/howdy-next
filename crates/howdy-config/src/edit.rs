use toml_edit::{DocumentMut, Item, RawString, Table, TableLike, Value};

use crate::parse::{UNSAFE_NAME, is_safe_name, parse_checked, unsafe_text};
use crate::{Error, ErrorKind, MAX_BYTES, is_safe_text};

/// A value to write. Config floats are `f32`; they are written in their shortest decimal form.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum EditValue<'a> {
    Bool(bool),
    Integer(i64),
    Float(f32),
    String(&'a str),
}

/// Sets `table.key = value` in `content` and returns the new document.
///
/// The document is rendered by `toml_edit`: line endings become LF and a byte-order mark is
/// dropped. An existing value is replaced in place and keeps its surrounding whitespace and
/// comment. A
/// missing key is appended to its table, and a missing table is appended after a blank line.
/// Comments at the top of a document without tables stay at the top.
///
/// # Errors
///
/// Returns the [`crate::parse()`] error when `content` is not a valid config document, a
/// [`ErrorKind::Structure`] error when `table`, `key`, or a string `value` would not pass
/// [`crate::parse()`] itself, and [`ErrorKind::TooLarge`] when the result would exceed [`MAX_BYTES`].
pub fn set(content: &str, table: &str, key: &str, value: EditValue<'_>) -> Result<String, Error> {
    check_target(table, key)?;
    if let EditValue::String(text) = value
        && !is_safe_text(text)
    {
        return Err(edit_error(unsafe_text(table, key)));
    }
    edit_document(content, |document| {
        let mut new_value = to_value(value);
        let root = document.as_table_mut();
        let has_tables = !root.is_empty();
        if let Some(existing_table) = root.get_mut(table) {
            let existing_table = table_like(existing_table, table)?;
            if let Some(existing) = existing_table.get_mut(key) {
                if let Some(old) = existing.as_value() {
                    *new_value.decor_mut() = old.decor().clone();
                }
                *existing = Item::Value(new_value);
            } else {
                existing_table.insert(key, Item::Value(new_value));
            }
            return Ok(());
        }

        let mut new_table = Table::new();
        new_table.insert(key, Item::Value(new_value));
        // Comments after the last item are document trailing text, which toml_edit prints after
        // every table. Keep them where they were, above the appended table's header, separated
        // by a blank line when the document already has tables.
        let mut leading = raw_text(document.trailing());
        if !leading.is_empty() && !leading.ends_with('\n') {
            // A final comment without a newline would otherwise swallow the table header.
            leading.push('\n');
        }
        if has_tables {
            leading.push('\n');
        }
        document.set_trailing("");
        new_table.decor_mut().set_prefix(leading);
        document
            .as_table_mut()
            .insert(table, Item::Table(new_table));
        Ok(())
    })
}

/// Parses and checks `content`, applies `apply`, and renders the result as `toml_edit` prints it.
fn edit_document(
    content: &str,
    apply: impl FnOnce(&mut DocumentMut) -> Result<(), Error>,
) -> Result<String, Error> {
    let mut document = parse_checked(content)?.into_mut();
    apply(&mut document)?;
    let edited = document.to_string();
    if edited.len() > MAX_BYTES {
        return Err(Error::too_large());
    }
    Ok(edited)
}

/// Rejects targets that [`crate::parse()`] would refuse in the edited document.
fn check_target(table: &str, key: &str) -> Result<(), Error> {
    if is_safe_name(table) && is_safe_name(key) {
        Ok(())
    } else {
        Err(edit_error(UNSAFE_NAME.to_owned()))
    }
}

fn edit_error(message: String) -> Error {
    Error {
        kind: ErrorKind::Structure,
        line: 0,
        column: 0,
        message,
    }
}

fn table_like<'a>(item: &'a mut Item, name: &str) -> Result<&'a mut dyn TableLike, Error> {
    // parse() already accepted only table-like root items; stay panic-free regardless.
    item.as_table_like_mut()
        .ok_or_else(|| edit_error(format!("\"{name}\" must be a table")))
}

fn raw_text(text: &RawString) -> String {
    text.as_str().unwrap_or_default().to_owned()
}

fn to_value(value: EditValue<'_>) -> Value {
    match value {
        EditValue::Bool(flag) => Value::from(flag),
        EditValue::Integer(number) => Value::from(number),
        EditValue::Float(number) => Value::from(shortest_f64(number)),
        EditValue::String(text) => Value::from(text),
    }
}

/// The `f64` nearest to the shortest decimal that round-trips `number`, so `0.3_f32` is written
/// as `0.3` rather than `0.30000001192092896`.
fn shortest_f64(number: f32) -> f64 {
    number
        .to_string()
        .parse()
        .unwrap_or_else(|_| f64::from(number))
}

#[cfg(test)]
mod tests {
    use super::{EditValue, set};
    use crate::{ErrorKind, Scalar, parse};

    fn value_of(content: &str, table: &str, key: &str) -> Option<Scalar> {
        let document = parse(content).expect("edited document parses");
        document
            .entries
            .iter()
            .find(|entry| document.tables[entry.table].name == table && entry.key == key)
            .map(|entry| entry.value.clone())
    }

    #[test]
    fn replaces_value_keeping_comments() {
        let content = "# head\n[core]\n# about disabled\ndisabled = false  # keep me\nother = 1\n";
        let updated = set(content, "core", "disabled", EditValue::Bool(true)).expect("set");
        assert_eq!(
            updated,
            "# head\n[core]\n# about disabled\ndisabled = true  # keep me\nother = 1\n"
        );
    }

    #[test]
    fn inserts_missing_key_into_existing_table() {
        let updated = set(
            "[video]\ntimeout = 4\n\n[face]\n",
            "video",
            "rotate",
            EditValue::Integer(90),
        )
        .expect("set");
        assert_eq!(
            value_of(&updated, "video", "rotate"),
            Some(Scalar::Integer(90))
        );
        assert_eq!(
            value_of(&updated, "video", "timeout"),
            Some(Scalar::Integer(4))
        );
        assert!(updated.find("rotate").unwrap() < updated.find("[face]").unwrap());
    }

    #[test]
    fn appends_missing_table_after_blank_line() {
        let updated = set(
            "[core]\ndisabled = false\n",
            "debug",
            "end_report",
            EditValue::Bool(true),
        )
        .expect("set");
        assert_eq!(
            updated,
            "[core]\ndisabled = false\n\n[debug]\nend_report = true\n"
        );
    }

    #[test]
    fn keeps_leading_comment_above_first_table() {
        let header = "# See howdy.toml(5) for configuration options.\n\n";
        let first = set(header, "core", "disabled", EditValue::Bool(false)).expect("set");
        let second = set(&first, "video", "max_height", EditValue::Float(320.0)).expect("set");
        assert_eq!(
            second,
            "# See howdy.toml(5) for configuration options.\n\n[core]\ndisabled = false\n\n\
             [video]\nmax_height = 320.0\n"
        );
    }

    #[test]
    fn edits_dotted_and_inline_tables() {
        let dotted = set(
            "core.disabled = false\n",
            "core",
            "disabled",
            EditValue::Bool(true),
        )
        .expect("set");
        assert_eq!(
            value_of(&dotted, "core", "disabled"),
            Some(Scalar::Bool(true))
        );
        let inline = set(
            "face = { sface_metric = \"cosine\" }\n",
            "face",
            "sface_threshold",
            EditValue::Float(0.3),
        )
        .expect("set");
        assert_eq!(
            value_of(&inline, "face", "sface_threshold"),
            Some(Scalar::Float(0.3))
        );
        assert_eq!(
            value_of(&inline, "face", "sface_metric"),
            Some(Scalar::String("cosine".to_owned()))
        );
    }

    #[test]
    fn writes_shortest_float_and_escaped_strings() {
        let updated =
            set("[face]\n", "face", "sface_threshold", EditValue::Float(0.3)).expect("set");
        assert!(updated.contains("sface_threshold = 0.3\n"), "{updated}");
        let path = set(
            "[video]\n",
            "video",
            "device_path",
            EditValue::String("/dev/\"q\"\\x"),
        )
        .expect("set");
        assert_eq!(
            value_of(&path, "video", "device_path"),
            Some(Scalar::String("/dev/\"q\"\\x".to_owned()))
        );
    }

    #[test]
    fn refuses_invalid_documents() {
        let error = set("[core\n", "core", "disabled", EditValue::Bool(true)).expect_err("syntax");
        assert_eq!(error.kind, ErrorKind::Syntax);
        let error = set(
            "[core]\nlist = [1]\n",
            "core",
            "disabled",
            EditValue::Bool(true),
        )
        .expect_err("structure");
        assert_eq!(error.kind, ErrorKind::Structure);
    }

    #[test]
    fn set_after_comment_without_newline() {
        let updated = set("# c", "core", "disabled", EditValue::Bool(true)).expect("set");
        assert_eq!(updated, "# c\n[core]\ndisabled = true\n");
    }

    #[test]
    fn refuses_edits_that_parse_would_reject() {
        let bad_name = set("", "a\u{7}", "k", EditValue::Bool(true)).expect_err("unsafe table");
        assert_eq!(bad_name.kind, ErrorKind::Structure);
        let bad_text = set(
            "",
            "video",
            "device_path",
            EditValue::String("/dev/\u{202e}x"),
        )
        .expect_err("unsafe string");
        assert_eq!(bad_text.kind, ErrorKind::Structure);
        let near_limit = format!("[core]\nx = 1\n#{}", " ".repeat(crate::MAX_BYTES - 15));
        let grown = set(&near_limit, "core", "y", EditValue::Bool(true)).expect_err("too large");
        assert_eq!(grown.kind, ErrorKind::TooLarge);
    }

    #[test]
    fn appended_table_keeps_trailing_comment_above_it() {
        let updated = set("[core]\n# c", "video", "timeout", EditValue::Integer(3)).expect("set");
        assert_eq!(updated, "[core]\n# c\n\n[video]\ntimeout = 3\n");
    }
}
