---
name: run-fuzzers
description: Configure with Clang libFuzzer and run the protocol decoder and INI editor fuzz targets.
disable-model-invocation: true
---

# Run Fuzzers

Fixed-input fuzz replay (`native-runtime-descriptor-message-fuzz-replay`,
`native-config-key-file-fuzz-replay`) runs in every CTest build. Use this procedure to build and run
the Clang libFuzzer + ASan/UBSan binaries.

## Steps

1. **Configure a Clang fuzz build** (`HOWDY_ENABLE_FUZZING=ON` fails closed if libFuzzer cannot
   link):

   ```sh
   cmake -S . -B build/fuzz \
     -DCMAKE_BUILD_TYPE=Debug \
     -DCMAKE_CXX_COMPILER=clang++ \
     -DHOWDY_ENABLE_FUZZING=ON
   ```

2. **Build and run the fuzz targets** (adjust `-max_total_time` as needed):

   ```sh
   cmake --build build/fuzz --parallel "$(nproc)" \
     --target howdy_runtime_descriptor_message_fuzzer howdy_config_key_file_fuzzer
   ./build/fuzz/howdy/howdy_runtime_descriptor_message_fuzzer -max_total_time=30
   ./build/fuzz/howdy/howdy_config_key_file_fuzzer -max_total_time=30
   ```

3. **Add regression inputs** — if fuzzing finds a crash or sanitizer issue, add a minimal replay
   case in the target's `*_fuzz_replay.cpp` (`howdy/tests/protocol/` or `howdy/tests/config/`) and
   verify with `ctest --test-dir build/fuzz -R fuzz-replay`.

## Done Criteria

- Each fuzz target runs for the requested duration with zero crashes or ASan/UBSan findings.
- Any discovered edge case is captured in the replay test and passes CTest.
