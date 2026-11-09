use toml_edit::{Item, Value};

use crate::position::{Positions, line_column};
use crate::{Error, ErrorKind, MAX_BYTES};

/// A scalar config value as written in TOML.
#[derive(Clone, Debug, PartialEq)]
pub enum Scalar {
    Bool(bool),
    Integer(i64),
    Float(f64),
    String(String),
}

/// A top-level table, in document order.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Table {
    pub name: String,
    pub line: usize,
    pub column: usize,
}

/// One `key = value` inside a table; `table` indexes [`Document::tables`].
#[derive(Clone, Debug, PartialEq)]
pub struct Entry {
    pub table: usize,
    pub key: String,
    pub value: Scalar,
    pub line: usize,
    pub column: usize,
}

/// A config document flattened to tables and their scalar entries.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Document {
    pub tables: Vec<Table>,
    pub entries: Vec<Entry>,
}

/// Parses `content` into tables of scalars.
///
/// Header tables, dotted keys, and inline tables are equivalent spellings and all accepted.
/// Root-level values, nested tables, arrays, and datetimes are structure errors, and so are
/// names that are not printable ASCII or string values that fail [`is_safe_text`].
///
/// # Errors
///
/// Returns [`ErrorKind::TooLarge`] beyond [`MAX_BYTES`], [`ErrorKind::Syntax`] for invalid TOML,
/// and [`ErrorKind::Structure`] for documents that are not tables of safe scalars.
pub fn parse(content: &str) -> Result<Document, Error> {
    let parsed = parse_toml(content)?;
    let mut document = Document::default();
    walk(content, &parsed, Some(&mut document))?;
    Ok(document)
}

/// Parses and checks `content` without collecting it, for editors that reuse the parsed TOML.
pub(crate) fn parse_checked(content: &str) -> Result<toml_edit::Document<&str>, Error> {
    let parsed = parse_toml(content)?;
    walk(content, &parsed, None)?;
    Ok(parsed)
}

fn parse_toml(content: &str) -> Result<toml_edit::Document<&str>, Error> {
    if content.len() > MAX_BYTES {
        return Err(Error::too_large());
    }
    toml_edit::Document::parse(content).map_err(|error| {
        let (line, column) = line_column(content, error.span());
        Error {
            kind: ErrorKind::Syntax,
            line,
            column,
            message: error.message().trim_end().to_owned(),
        }
    })
}

/// Checks the table-of-safe-scalars shape, collecting into `sink` when one is given.
fn walk(
    content: &str,
    parsed: &toml_edit::Document<&str>,
    mut sink: Option<&mut Document>,
) -> Result<(), Error> {
    let root = parsed.as_table();
    let mut positions = Positions::new(content);
    for (name, item) in root {
        let span = root
            .key(name)
            .and_then(toml_edit::Key::span)
            .or_else(|| item.span());
        if !is_safe_name(name) {
            return Err(structure(content, span, UNSAFE_NAME.to_owned()));
        }
        let Some(table) = item.as_table_like() else {
            return Err(structure(
                content,
                span,
                format!("\"{name}\" must be a table"),
            ));
        };
        let table_index = sink.as_deref_mut().map(|document| {
            let (line, column) = positions.at(span.clone());
            document.tables.push(Table {
                name: name.to_owned(),
                line,
                column,
            });
            document.tables.len() - 1
        });
        for (key, item) in table.iter() {
            let span = table
                .key(key)
                .and_then(toml_edit::Key::span)
                .or_else(|| item.span());
            if !is_safe_name(key) {
                return Err(structure(content, span, UNSAFE_NAME.to_owned()));
            }
            let Item::Value(value) = item else {
                return Err(structure(content, span, not_scalar(name, key)));
            };
            let checked = check_scalar(content, span.clone(), name, key, value)?;
            if let (Some(document), Some(table)) = (sink.as_deref_mut(), table_index) {
                let (line, column) = positions.at(span);
                document.entries.push(Entry {
                    table,
                    key: key.to_owned(),
                    value: checked.to_owned(),
                    line,
                    column,
                });
            }
        }
    }
    Ok(())
}

pub(crate) const UNSAFE_NAME: &str = "table and key names must be printable ASCII";

pub(crate) fn unsafe_text(table: &str, key: &str) -> String {
    format!("{table}.{key} must be printable ASCII")
}

fn not_scalar(table: &str, key: &str) -> String {
    format!("{table}.{key} must be a string, integer, float, or boolean")
}

/// A checked scalar borrowed from the parsed document.
enum ScalarRef<'a> {
    Bool(bool),
    Integer(i64),
    Float(f64),
    String(&'a str),
}

impl ScalarRef<'_> {
    fn to_owned(&self) -> Scalar {
        match *self {
            Self::Bool(flag) => Scalar::Bool(flag),
            Self::Integer(number) => Scalar::Integer(number),
            Self::Float(number) => Scalar::Float(number),
            Self::String(text) => Scalar::String(text.to_owned()),
        }
    }
}

