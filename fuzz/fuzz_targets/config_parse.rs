#![no_main]

libfuzzer_sys::fuzz_target!(|data: &[u8]| howdy_config::fuzzing::parse_bytes(data));
