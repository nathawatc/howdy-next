//! Howdy binary user-model files (`<user>.model`): layout, limits, label rules, encoding, and
//! decoding.
//!
//! The crate owns every fact about the byte format and does no I/O: C++ opens, locks, and
//! commits files. Backend and model compatibility is policy and stays in C++.
//!
//! Layout (format version 1, all integers little-endian):
//!
//! ```text
//! header   magic "HOWDYMDL" | format_version u16 | entry_count u16
//! entry    id u32 | time i64 | metric u8 | encoding_count u8 | encoding_len u16
//!          | label_len u8, label | backend_len u8, backend | model_len u8, model
//!          | encoding_count * encoding_len f32 values
//! trailer  CRC32C u32 over every preceding byte
//! ```
//!
//! Entry ids are strictly ascending. Encoding is canonical: `encode(decode(b)) == b` for every
//! accepted `b`.
#![forbid(unsafe_code)]

mod decode;
mod encode;
mod entry;
#[cfg(any(test, feature = "fuzzing"))]
pub mod fuzzing;
mod label;

pub use decode::decode;
pub use encode::{EntryRef, encode};
pub use label::{is_valid_label, label_prefix_len};

/// First bytes of every model file.
pub const MAGIC: [u8; 8] = *b"HOWDYMDL";
/// The only format version this crate reads and writes.
pub const FORMAT_VERSION: u16 = 1;
/// Largest model file, in bytes, that decoding and encoding accept.
pub const MAX_FILE_BYTES: usize = 1024 * 1024;
/// Most entries one file holds.
pub const MAX_MODELS: usize = 256;
/// Most encodings one entry holds.
pub const MAX_ENCODINGS_PER_MODEL: usize = 32;
/// Most values one encoding holds.
pub const MAX_ENCODING_LEN: usize = 1024;
/// Longest label, in bytes.
pub const MAX_LABEL_BYTES: usize = 24;
/// Longest backend or model name, in bytes.
pub const MAX_METADATA_BYTES: usize = 64;
/// Largest entry id; one below `i32::MAX`, so the next id still fits a C++ `int`.
pub const MAX_ID: u32 = 0x7FFF_FFFE;

/// Bytes before the first entry: magic, format version, entry count.
pub(crate) const HEADER_LEN: usize = MAGIC.len() + 2 + 2;
/// Bytes of the CRC32C trailer.
pub(crate) const CHECKSUM_LEN: usize = 4;

const CRC32C: crc::Crc<u32> = crc::Crc::<u32>::new(&crc::CRC_32_ISCSI);

/// CRC32C of `data`. It detects corruption; it does not authenticate.
pub(crate) fn checksum(data: &[u8]) -> u32 {
    CRC32C.checksum(data)
}

/// Distance metric an entry's encodings were enrolled for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Metric {
    Cosine = 1,
    L2 = 2,
    L2Norm = 3,
}

impl Metric {
    /// The metric stored as `code`, if any.
    #[must_use]
    pub fn from_code(code: u8) -> Option<Self> {
        match code {
            1 => Some(Self::Cosine),
            2 => Some(Self::L2),
            3 => Some(Self::L2Norm),
            _ => None,
        }
    }

    /// The byte stored for this metric.
    #[must_use]
    pub fn code(self) -> u8 {
        self as u8
    }
}

/// One decoded model entry. Its encodings are stored flat: `values` holds
/// `encoding_count() * encoding_len` values.
#[derive(Clone, Debug, PartialEq)]
pub struct Entry {
    pub id: u32,
    pub time: i64,
    pub metric: Metric,
    pub label: String,
    pub backend: String,
    pub model: String,
    pub encoding_len: usize,
    pub values: Vec<f32>,
}

impl Entry {
    /// Number of encodings in the entry.
    #[must_use]
    pub fn encoding_count(&self) -> usize {
        self.values.len() / self.encoding_len
    }

    /// The encodings, each `encoding_len` values long.
    #[must_use]
    pub fn encodings(&self) -> Vec<&[f32]> {
        self.values.chunks_exact(self.encoding_len).collect()
    }
}

/// Why decoding or encoding failed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// The file exceeds [`MAX_FILE_BYTES`].
    TooLarge,
    /// The file ends inside the header, an entry, or the trailer.
    Truncated,
    /// The file does not start with [`MAGIC`].
    BadMagic,
    /// The format version is not [`FORMAT_VERSION`].
    UnsupportedVersion,
    /// The CRC32C trailer does not match the content.
    ChecksumMismatch,
    /// Bytes follow the last entry.
    TrailingBytes,
    /// The file holds no entries.
    NoModels,
    /// The file holds more than [`MAX_MODELS`] entries.
    TooManyModels,
    /// An id exceeds [`MAX_ID`].
    IdOutOfRange,
    /// An id is not greater than the one before it (duplicate or out of order).
    IdNotAscending,
    /// The metric code is unknown.
    InvalidMetric,
    /// A label breaks the label rules (see [`is_valid_label`]).
    InvalidLabel,
    /// A backend or model name is empty, too long, or not printable ASCII.
    InvalidMetadata,
    /// An entry holds no encodings.
    NoEncodings,
    /// An entry holds more than [`MAX_ENCODINGS_PER_MODEL`] encodings.
    TooManyEncodings,
    /// An encoding is empty or longer than [`MAX_ENCODING_LEN`].
    EncodingLength,
    /// Encodings of one entry differ in length.
    EncodingLengthMismatch,
    /// A value is NaN or infinite.
    NonFiniteValue,
    /// Encoded bytes did not decode back to the input; an encoder defect.
    EncodeMismatch,
}