fn check_scalar<'a>(
    content: &str,
    span: Option<std::ops::Range<usize>>,
    table: &str,
    key: &str,
    value: &'a Value,
) -> Result<ScalarRef<'a>, Error> {
    match value {
        Value::Integer(number) => Ok(ScalarRef::Integer(*number.value())),
        Value::Float(number) => Ok(ScalarRef::Float(*number.value())),
        Value::Boolean(flag) => Ok(ScalarRef::Bool(*flag.value())),
        Value::String(text) if is_safe_text(text.value()) => Ok(ScalarRef::String(text.value())),
        Value::String(_) => Err(structure(content, span, unsafe_text(table, key))),
        Value::Datetime(_) | Value::Array(_) | Value::InlineTable(_) => {
            Err(structure(content, span, not_scalar(table, key)))
        }
    }
}

/// Parses one TOML boolean, integer, or float literal, such as `true`, `+5`, `1_000`, `0x10`,
/// or `0.5`. Text that is not [`is_safe_text`] (including a byte-order mark), surrounding
/// whitespace, strings, and other values are rejected.
#[must_use]
pub fn parse_scalar(text: &str) -> Option<Scalar> {
    if text.len() > MAX_BYTES || !is_safe_text(text) || text.trim() != text {
        return None;
    }
    match text.parse::<Value>().ok()? {
        Value::Integer(number) => Some(Scalar::Integer(*number.value())),
        Value::Float(number) => Some(Scalar::Float(*number.value())),
        Value::Boolean(flag) => Some(Scalar::Bool(*flag.value())),
        Value::String(_) | Value::Datetime(_) | Value::Array(_) | Value::InlineTable(_) => None,
    }
}

/// Table and key names are printable ASCII: every schema name is a bare key, and anything else
/// would be echoed in diagnostics.
#[must_use]
pub(crate) fn is_safe_name(name: &str) -> bool {
    !name.is_empty() && name.bytes().all(|byte| byte.is_ascii_graphic())
}

/// String values are printable ASCII (space through `~`). Every schema string is a choice word
/// or a device path, and udev escapes non-ASCII bytes in `/dev/v4l` names, so this costs nothing
/// while ruling out control characters and every invisible or look-alike Unicode character that
/// could spoof a terminal, a log line, or a path.
#[must_use]
pub fn is_safe_text(text: &str) -> bool {
    text.bytes()
        .all(|byte| byte == b' ' || byte.is_ascii_graphic())
}

fn structure(content: &str, span: Option<std::ops::Range<usize>>, message: String) -> Error {
    let (line, column) = line_column(content, span);
    Error {
        kind: ErrorKind::Structure,
        line,
        column,
        message,
    }
}

#[cfg(test)]
mod tests {
    use super::{Scalar, parse};
    use crate::ErrorKind;

    #[test]
    fn flattens_header_dotted_and_inline_tables() {
        // Root-level dotted keys and inline tables must precede the first header.
        let document = parse(
            "# comment\nvideo.timeout = 4\n\
             face = { sface_metric = \"cosine\", sface_threshold = 0.5 }\n\
             [core]\ndisabled = true # trailing\n",
        )
        .expect("valid config");
        let names: Vec<_> = document
            .tables
            .iter()
            .map(|table| table.name.as_str())
            .collect();
        assert_eq!(names, ["video", "face", "core"]);
        let values: Vec<_> = document
            .entries
            .iter()
            .map(|entry| {
                (
                    document.tables[entry.table].name.as_str(),
                    entry.key.as_str(),
                    &entry.value,
                )
            })
            .collect();
        assert_eq!(
            values,
            [
                ("video", "timeout", &Scalar::Integer(4)),
                ("face", "sface_metric", &Scalar::String("cosine".to_owned())),
                ("face", "sface_threshold", &Scalar::Float(0.5)),
                ("core", "disabled", &Scalar::Bool(true)),
            ]
        );
        assert_eq!((document.tables[0].line, document.tables[0].column), (2, 1));
        assert_eq!(
            (document.entries[0].line, document.entries[0].column),
            (2, 7)
        );
        assert_eq!((document.tables[2].line, document.tables[2].column), (4, 2));
        assert_eq!(
            (document.entries[3].line, document.entries[3].column),
            (5, 1)
        );
    }

    #[test]
    fn keeps_empty_tables() {
        let document = parse("[debug]\n").expect("valid config");
        assert_eq!(document.tables.len(), 1);
        assert_eq!(document.entries, []);
    }

    #[test]
    fn reports_syntax_errors_with_position() {
        let error = parse("[core]\ndisabled = \n").expect_err("missing value");
        assert_eq!(error.kind, ErrorKind::Syntax);
        assert_eq!(error.line, 2);
        assert_ne!(error.message, "");
    }

    #[test]
    fn rejects_duplicate_keys() {
        let error = parse("[core]\ndisabled = true\ndisabled = false\n").expect_err("duplicate");
        assert_eq!(error.kind, ErrorKind::Syntax);
    }

