//! `config.toml` parse and edit exports backed by `howdy-config`.

use howdy_config::{Document, EditValue, Error, ErrorKind, Scalar};

use super::{HowdyStr, write_out};
use crate::HowdyFfiStatus;
use crate::boundary::guard;

/// Category of a config document error.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HowdyConfigErrorKind {
    /// The text is not valid TOML.
    Syntax = 0,
    /// The text is valid TOML but not a table-of-scalars config document.
    Structure = 1,
    /// The text exceeds [`HOWDY_CONFIG_MAX_BYTES`].
    TooLarge = 2,
}

/// Largest config text, in bytes, that parsing and editing accept. Spelled as a literal because
/// cbindgen cannot evaluate a path into another crate; the assertion keeps it equal to
/// `howdy_config::MAX_BYTES`, which the parser enforces.
pub const HOWDY_CONFIG_MAX_BYTES: usize = 1_048_576;
const _: () = assert!(HOWDY_CONFIG_MAX_BYTES == howdy_config::MAX_BYTES);

/// A parse or edit error; `message` is borrowed from the handle that reported it.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct HowdyConfigError {
    pub kind: HowdyConfigErrorKind,
    pub line: usize,
    pub column: usize,
    pub message: HowdyStr,
}

/// Type of a scalar config value.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HowdyConfigValueKind {
    Boolean = 0,
    Integer = 1,
    Float = 2,
    String = 3,
}

impl HowdyConfigValueKind {
    /// The kind whose discriminant is `raw`, if any.
    fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(Self::Boolean),
            1 => Some(Self::Integer),
            2 => Some(Self::Float),
            3 => Some(Self::String),
            _ => None,
        }
    }
}

/// A top-level table; `name` is borrowed from the document handle.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct HowdyConfigTable {
    pub name: HowdyStr,
    pub line: usize,
    pub column: usize,
}

/// One scalar entry; only the field matching `kind` is meaningful. Strings are borrowed from the
/// document handle.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct HowdyConfigEntry {
    pub table_index: usize,
    pub key: HowdyStr,
    pub kind: HowdyConfigValueKind,
    pub boolean: bool,
    pub integer: i64,
    pub floating: f64,
    pub string: HowdyStr,
    pub line: usize,
    pub column: usize,
}

/// A `table.key = value` edit; only the field matching `kind` is read.
///
/// `kind` holds a [`HowdyConfigValueKind`] value and `boolean` is 0 or 1. Both use plain integer
/// types because the struct is read from C memory: a Rust enum or `bool` with an invalid bit
/// pattern would be undefined behaviour before any check could run.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct HowdyConfigEdit {
    pub table: HowdyStr,
    pub key: HowdyStr,
    pub kind: u32,
    pub boolean: u8,
    pub integer: i64,
    pub floating: f32,
    pub string: HowdyStr,
}

/// Parsed config document or its error. Released by [`howdy_config_document_free`].
pub struct HowdyConfigDocument {
    result: Result<Document, Error>,
}

/// New config text or the edit error from [`howdy_config_set`]. Released by
/// [`howdy_config_edit_result_free`].
pub struct HowdyConfigEditResult {
    result: Result<String, Error>,
}

fn error_view(error: &Error) -> HowdyConfigError {
    HowdyConfigError {
        kind: match error.kind {
            ErrorKind::Syntax => HowdyConfigErrorKind::Syntax,
            ErrorKind::Structure => HowdyConfigErrorKind::Structure,
            ErrorKind::TooLarge => HowdyConfigErrorKind::TooLarge,
        },
        line: error.line,
        column: error.column,
        message: HowdyStr::from_str(&error.message),
    }
}

/// Borrows `text` as UTF-8 after checking its length, so oversized input is refused before any
/// byte is read.
///
/// # Safety
///
/// Same contract as [`HowdyStr::bytes`].
unsafe fn utf8<'a>(text: HowdyStr) -> Result<&'a str, HowdyFfiStatus> {
    if text.len > HOWDY_CONFIG_MAX_BYTES {
        return Err(HowdyFfiStatus::TooLarge);
    }
    // SAFETY: forwarded caller contract.
    let bytes = unsafe { text.bytes() }.ok_or(HowdyFfiStatus::InvalidArgument)?;
    std::str::from_utf8(bytes).map_err(|_| HowdyFfiStatus::InvalidUtf8)
}

