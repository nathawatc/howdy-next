//! Decoding model file bytes into entries.

use crate::entry::{check_count, check_id, check_label, check_metadata, check_shape, check_value};
use crate::{
    CHECKSUM_LEN, Entry, Error, FORMAT_VERSION, HEADER_LEN, MAGIC, MAX_FILE_BYTES, Metric, checksum,
};

/// Decodes a model file.
///
/// Checks run in this order: size limit, minimum length, magic, format version, checksum, the
/// entries field by field, and finally that no bytes follow the last entry. Nothing is allocated
/// from a stored count until the bytes it describes are known to be present.
///
/// # Errors
///
/// Returns the first rule the bytes break.
pub fn decode(bytes: &[u8]) -> Result<Vec<Entry>, Error> {
    if bytes.len() > MAX_FILE_BYTES {
        return Err(Error::TooLarge);
    }
    if bytes.len() < HEADER_LEN + CHECKSUM_LEN {
        return Err(Error::Truncated);
    }
    let (content, trailer) = bytes.split_at(bytes.len() - CHECKSUM_LEN);
    let mut reader = Reader(content);
    if reader.take(MAGIC.len())? != MAGIC {
        return Err(Error::BadMagic);
    }
    if reader.u16()? != FORMAT_VERSION {
        return Err(Error::UnsupportedVersion);
    }
    let stored = u32::from_le_bytes(trailer.try_into().map_err(|_| Error::Truncated)?);
    if checksum(content) != stored {
        return Err(Error::ChecksumMismatch);
    }

    let count = usize::from(reader.u16()?);
    check_count(count)?;
    let mut entries = Vec::with_capacity(count);
    let mut previous = None;
    for _ in 0..count {
        let entry = read_entry(&mut reader, previous)?;
        previous = Some(entry.id);
        entries.push(entry);
    }
    if !reader.0.is_empty() {
        return Err(Error::TrailingBytes);
    }
    Ok(entries)
}

fn read_entry(reader: &mut Reader<'_>, previous: Option<u32>) -> Result<Entry, Error> {
    let id = reader.u32()?;
    check_id(id, previous)?;
    let time = reader.i64()?;
    let metric = Metric::from_code(reader.u8()?).ok_or(Error::InvalidMetric)?;
    let encoding_count = usize::from(reader.u8()?);
    let encoding_len = usize::from(reader.u16()?);
    let value_count = check_shape(encoding_count, encoding_len)?;
    let label_len = usize::from(reader.u8()?);
    let label = check_label(reader.take(label_len)?)?.to_owned();
    let backend_len = usize::from(reader.u8()?);
    let backend = check_metadata(reader.take(backend_len)?)?.to_owned();
    let model_len = usize::from(reader.u8()?);
    let model = check_metadata(reader.take(model_len)?)?.to_owned();

    let byte_count = value_count
        .checked_mul(size_of::<f32>())
        .ok_or(Error::Truncated)?;
    let raw = reader.take(byte_count)?;
    let mut values = Vec::with_capacity(value_count);
    let (chunks, _) = raw.as_chunks::<{ size_of::<f32>() }>();
    for &chunk in chunks {
        let value = f32::from_le_bytes(chunk);
        check_value(value)?;
        values.push(value);
    }
    Ok(Entry {
        id,
        time,
        metric,
        label,
        backend,
        model,
        encoding_len,
        values,
    })
}

/// A cursor over the bytes still to decode.
struct Reader<'a>(&'a [u8]);

