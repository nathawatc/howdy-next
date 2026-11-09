//! Exported C functions.
//!
//! Rules for every export: `extern "C"`, byte slices as `(ptr, len)`, a [`HowdyFfiStatus`]
//! return value with results in out-parameters, and the whole body inside [`guard`]. Memory
//! allocated here is released only by a matching exported free function.
#![allow(unsafe_code)]

use crate::boundary::guard;
use crate::{HOWDY_FFI_ABI_VERSION, HowdyFfiStatus};

/// Writes [`HOWDY_FFI_ABI_VERSION`] to `out`.
///
/// # Safety
///
/// `out` must be null or valid for a write of one `u32`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn howdy_ffi_abi_version(out: *mut u32) -> HowdyFfiStatus {
    guard(|| {
        // SAFETY: the caller guarantees `out` is null or valid for writes; `as_mut` handles null.
        let Some(out) = (unsafe { out.as_mut() }) else {
            return HowdyFfiStatus::InvalidArgument;
        };
        *out = HOWDY_FFI_ABI_VERSION;
        HowdyFfiStatus::Ok
    })
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