/// Reports whether `result` failed, writing the error view when it did.
///
/// # Safety
///
/// `out_has_error` and `out_error` must be null or valid for one write each.
unsafe fn report_error<T>(
    result: &Result<T, Error>,
    out_has_error: *mut bool,
    out_error: *mut HowdyConfigError,
) -> HowdyFfiStatus {
    if out_has_error.is_null() || out_error.is_null() {
        return HowdyFfiStatus::InvalidArgument;
    }
    // SAFETY: both pointers are non-null and valid for writes per the caller contract.
    unsafe {
        if let Err(error) = result {
            write_out(out_error, error_view(error));
        }
        write_out(out_has_error, result.is_err())
    }
}

/// Parses `content` into a new document handle stored in `out`.
///
/// The handle exists whenever this returns `Ok`, including for invalid TOML; read the outcome
/// with [`howdy_config_document_error`].
///
/// # Safety
///
/// `content` follows the [`HowdyStr`] contract and `out` must be valid for one write.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn howdy_config_parse(
    content: HowdyStr,
    out: *mut *mut HowdyConfigDocument,
) -> HowdyFfiStatus {
    guard(|| {
        if out.is_null() {
            return HowdyFfiStatus::InvalidArgument;
        }
        // SAFETY: forwarded caller contract.
        let content = match unsafe { utf8(content) } {
            Ok(content) => content,
            Err(status) => return status,
        };
        let handle = Box::new(HowdyConfigDocument {
            result: howdy_config::parse(content),
        });
        // SAFETY: `out` is non-null and valid for writes per the caller contract.
        unsafe { write_out(out, Box::into_raw(handle)) }
    })
}

/// Writes whether `document` holds a parse error and, if so, the error.
///
/// # Safety
///
/// `document` must be a live handle from [`howdy_config_parse`]; the out pointers must be valid
/// for one write each. The message stays valid until the handle is freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn howdy_config_document_error(
    document: *const HowdyConfigDocument,
    out_has_error: *mut bool,
    out_error: *mut HowdyConfigError,
) -> HowdyFfiStatus {
    guard(|| {
        // SAFETY: the caller passes null or a live handle.
        let Some(document) = (unsafe { document.as_ref() }) else {
            return HowdyFfiStatus::InvalidArgument;
        };
        // SAFETY: forwarded caller contract.
        unsafe { report_error(&document.result, out_has_error, out_error) }
    })
}

/// Writes the number of tables and entries in a successfully parsed `document`.
///
/// # Safety
///
/// `document` must be a live handle; the out pointers must be valid for one write each.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn howdy_config_document_counts(
    document: *const HowdyConfigDocument,
    out_tables: *mut usize,
    out_entries: *mut usize,
) -> HowdyFfiStatus {
    guard(|| {
        // SAFETY: the caller passes null or a live handle.
        let Some(Ok(parsed)) = (unsafe { document.as_ref() }).map(|handle| &handle.result) else {
            return HowdyFfiStatus::InvalidArgument;
        };
        if out_tables.is_null() || out_entries.is_null() {
            return HowdyFfiStatus::InvalidArgument;
        }
        // SAFETY: both pointers are non-null and valid for writes per the caller contract.
        unsafe {
            write_out(out_tables, parsed.tables.len());
            write_out(out_entries, parsed.entries.len())
        }
    })
}

/// Writes table `index` of a successfully parsed `document`.
///
/// # Safety
///
/// `document` must be a live handle and `out` valid for one write. Borrowed strings stay valid
/// until the handle is freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn howdy_config_document_table(
    document: *const HowdyConfigDocument,
    index: usize,
    out: *mut HowdyConfigTable,
) -> HowdyFfiStatus {
    guard(|| {
        // SAFETY: the caller passes null or a live handle.
        let Some(Ok(parsed)) = (unsafe { document.as_ref() }).map(|handle| &handle.result) else {
            return HowdyFfiStatus::InvalidArgument;
        };
        let Some(table) = parsed.tables.get(index) else {
            return HowdyFfiStatus::InvalidArgument;
        };
        let view = HowdyConfigTable {
            name: HowdyStr::from_str(&table.name),
            line: table.line,
            column: table.column,
        };
        // SAFETY: the caller guarantees `out` is null or valid for writes.
        unsafe { write_out(out, view) }
    })
}

