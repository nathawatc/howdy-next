//! Exported C functions.
//!
//! Rules for every export: `extern "C"`, byte slices as `(ptr, len)`, a [`HowdyFfiStatus`]
//! return value with results in out-parameters, and the whole body inside `boundary::guard`. Memory
//! allocated here is released only by a matching exported free function.
#![allow(unsafe_code)]

mod config;
mod user_model;

pub use config::HOWDY_CONFIG_MAX_BYTES;
pub use user_model::{
    HOWDY_USER_MODEL_MAX_ENCODING_LEN, HOWDY_USER_MODEL_MAX_ENCODINGS_PER_MODEL,
    HOWDY_USER_MODEL_MAX_FILE_BYTES, HOWDY_USER_MODEL_MAX_ID, HOWDY_USER_MODEL_MAX_LABEL_BYTES,
    HOWDY_USER_MODEL_MAX_MODELS,
};

use std::ffi::c_char;

use crate::boundary::guard;
use crate::{HOWDY_FFI_ABI_VERSION, HowdyFfiStatus};

/// A borrowed byte range (`char` on the C side), holding text or binary content. Inputs with
/// `len == 0` may have a null `ptr`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct HowdyStr {
    pub ptr: *const c_char,
    pub len: usize,
}

impl HowdyStr {
    pub(crate) fn from_str(text: &str) -> Self {
        Self::from_bytes(text.as_bytes())
    }

    pub(crate) fn from_bytes(bytes: &[u8]) -> Self {
        Self {
            ptr: bytes.as_ptr().cast::<c_char>(),
            len: bytes.len(),
        }
    }

    /// Borrows the bytes, or `None` when `ptr` is null with a non-zero `len`.
    ///
    /// # Safety
    ///
    /// When `len > 0`, `ptr` must be valid for reads of `len` bytes for `'a`.
    pub(crate) unsafe fn bytes<'a>(self) -> Option<&'a [u8]> {
        // SAFETY: forwarded caller contract.
        unsafe { borrow(self.ptr.cast::<u8>(), self.len) }
    }
}

/// Borrows `len` elements at `ptr`: empty when `len == 0` (any `ptr`), `None` when `ptr` is null
/// with a non-zero `len`.
///
/// # Safety
///
/// When `len > 0`, `ptr` must be aligned and valid for reads of `len` elements for `'a`.
pub(crate) unsafe fn borrow<'a, T>(ptr: *const T, len: usize) -> Option<&'a [T]> {
    if len == 0 {
        return Some(&[]);
    }
    if ptr.is_null() {
        return None;
    }
    // SAFETY: non-null and, per the caller contract, aligned and valid for `len` elements.
    Some(unsafe { std::slice::from_raw_parts(ptr, len) })
}

/// Writes `value` through `out`, or returns `InvalidArgument` when `out` is null.
///
/// # Safety
///
/// `out` must be null or valid for a write of one `T`.
pub(crate) unsafe fn write_out<T>(out: *mut T, value: T) -> HowdyFfiStatus {
    // SAFETY: the caller guarantees `out` is null or valid for writes; `as_mut` handles null.
    let Some(out) = (unsafe { out.as_mut() }) else {
        return HowdyFfiStatus::InvalidArgument;
    };
    *out = value;
    HowdyFfiStatus::Ok
}

/// Writes [`HOWDY_FFI_ABI_VERSION`] to `out`.
///
/// # Safety
///
/// `out` must be null or valid for a write of one `u32`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn howdy_ffi_abi_version(out: *mut u32) -> HowdyFfiStatus {
    // SAFETY: the caller guarantees `out` is null or valid for one `u32` write.
    guard(|| unsafe { write_out(out, HOWDY_FFI_ABI_VERSION) })
}

#[cfg(test)]
mod tests {
    use super::howdy_ffi_abi_version;
    use crate::{HOWDY_FFI_ABI_VERSION, HowdyFfiStatus};

    #[test]
    fn writes_abi_version() {
        let mut version = 0;
        // SAFETY: `version` is a live, writable u32.
        let status = unsafe { howdy_ffi_abi_version(&raw mut version) };
        assert_eq!(status, HowdyFfiStatus::Ok);
        assert_eq!(version, HOWDY_FFI_ABI_VERSION);
    }

    #[test]
    fn rejects_null_out() {
        // SAFETY: null is an accepted input.
        let status = unsafe { howdy_ffi_abi_version(std::ptr::null_mut()) };
        assert_eq!(status, HowdyFfiStatus::InvalidArgument);
    }
}
