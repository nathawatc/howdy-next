//! Per-entry rules shared by decoding and encoding, so both enforce one rule set in one order:
//! id, metric, encoding shape, label, backend, model, values.

use crate::{
    Error, MAX_ENCODING_LEN, MAX_ENCODINGS_PER_MODEL, MAX_ID, MAX_METADATA_BYTES, MAX_MODELS,
    is_valid_label,
};

/// Checks the number of entries in a file.
pub(crate) fn check_count(count: usize) -> Result<(), Error> {
    match count {
        0 => Err(Error::NoModels),
        count if count > MAX_MODELS => Err(Error::TooManyModels),
        _ => Ok(()),
    }
}

/// Checks `id` against the limit and the previous entry's id.
pub(crate) fn check_id(id: u32, previous: Option<u32>) -> Result<(), Error> {
    if id > MAX_ID {
        return Err(Error::IdOutOfRange);
    }
    if previous.is_some_and(|previous| id <= previous) {
        return Err(Error::IdNotAscending);
    }
    Ok(())
}

/// Checks an entry's encoding count and length; returns the total number of values.
pub(crate) fn check_shape(count: usize, len: usize) -> Result<usize, Error> {
    if count == 0 {
        return Err(Error::NoEncodings);
    }
    if count > MAX_ENCODINGS_PER_MODEL {
        return Err(Error::TooManyEncodings);
    }
    if len == 0 || len > MAX_ENCODING_LEN {
        return Err(Error::EncodingLength);
    }
    // Both factors are bounded above, so this cannot overflow; stay explicit anyway.
    count.checked_mul(len).ok_or(Error::EncodingLength)
}

/// Checks a label and returns it as text.
pub(crate) fn check_label(label: &[u8]) -> Result<&str, Error> {
    if !is_valid_label(label) {
        return Err(Error::InvalidLabel);
    }
    std::str::from_utf8(label).map_err(|_| Error::InvalidLabel)
}

/// Checks a backend or model name (1 to [`MAX_METADATA_BYTES`] bytes of printable ASCII) and
/// returns it as text.
pub(crate) fn check_metadata(name: &[u8]) -> Result<&str, Error> {
    if name.is_empty() || name.len() > MAX_METADATA_BYTES || !name.iter().all(u8::is_ascii_graphic)
    {
        return Err(Error::InvalidMetadata);
    }
    std::str::from_utf8(name).map_err(|_| Error::InvalidMetadata)
}

/// Checks one encoding value.
pub(crate) fn check_value(value: f32) -> Result<(), Error> {
    if value.is_finite() {
        Ok(())
    } else {
        Err(Error::NonFiniteValue)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn count_limits() {
        assert_eq!(check_count(0), Err(Error::NoModels));
        assert_eq!(check_count(1), Ok(()));
        assert_eq!(check_count(MAX_MODELS), Ok(()));
        assert_eq!(check_count(MAX_MODELS + 1), Err(Error::TooManyModels));
    }

    #[test]
    fn id_limits_and_order() {
        assert_eq!(check_id(0, None), Ok(()));
        assert_eq!(check_id(MAX_ID, Some(0)), Ok(()));
        assert_eq!(check_id(MAX_ID + 1, None), Err(Error::IdOutOfRange));
        assert_eq!(check_id(5, Some(5)), Err(Error::IdNotAscending));
        assert_eq!(check_id(4, Some(5)), Err(Error::IdNotAscending));
    }

    #[test]
    fn shape_limits() {
        assert_eq!(check_shape(0, 1), Err(Error::NoEncodings));
        assert_eq!(check_shape(1, 0), Err(Error::EncodingLength));
        assert_eq!(
            check_shape(MAX_ENCODINGS_PER_MODEL, MAX_ENCODING_LEN),
            Ok(MAX_ENCODINGS_PER_MODEL * MAX_ENCODING_LEN)
        );
        assert_eq!(
            check_shape(MAX_ENCODINGS_PER_MODEL + 1, 1),
            Err(Error::TooManyEncodings)
        );
        assert_eq!(
            check_shape(1, MAX_ENCODING_LEN + 1),
            Err(Error::EncodingLength)
        );
    }

    #[test]
    fn metadata_rules() {
        assert_eq!(check_metadata(b"opencv_dnn_sface"), Ok("opencv_dnn_sface"));
        assert!(check_metadata(&[b'm'; MAX_METADATA_BYTES]).is_ok());
        assert_eq!(check_metadata(b""), Err(Error::InvalidMetadata));
        assert_eq!(
            check_metadata(&[b'm'; MAX_METADATA_BYTES + 1]),
            Err(Error::InvalidMetadata)
        );
        assert_eq!(check_metadata(b"a b"), Err(Error::InvalidMetadata));
        assert_eq!(check_metadata("é".as_bytes()), Err(Error::InvalidMetadata));
    }

    #[test]
    fn value_rules() {
        assert_eq!(check_value(-0.0), Ok(()));
        assert_eq!(check_value(f32::MAX), Ok(()));
        assert_eq!(check_value(f32::NAN), Err(Error::NonFiniteValue));
        assert_eq!(check_value(f32::INFINITY), Err(Error::NonFiniteValue));
        assert_eq!(check_value(f32::NEG_INFINITY), Err(Error::NonFiniteValue));
    }
}