/// Writes entry `index` of a successfully parsed `document`.
///
/// # Safety
///
/// `document` must be a live handle and `out` valid for one write. Borrowed strings stay valid
/// until the handle is freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn howdy_config_document_entry(
    document: *const HowdyConfigDocument,
    index: usize,
    out: *mut HowdyConfigEntry,
) -> HowdyFfiStatus {
    guard(|| {
        // SAFETY: the caller passes null or a live handle.
        let Some(Ok(parsed)) = (unsafe { document.as_ref() }).map(|handle| &handle.result) else {
            return HowdyFfiStatus::InvalidArgument;
        };
        let Some(entry) = parsed.entries.get(index) else {
            return HowdyFfiStatus::InvalidArgument;
        };
        let mut view = HowdyConfigEntry {
            table_index: entry.table,
            key: HowdyStr::from_str(&entry.key),
            kind: HowdyConfigValueKind::Boolean,
            boolean: false,
            integer: 0,
            floating: 0.0,
            string: HowdyStr::from_str(""),
            line: entry.line,
            column: entry.column,
        };
        match &entry.value {
            Scalar::Bool(flag) => view.boolean = *flag,
            Scalar::Integer(number) => {
                view.kind = HowdyConfigValueKind::Integer;
                view.integer = *number;
            }
            Scalar::Float(number) => {
                view.kind = HowdyConfigValueKind::Float;
                view.floating = *number;
            }
            Scalar::String(text) => {
                view.kind = HowdyConfigValueKind::String;
                view.string = HowdyStr::from_str(text);
            }
        }
        // SAFETY: the caller guarantees `out` is null or valid for writes.
        unsafe { write_out(out, view) }
    })
}

/// Releases a document handle; null is ignored.
///
/// # Safety
///
/// `document` must be null or a handle from [`howdy_config_parse`] not yet freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn howdy_config_document_free(document: *mut HowdyConfigDocument) {
    let _ = guard(|| {
        if !document.is_null() {
            // SAFETY: the handle came from `Box::into_raw` and is freed exactly once.
            drop(unsafe { Box::from_raw(document) });
        }
        HowdyFfiStatus::Ok
    });
}

/// Applies `edit` to `content` and stores the outcome in a new handle in `out`.
///
/// The handle exists whenever this returns `Ok`; read it with
/// [`howdy_config_edit_result_error`] and [`howdy_config_edit_result_content`].
///
/// # Safety
///
/// `content` and the strings in `edit` follow the [`HowdyStr`] contract; `edit` must be valid for
/// one read and `out` for one write.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn howdy_config_set(
    content: HowdyStr,
    edit: *const HowdyConfigEdit,
    out: *mut *mut HowdyConfigEditResult,
) -> HowdyFfiStatus {
    guard(|| {
        // SAFETY: the caller passes null or a valid edit.
        let Some(edit) = (unsafe { edit.as_ref() }) else {
            return HowdyFfiStatus::InvalidArgument;
        };
        if out.is_null() {
            return HowdyFfiStatus::InvalidArgument;
        }
        // SAFETY: forwarded caller contract for every borrowed string.
        let (content, table, key) =
            match unsafe { (utf8(content), utf8(edit.table), utf8(edit.key)) } {
                (Ok(content), Ok(table), Ok(key)) => (content, table, key),
                (Err(status), _, _) | (_, Err(status), _) | (_, _, Err(status)) => return status,
            };
        let value = match (HowdyConfigValueKind::from_raw(edit.kind), edit.boolean) {
            (Some(HowdyConfigValueKind::Boolean), 0 | 1) => EditValue::Bool(edit.boolean == 1),
            (Some(HowdyConfigValueKind::Integer), _) => EditValue::Integer(edit.integer),
            (Some(HowdyConfigValueKind::Float), _) => EditValue::Float(edit.floating),
            // SAFETY: forwarded caller contract.
            (Some(HowdyConfigValueKind::String), _) => match unsafe { utf8(edit.string) } {
                Ok(text) => EditValue::String(text),
                Err(status) => return status,
            },
            (Some(HowdyConfigValueKind::Boolean) | None, _) => {
                return HowdyFfiStatus::InvalidArgument;
            }
        };
        let handle = Box::new(HowdyConfigEditResult {
            result: howdy_config::set(content, table, key, value),
        });
        // SAFETY: `out` is non-null and valid for writes per the caller contract.
        unsafe { write_out(out, Box::into_raw(handle)) }
    })
}

