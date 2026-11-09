//! Howdy `config.toml` parsing and comment-preserving editing.
//!
//! The crate knows TOML and the config file's shape (tables holding scalars), never the option
//! schema: C++ checks table and key names, types, and ranges. It does no I/O.
#![forbid(unsafe_code)]

mod edit;
#[cfg(any(test, feature = "fuzzing"))]
pub mod fuzzing;
mod parse;
mod position;

pub use edit::{EditValue, set};
pub use parse::{Document, Entry, Scalar, Table, is_safe_text, parse, parse_scalar};

/// Largest config text, in bytes, that parsing and editing accept.
pub const MAX_BYTES: usize = 1024 * 1024;

/// Why parsing or editing failed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ErrorKind {
    /// The input is not valid TOML.
    Syntax,
    /// The input is valid TOML but not a table-of-scalars config document.
    Structure,
    /// The input exceeds [`MAX_BYTES`].
    TooLarge,
}

/// A parse or edit failure with a 1-based source position (0 when unknown).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Error {
    pub kind: ErrorKind,
    pub line: usize,
    pub column: usize,
    pub message: String,
}

impl Error {
    /// The error for input beyond [`MAX_BYTES`].
    #[must_use]
    pub fn too_large() -> Self {
        Self {
            kind: ErrorKind::TooLarge,
            line: 0,
            column: 0,
            message: "config exceeds maximum size".to_owned(),
        }
    }
}
