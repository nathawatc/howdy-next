use std::panic::{AssertUnwindSafe, catch_unwind};

use crate::HowdyFfiStatus;

/// Runs an export body so that a panic becomes [`HowdyFfiStatus::Panic`] instead of unwinding
/// into C++ or a PAM client.
pub(crate) fn guard(body: impl FnOnce() -> HowdyFfiStatus) -> HowdyFfiStatus {
    catch_unwind(AssertUnwindSafe(body)).unwrap_or(HowdyFfiStatus::Panic)
}

#[cfg(test)]
mod tests {
    use super::guard;
    use crate::HowdyFfiStatus;

    #[test]
    fn returns_body_status() {
        assert_eq!(guard(|| HowdyFfiStatus::Ok), HowdyFfiStatus::Ok);
        assert_eq!(
            guard(|| HowdyFfiStatus::InvalidArgument),
            HowdyFfiStatus::InvalidArgument
        );
    }

    #[test]
    fn converts_panic_to_status() {
        let status = guard(|| panic!("boundary test panic"));
        assert_eq!(status, HowdyFfiStatus::Panic);
    }
}