/// A scalar parsed from `howdy set` text; only the field matching `kind` is meaningful.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct HowdyConfigScalar {
    pub kind: HowdyConfigValueKind,
    pub boolean: bool,
    pub integer: i64,
    pub floating: f64,
}

/// Parses `text` as one TOML boolean, integer, or float literal. Writes whether it is one and,
/// if so, the scalar; strings are never produced.
///
/// # Safety
///
/// `text` follows the [`HowdyStr`] contract; the out pointers must be valid for one write each.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn howdy_config_parse_scalar(
    text: HowdyStr,
    out_valid: *mut bool,
    out: *mut HowdyConfigScalar,
) -> HowdyFfiStatus {
    guard(|| {
        if out_valid.is_null() || out.is_null() {
            return HowdyFfiStatus::InvalidArgument;
        }
        // SAFETY: forwarded caller contract.
        let text = match unsafe { utf8(text) } {
            Ok(text) => text,
            Err(status) => return status,
        };
        let mut scalar = HowdyConfigScalar {
            kind: HowdyConfigValueKind::Boolean,
            boolean: false,
            integer: 0,
            floating: 0.0,
        };
        let valid = match howdy_config::parse_scalar(text) {
            Some(Scalar::Bool(flag)) => {
                scalar.boolean = flag;
                true
            }
            Some(Scalar::Integer(number)) => {
                scalar.kind = HowdyConfigValueKind::Integer;
                scalar.integer = number;
                true
            }
            Some(Scalar::Float(number)) => {
                scalar.kind = HowdyConfigValueKind::Float;
                scalar.floating = number;
                true
            }
            Some(Scalar::String(_)) | None => false,
        };
        // SAFETY: both pointers are non-null and valid for writes per the caller contract.
        unsafe {
            write_out(out, scalar);
            write_out(out_valid, valid)
        }
    })
}

/// Writes whether `text` may appear in a config string value (see `howdy_config::is_safe_text`).
///
/// # Safety
///
/// `text` follows the [`HowdyStr`] contract and `out_safe` must be valid for one write.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn howdy_config_is_safe_text(
    text: HowdyStr,
    out_safe: *mut bool,
) -> HowdyFfiStatus {
    guard(|| {
        // SAFETY: forwarded caller contract.
        let safe = match unsafe { utf8(text) } {
            Ok(text) => howdy_config::is_safe_text(text),
            Err(HowdyFfiStatus::InvalidUtf8) => false,
            Err(status) => return status,
        };
        // SAFETY: the caller guarantees `out_safe` is null or valid for writes.
        unsafe { write_out(out_safe, safe) }
    })
}

/// Writes whether `result` holds an edit error and, if so, the error.
///
/// # Safety
///
/// `result` must be a live handle from [`howdy_config_set`]; the out pointers must be valid for
/// one write each. The message stays valid until the handle is freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn howdy_config_edit_result_error(
    result: *const HowdyConfigEditResult,
    out_has_error: *mut bool,
    out_error: *mut HowdyConfigError,
) -> HowdyFfiStatus {
    guard(|| {
        // SAFETY: the caller passes null or a live handle.
        let Some(result) = (unsafe { result.as_ref() }) else {
            return HowdyFfiStatus::InvalidArgument;
        };
        // SAFETY: forwarded caller contract.
        unsafe { report_error(&result.result, out_has_error, out_error) }
    })
}

/// Writes the edited content of a successful `result`.
///
/// # Safety
///
/// `result` must be a live handle and `out` valid for one write. The content stays valid until
/// the handle is freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn howdy_config_edit_result_content(
    result: *const HowdyConfigEditResult,
    out: *mut HowdyStr,
) -> HowdyFfiStatus {
    guard(|| {
        // SAFETY: the caller passes null or a live handle.
        let Some(Ok(content)) = (unsafe { result.as_ref() }).map(|handle| &handle.result) else {
            return HowdyFfiStatus::InvalidArgument;
        };
        // SAFETY: the caller guarantees `out` is null or valid for writes.
        unsafe { write_out(out, HowdyStr::from_str(content)) }
    })
}