    #[test]
    fn rejects_non_table_shapes() {
        for (content, line) in [
            ("disabled = true\n", 1),
            ("[core]\nlist = [1, 2]\n", 2),
            ("[core]\nwhen = 1979-05-27\n", 2),
            ("[core]\n[core.nested]\nkey = 1\n", 2),
            ("[core]\nnested = { key = 1 }\n", 2),
            ("[[core]]\nkey = 1\n", 1),
        ] {
            let error = parse(content).expect_err(content);
            assert_eq!(error.kind, ErrorKind::Structure, "{content}");
            assert_eq!(error.line, line, "{content}");
        }
    }

    #[test]
    fn decodes_string_escapes() {
        let document = parse("[video]\ndevice_path = \"/dev/v\\u0041\\\"q\"\n").expect("valid");
        assert_eq!(
            document.entries[0].value,
            Scalar::String("/dev/vA\"q".to_owned())
        );
    }

    #[test]
    fn rejects_unsafe_string_values() {
        for escaped in [
            "\\u0000", "\\n", "\\t", "\\u009b", "\\u202e", "\\u2028", "\\u200b",
        ] {
            let content = format!("[video]\ndevice_path = \"/dev/v{escaped}\"\n");
            let error = parse(&content).expect_err(&content);
            assert_eq!(error.kind, ErrorKind::Structure, "{content}");
            assert_eq!(error.line, 2, "{content}");
        }
    }

    #[test]
    fn rejects_non_ascii_names() {
        let error = parse("[core]\n\"\u{202e}x\" = 1\n").expect_err("bidi name");
        assert_eq!(error.kind, ErrorKind::Structure);
        assert!(!error.message.contains('\u{202e}'));
    }

    #[test]
    fn parses_toml_scalar_literals() {
        use super::parse_scalar;
        assert_eq!(parse_scalar("true"), Some(Scalar::Bool(true)));
        assert_eq!(parse_scalar("+5"), Some(Scalar::Integer(5)));
        assert_eq!(parse_scalar("1_000"), Some(Scalar::Integer(1000)));
        assert_eq!(parse_scalar("0x10"), Some(Scalar::Integer(16)));
        assert_eq!(parse_scalar("0.5"), Some(Scalar::Float(0.5)));
        for invalid in [
            "yes", "True", "1.", ".5", "+-1", " 5", "5 ", "\"x\"", "[1]", "",
        ] {
            assert_eq!(parse_scalar(invalid), None, "{invalid:?}");
        }
    }

    #[test]
    fn rejects_control_characters_in_names() {
        for content in ["[core]\n\"\\u001b[2J\" = 1\n", "[\"a\\u0007\"]\nx = 1\n"] {
            let error = parse(content).expect_err(content);
            assert_eq!(error.kind, ErrorKind::Structure, "{content}");
            assert!(!error.message.contains('\u{1b}') && !error.message.contains('\u{7}'));
        }
    }

    #[test]
    fn rejects_oversized_input() {
        let oversized = " ".repeat(crate::MAX_BYTES + 1);
        assert_eq!(
            parse(&oversized).expect_err("too large").kind,
            ErrorKind::TooLarge
        );
        assert!(parse(&" ".repeat(crate::MAX_BYTES)).is_ok());
    }

    #[test]
    fn parses_interleaved_dotted_tables_in_linear_time() {
        use std::fmt::Write as _;

        // Tables are walked in insertion order, so their entries are visited out of document
        // order; a forward-only position cursor rescanned the document for each table.
        let tables = 16_000;
        let mut content = String::new();
        for key in ["a", "b"] {
            for table in 0..tables {
                writeln!(content, "t{table}.{key}=1").expect("writing to a String cannot fail");
            }
        }
        let started = std::time::Instant::now();
        let document = parse(&content).expect("valid");
        assert_eq!(document.entries.len(), 2 * tables);
        let last_b = &document.entries[document.entries.len() - 1];
        assert_eq!((last_b.key.as_str(), last_b.line), ("b", 2 * tables));
        assert!(
            started.elapsed() < std::time::Duration::from_secs(20),
            "parse took {:?}",
            started.elapsed()
        );
    }

    #[test]
    fn parses_maximum_size_document_in_linear_time() {
        use std::fmt::Write as _;

        // About 116k entries: the old per-entry rescan from byte 0 took tens of seconds here.
        let mut content = String::from("[core]\n");
        let mut index = 0;
        while content.len() + 16 < crate::MAX_BYTES {
            writeln!(content, "k{index}=1").expect("writing to a String cannot fail");
            index += 1;
        }
        let started = std::time::Instant::now();
        let document = parse(&content).expect("valid");
        assert_eq!(document.entries.len(), index);
        assert_eq!(
            document.entries.last().map(|entry| entry.line),
            Some(index + 1)
        );
        assert!(
            started.elapsed() < std::time::Duration::from_secs(20),
            "parse took {:?}",
            started.elapsed()
        );
    }
}
