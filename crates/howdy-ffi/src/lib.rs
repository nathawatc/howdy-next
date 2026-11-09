//! C ABI boundary between Howdy C++ and its Rust components.
//!
//! This crate is the only one C++ links. Every export lives in [`ffi`], the only module allowed
//! to use `unsafe`; everything else is safe Rust.

mod boundary;
pub mod ffi;

/// Version of the exported C ABI. C++ compares it with the value in the generated header.
pub const HOWDY_FFI_ABI_VERSION: u32 = 1;

/// Result of every exported function.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HowdyFfiStatus {
    /// The call succeeded and its out-parameters are set.
    Ok = 0,
    /// A required pointer was null or an argument was out of range.
    InvalidArgument = 1,
    /// The call panicked; the panic was caught at the boundary.
    Panic = 2,
}