/// Releases an edit result handle; null is ignored.
///
/// # Safety
///
/// `result` must be null or a handle from [`howdy_config_set`] not yet freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn howdy_config_edit_result_free(result: *mut HowdyConfigEditResult) {
    let _ = guard(|| {
        if !result.is_null() {
            // SAFETY: the handle came from `Box::into_raw` and is freed exactly once.
            drop(unsafe { Box::from_raw(result) });
        }
        HowdyFfiStatus::Ok
    });
}

#[cfg(test)]
mod tests {
    use std::ptr;

    use super::*;

    fn text(value: &str) -> HowdyStr {
        HowdyStr::from_str(value)
    }

    unsafe fn view(value: HowdyStr) -> &'static str {
        // SAFETY: test views point into handles that outlive the assertion.
        std::str::from_utf8(unsafe { value.bytes() }.expect("bytes")).expect("utf8")
    }

    #[test]
    fn parses_tables_and_entries() {
        let mut handle = ptr::null_mut();
        // SAFETY: valid input and out pointer.
        let status =
            unsafe { howdy_config_parse(text("[core]\ndisabled = true\n"), &raw mut handle) };
        assert_eq!(status, HowdyFfiStatus::Ok);

        let (mut has_error, mut error) = (
            true,
            HowdyConfigError {
                kind: HowdyConfigErrorKind::Syntax,
                line: 0,
                column: 0,
                message: text(""),
            },
        );
        let (mut tables, mut entries) = (0, 0);
        let mut entry = HowdyConfigEntry {
            table_index: 9,
            key: text(""),
            kind: HowdyConfigValueKind::String,
            boolean: false,
            integer: 0,
            floating: 0.0,
            string: text(""),
            line: 0,
            column: 0,
        };
        // SAFETY: live handle and valid out pointers.
        unsafe {
            assert_eq!(
                howdy_config_document_error(handle, &raw mut has_error, &raw mut error),
                HowdyFfiStatus::Ok
            );
            assert!(!has_error);
            assert_eq!(
                howdy_config_document_counts(handle, &raw mut tables, &raw mut entries),
                HowdyFfiStatus::Ok
            );
            assert_eq!((tables, entries), (1, 1));
            assert_eq!(
                howdy_config_document_entry(handle, 0, &raw mut entry),
                HowdyFfiStatus::Ok
            );
            assert_eq!(view(entry.key), "disabled");
            assert_eq!(entry.kind, HowdyConfigValueKind::Boolean);
            assert!(entry.boolean);
            assert_eq!(
                howdy_config_document_entry(handle, 1, &raw mut entry),
                HowdyFfiStatus::InvalidArgument
            );
            howdy_config_document_free(handle);
        }
    }

    #[test]
    fn reports_parse_errors_through_handle() {
        let mut handle = ptr::null_mut();
        let mut has_error = false;
        let mut error = HowdyConfigError {
            kind: HowdyConfigErrorKind::Syntax,
            line: 0,
            column: 0,
            message: text(""),
        };
        let (mut tables, mut entries) = (0, 0);
        // SAFETY: valid input, live handle, and valid out pointers.
        unsafe {
            assert_eq!(
                howdy_config_parse(text("x = 1\n"), &raw mut handle),
                HowdyFfiStatus::Ok
            );
            howdy_config_document_error(handle, &raw mut has_error, &raw mut error);
            assert!(has_error);
            assert_eq!(error.kind, HowdyConfigErrorKind::Structure);
            assert_eq!((error.line, error.column), (1, 1));
            assert_ne!(view(error.message), "");
            assert_eq!(
                howdy_config_document_counts(handle, &raw mut tables, &raw mut entries),
                HowdyFfiStatus::InvalidArgument
            );
            howdy_config_document_free(handle);
        }
    }

    #[test]
    fn rejects_invalid_arguments() {
        let mut handle = ptr::null_mut();
        let invalid = [0xff_u8];
        // SAFETY: deliberately invalid but memory-safe arguments.
        unsafe {
            assert_eq!(
                howdy_config_parse(
                    HowdyStr {
                        ptr: invalid.as_ptr().cast(),
                        len: 1
                    },
                    &raw mut handle
                ),
                HowdyFfiStatus::InvalidUtf8
            );
            assert_eq!(
                howdy_config_parse(
                    HowdyStr {
                        ptr: ptr::null(),
                        len: 3
                    },
                    &raw mut handle
                ),
                HowdyFfiStatus::InvalidArgument
            );
            assert_eq!(
                howdy_config_parse(
                    HowdyStr {
                        ptr: ptr::null(),
                        len: 0
                    },
                    ptr::null_mut()
                ),
                HowdyFfiStatus::InvalidArgument
            );
            assert!(handle.is_null());
            howdy_config_document_free(ptr::null_mut());
            howdy_config_edit_result_free(ptr::null_mut());
        }
    }

    #[test]
    fn sets_values_and_reports_edit_errors() {
        let edit = HowdyConfigEdit {
            table: text("core"),
            key: text("disabled"),
            kind: HowdyConfigValueKind::Boolean as u32,
            boolean: 1,
            integer: 0,
            floating: 0.0,
            string: HowdyStr {
                ptr: ptr::null(),
                len: 0,
            },
        };
        let mut handle = ptr::null_mut();
        let mut has_error = true;
        let mut error = HowdyConfigError {
            kind: HowdyConfigErrorKind::Syntax,
            line: 0,
            column: 0,
            message: text(""),
        };
        let mut content = text("");
        // SAFETY: valid inputs, live handles, and valid out pointers.
        unsafe {
            assert_eq!(
                howdy_config_set(
                    text("[core]\ndisabled = false # c\n"),
                    &raw const edit,
                    &raw mut handle
                ),
                HowdyFfiStatus::Ok
            );
            howdy_config_edit_result_error(handle, &raw mut has_error, &raw mut error);
            assert!(!has_error);
            assert_eq!(
                howdy_config_edit_result_content(handle, &raw mut content),
                HowdyFfiStatus::Ok
            );
            assert_eq!(view(content), "[core]\ndisabled = true # c\n");
            howdy_config_edit_result_free(handle);

            assert_eq!(
                howdy_config_set(text("[core\n"), &raw const edit, &raw mut handle),
                HowdyFfiStatus::Ok
            );
            howdy_config_edit_result_error(handle, &raw mut has_error, &raw mut error);
            assert!(has_error);
            assert_eq!(error.kind, HowdyConfigErrorKind::Syntax);
            assert_eq!(
                howdy_config_edit_result_content(handle, &raw mut content),
                HowdyFfiStatus::InvalidArgument
            );
            howdy_config_edit_result_free(handle);
        }
    }

    #[test]
    fn rejects_invalid_edit_discriminants() {
        let mut handle = ptr::null_mut();
        for (kind, boolean) in [(7_u32, 0_u8), (HowdyConfigValueKind::Boolean as u32, 2)] {
            let edit = HowdyConfigEdit {
                table: text("core"),
                key: text("disabled"),
                kind,
                boolean,
                integer: 0,
                floating: 0.0,
                string: text(""),
            };
            // SAFETY: valid pointers; the discriminants are deliberately invalid.
            let status =
                unsafe { howdy_config_set(text("[core]\n"), &raw const edit, &raw mut handle) };
            assert_eq!(status, HowdyFfiStatus::InvalidArgument);
            assert!(handle.is_null());
        }
    }

    #[test]
    fn parses_scalars_and_checks_text() {
        let mut valid = false;
        let mut scalar = HowdyConfigScalar {
            kind: HowdyConfigValueKind::String,
            boolean: false,
            integer: 0,
            floating: 0.0,
        };
        let mut safe = true;
        // SAFETY: valid inputs and out pointers.
        unsafe {
            assert_eq!(
                howdy_config_parse_scalar(text("1_000"), &raw mut valid, &raw mut scalar),
                HowdyFfiStatus::Ok
            );
            assert!(valid);
            assert_eq!(scalar.kind, HowdyConfigValueKind::Integer);
            assert_eq!(scalar.integer, 1000);
            howdy_config_parse_scalar(text("yes"), &raw mut valid, &raw mut scalar);
            assert!(!valid);
            assert_eq!(
                howdy_config_is_safe_text(text("/dev/\u{202e}"), &raw mut safe),
                HowdyFfiStatus::Ok
            );
            assert!(!safe);
            let oversized = HowdyStr {
                ptr: ptr::null(),
                len: HOWDY_CONFIG_MAX_BYTES + 1,
            };
            let mut handle = ptr::null_mut();
            assert_eq!(
                howdy_config_parse(oversized, &raw mut handle),
                HowdyFfiStatus::TooLarge
            );
        }
    }
}