impl<'a> Reader<'a> {
    fn take(&mut self, len: usize) -> Result<&'a [u8], Error> {
        if len > self.0.len() {
            return Err(Error::Truncated);
        }
        let (head, rest) = self.0.split_at(len);
        self.0 = rest;
        Ok(head)
    }

    fn array<const N: usize>(&mut self) -> Result<[u8; N], Error> {
        self.take(N)?.try_into().map_err(|_| Error::Truncated)
    }

    fn u8(&mut self) -> Result<u8, Error> {
        Ok(self.array::<1>()?[0])
    }

    fn u16(&mut self) -> Result<u16, Error> {
        self.array().map(u16::from_le_bytes)
    }

    fn u32(&mut self) -> Result<u32, Error> {
        self.array().map(u32::from_le_bytes)
    }

    fn i64(&mut self) -> Result<i64, Error> {
        self.array().map(i64::from_le_bytes)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        EntryRef, MAX_ENCODING_LEN, MAX_ENCODINGS_PER_MODEL, MAX_ID, MAX_LABEL_BYTES, MAX_MODELS,
        encode,
    };

    /// Offset of the first entry's fields, used to corrupt one field at a time.
    const ID: usize = HEADER_LEN;
    const METRIC: usize = ID + 4 + 8;
    const ENCODING_COUNT: usize = METRIC + 1;
    const ENCODING_LEN: usize = ENCODING_COUNT + 1;
    const LABEL_LEN: usize = ENCODING_LEN + 2;

    fn sample(
        id: u32,
        label: &'static [u8],
        encodings: &'static [&'static [f32]],
    ) -> EntryRef<'static> {
        EntryRef {
            id,
            time: 1_700_000_000,
            metric: Metric::Cosine,
            label,
            backend: b"opencv_dnn_sface",
            model: b"sface.onnx",
            encodings,
        }
    }

    fn one() -> Vec<u8> {
        encode(&[sample(0, b"a", &[&[1.0, -2.0]])]).expect("valid entry encodes")
    }

    /// Replaces the checksum after `bytes` was modified, so a later rule is reached.
    fn reseal(mut bytes: Vec<u8>) -> Vec<u8> {
        let end = bytes.len() - CHECKSUM_LEN;
        let sum = checksum(&bytes[..end]);
        bytes[end..].copy_from_slice(&sum.to_le_bytes());
        bytes
    }

    fn patched(offset: usize, replacement: &[u8]) -> Vec<u8> {
        let mut bytes = one();
        bytes[offset..offset + replacement.len()].copy_from_slice(replacement);
        reseal(bytes)
    }

    #[test]
    fn checksum_is_crc32c() {
        assert_eq!(checksum(b"123456789"), 0xE306_9283);
    }

    #[test]
    fn layout_is_pinned() {
        let entry = EntryRef {
            id: 0x0102_0304,
            time: -2,
            metric: Metric::L2Norm,
            label: b"a",
            backend: b"b",
            model: b"c",
            encodings: &[&[1.0, -2.0]],
        };
        let bytes = encode(&[entry]).expect("encodes");
        let mut expected = Vec::new();
        expected.extend_from_slice(b"HOWDYMDL");
        expected.extend_from_slice(&[1, 0, 1, 0]);
        expected.extend_from_slice(&[4, 3, 2, 1]);
        expected.extend_from_slice(&[0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]);
        expected.extend_from_slice(&[3, 1, 2, 0]);
        expected.extend_from_slice(&[1, b'a', 1, b'b', 1, b'c']);
        expected.extend_from_slice(&[0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0xC0]);
        expected.extend_from_slice(&checksum(&expected).to_le_bytes());
        assert_eq!(bytes, expected);
    }

    #[test]
    fn round_trips_canonically() {
        let bytes = encode(&[
            sample(0, b"first", &[&[0.5, -0.0], &[1.5, 2.5]]),
            sample(
                7,
                "Café".as_bytes(),
                &[&[f32::MIN_POSITIVE, f32::MAX, -1.0]],
            ),
            sample(MAX_ID, b"first", &[&[3.0]]),
        ])
        .expect("encodes");
        let entries = decode(&bytes).expect("decodes");
        assert_eq!(entries.len(), 3);
        assert_eq!(
            entries[0].encodings(),
            vec![&[0.5, -0.0][..], &[1.5, 2.5][..]]
        );
        assert_eq!(entries[1].label, "Café");
        assert_eq!(entries[2].id, MAX_ID);
        assert_eq!(entries[2].label, entries[0].label, "labels may repeat");

        let slices: Vec<Vec<&[f32]>> = entries.iter().map(Entry::encodings).collect();
        let refs: Vec<EntryRef<'_>> = entries
            .iter()
            .zip(&slices)
            .map(|(entry, encodings)| entry.as_ref(encodings))
            .collect();
        assert_eq!(encode(&refs).expect("re-encodes"), bytes);
    }

    #[test]
    fn rejects_container_errors() {
        let valid = one();
        assert_eq!(decode(&[]), Err(Error::Truncated));
        assert_eq!(
            decode(&valid[..HEADER_LEN + CHECKSUM_LEN - 1]),
            Err(Error::Truncated)
        );
        assert_eq!(decode(&vec![0; MAX_FILE_BYTES + 1]), Err(Error::TooLarge));

        let mut bad_magic = valid.clone();
        bad_magic[0] = b'X';
        assert_eq!(decode(&bad_magic), Err(Error::BadMagic));

        let mut version = valid.clone();
        version[MAGIC.len()] = 2;
        assert_eq!(decode(&version), Err(Error::UnsupportedVersion));

        let mut flipped = valid.clone();
        flipped[METRIC + 3] ^= 0x01;
        assert_eq!(decode(&flipped), Err(Error::ChecksumMismatch));

        let mut cut = valid[..valid.len() - CHECKSUM_LEN - 1].to_vec();
        cut.extend_from_slice(&[0; CHECKSUM_LEN]);
        assert_eq!(decode(&reseal(cut)), Err(Error::Truncated));

        let mut trailing = valid[..valid.len() - CHECKSUM_LEN].to_vec();
        trailing.extend_from_slice(&[0; 1 + CHECKSUM_LEN]);
        assert_eq!(decode(&reseal(trailing)), Err(Error::TrailingBytes));
    }

    #[test]
    fn rejects_entry_errors() {
        let count = MAGIC.len() + 2;
        assert_eq!(decode(&patched(count, &[0, 0])), Err(Error::NoModels));
        let too_many = u16::try_from(MAX_MODELS + 1).expect("fits").to_le_bytes();
        assert_eq!(
            decode(&patched(count, &too_many)),
            Err(Error::TooManyModels)
        );
        assert_eq!(decode(&patched(count, &[2, 0])), Err(Error::Truncated));
        assert_eq!(
            decode(&patched(ID, &(MAX_ID + 1).to_le_bytes())),
            Err(Error::IdOutOfRange)
        );
        assert_eq!(decode(&patched(METRIC, &[0])), Err(Error::InvalidMetric));
        assert_eq!(decode(&patched(METRIC, &[4])), Err(Error::InvalidMetric));
        assert_eq!(
            decode(&patched(ENCODING_COUNT, &[0])),
            Err(Error::NoEncodings)
        );
        let encodings = u8::try_from(MAX_ENCODINGS_PER_MODEL + 1).expect("fits");
        assert_eq!(
            decode(&patched(ENCODING_COUNT, &[encodings])),
            Err(Error::TooManyEncodings)
        );
        assert_eq!(
            decode(&patched(ENCODING_LEN, &[0, 0])),
            Err(Error::EncodingLength)
        );
        let too_long = u16::try_from(MAX_ENCODING_LEN + 1)
            .expect("fits")
            .to_le_bytes();
        assert_eq!(
            decode(&patched(ENCODING_LEN, &too_long)),
            Err(Error::EncodingLength)
        );
        assert_eq!(decode(&patched(LABEL_LEN, &[0])), Err(Error::InvalidLabel));
        assert_eq!(
            decode(&patched(LABEL_LEN, &[1, b'/'])),
            Err(Error::InvalidLabel)
        );
        assert_eq!(
            decode(&patched(LABEL_LEN, &[1, 0xFF])),
            Err(Error::InvalidLabel)
        );
        let label = u8::try_from(MAX_LABEL_BYTES + 1).expect("fits");
        assert_eq!(
            decode(&patched(LABEL_LEN, &[label])),
            Err(Error::InvalidLabel)
        );
        assert_eq!(
            decode(&patched(LABEL_LEN + 2, &[0])),
            Err(Error::InvalidMetadata)
        );
        assert_eq!(
            decode(&patched(LABEL_LEN + 3, b" ")),
            Err(Error::InvalidMetadata)
        );
    }

    #[test]
    fn rejects_ids_out_of_order() {
        let bytes =
            encode(&[sample(1, b"a", &[&[1.0]]), sample(2, b"b", &[&[1.0]])]).expect("encodes");
        let second = bytes.len() - CHECKSUM_LEN - (4 + 8 + 1 + 1 + 2 + 2 + 17 + 11 + 4);
        let mut duplicate = bytes.clone();
        duplicate[second..second + 4].copy_from_slice(&1_u32.to_le_bytes());
        assert_eq!(decode(&reseal(duplicate)), Err(Error::IdNotAscending));
        let mut descending = bytes;
        descending[second..second + 4].copy_from_slice(&0_u32.to_le_bytes());
        assert_eq!(decode(&reseal(descending)), Err(Error::IdNotAscending));
    }

    #[test]
    fn rejects_non_finite_values() {
        let values = one().len() - CHECKSUM_LEN - 8;
        for bad in [f32::NAN, f32::INFINITY, f32::NEG_INFINITY] {
            assert_eq!(
                decode(&patched(values, &bad.to_le_bytes())),
                Err(Error::NonFiniteValue)
            );
        }
    }

    #[test]
    fn huge_stored_counts_fail_before_allocating() {
        // The largest shape the header can describe, backed by no value bytes at all.
        let mut bytes = one();
        bytes[ENCODING_COUNT] = u8::try_from(MAX_ENCODINGS_PER_MODEL).expect("fits");
        bytes[ENCODING_LEN..ENCODING_LEN + 2]
            .copy_from_slice(&u16::try_from(MAX_ENCODING_LEN).expect("fits").to_le_bytes());
        assert_eq!(decode(&reseal(bytes)), Err(Error::Truncated));
    }

    #[test]
    fn accepts_files_at_the_model_limit() {
        let entries: Vec<EntryRef<'static>> = (0..MAX_MODELS)
            .map(|id| sample(u32::try_from(id).expect("fits"), b"m", &[&[1.0]]))
            .collect();
        let bytes = encode(&entries).expect("encodes");
        assert_eq!(decode(&bytes).expect("decodes").len(), MAX_MODELS);
    }
}
