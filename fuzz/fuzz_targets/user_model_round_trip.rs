#![no_main]
libfuzzer_sys::fuzz_target!(|data: &[u8]| howdy_user_model::fuzzing::round_trip(data));
