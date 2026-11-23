---
name: run-fuzzers
description: Configure with Clang libFuzzer and run protocol decoder fuzz targets.
disable-model-invocation: true
---

# Run Protocol Fuzzers

Fixed-input fuzz replay (`native-runtime-descriptor-message-fuzz-replay`) runs in every CTest
build. Use this procedure to build and run the Clang libFuzzer + ASan/UBSan binary.

## Steps

1. **Configure a Clang fuzz build** (`HOWDY_ENABLE_FUZZING=ON` fails closed if libFuzzer cannot
   link):

   ```sh
   cmake -S . -B build/fuzz \
     -DCMAKE_BUILD_TYPE=Debug \
     -DCMAKE_CXX_COMPILER=clang++ \
     -DHOWDY_ENABLE_FUZZING=ON
   ```

2. **Build and run the fuzz target** (adjust `-max_total_time` as needed):

   ```sh
   cmake --build build/fuzz --target howdy_runtime_descriptor_message_fuzzer --parallel "$(nproc)"
   ./build/fuzz/howdy/howdy_runtime_descriptor_message_fuzzer -max_total_time=30
   ```

3. **Add regression inputs** — if fuzzing finds a crash or sanitizer issue, add a minimal replay
   case in `howdy/tests/protocol/runtime_descriptor_message_fuzz_replay.cpp` and verify with
   `ctest --test-dir build/fuzz -R native-runtime-descriptor-message`.

## Done Criteria

- `howdy_runtime_descriptor_message_fuzzer` runs for the requested duration with zero crashes or
  ASan/UBSan findings.
- Any discovered edge case is captured in the replay test and passes CTest.
