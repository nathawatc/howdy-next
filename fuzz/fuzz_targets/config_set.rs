#![no_main]

libfuzzer_sys::fuzz_target!(|data: &[u8]| howdy_config::fuzzing::set_round_trips(data));
