# Property tests blocked on server source changes

Tests in `tests/property/` must not require changes to the server sources
(the `WITH_PROPERTY_TESTS` CMake switch is the only exception). Each entry
below names the tests that need a change, the change itself, and the reason
for it. When a change lands, move the tests into `PBT_PLAN.md` as
"can be written now".

## T1 - library target for `src/binsrv/*.cpp` (and `src/operations`)

- **Tests that need it:**
  - A9: `size_unit` and `time_unit` parsing
  - A10: `main_config` and keyring validation
  - B1–B4: storage stateful model, S3 equivalence, encryption at rest,
    crash recovery
  - C1–C4: storage generator for the binary-level tests
- **Change:** add a static library, e.g. `lib_binsrv` (`binsrv::lib_binsrv`),
  built from the `src/binsrv/*.cpp` files that are currently compiled only
  into the `binlog_server` executable (`CMakeLists.txt`, the
  `add_executable(binlog_server ...)` source list). Optionally add
  `lib_operations` for `src/operations`. `binlog_server` would then link
  these libraries instead of compiling the files itself.
- **Why:** the tests need `storage`, `storage_core`, both storage backends,
  `main_config`, `size_unit`, `time_unit` and the keyring classes. None of
  them is reachable from a test target today.
- **Workaround until then:** `tests/property/CMakeLists.txt` can compile the
  needed `.cpp` files straight into the test executable. That needs no
  server change, but it compiles them twice, and the test's source list has
  to be updated whenever the server's list changes.

## T2 - injectable clock in `binsrv::storage`

- **Tests that need it:** B5, checkpointing by interval (`checkpoint_interval`).
- **Change:** let `storage` take a clock, e.g. a `std::function` returning
  `steady_clock::time_point` or a template/policy parameter, that defaults
  to `std::chrono::steady_clock`. Today `storage.cpp` calls
  `std::chrono::steady_clock::now()` directly (lines ~181 and ~268).
- **Why:** the interval-based flush decisions depend on wall time, so a
  property cannot assert when a checkpoint must happen without sleeping, and
  sleeping makes the test slow and flaky.

## T3 - fault-injection seam for storage backends

- **Tests that need it:** B4, crash consistency. The property is: a failure
  at any point while writing leaves storage that can be reopened, and that
  holds exactly the transactions written up to the last successful flush.
- **Change:** a way to give `storage` a custom `basic_storage_backend`
  (wrapping the real one) instead of always creating it through
  `storage_backend_factory::create(config)`. Examples: a constructor
  overload that takes a backend pointer, or a factory hook used only by tests.
- **Why:** to fail the N-th `put_object`, `write_data_to_stream` or `close`
  call deterministically. Killing the whole process (layer D) covers only
  some of these points and is much slower.

## T4 - operations return results instead of printing them (optional)

- **Tests that need it:** C1–C4 run in-process. Without this change they
  can still run through the `binlog_server` binary, which is the current
  plan.
- **Change:** have `list`, `search_by_*` and `purge_binlogs` build their
  `models::*_response` object in a function that the operation then prints.
  Today `execute()` writes to `std::cout` directly.
- **Why:** model checks would run in-process, about 100x faster than
  spawning the binary for each generated case. That allows far more cases
  per run.

## T5 - g++-14 -Wconversion false positive under UBSan (build fix)

- **Tests that need it:** none are blocked. `build_property_tests.sh --sanitize undefined`
  works around it with `-Wno-error=conversion`, which downgrades every
  `-Wconversion` error to a warning in UBSan builds.
- **Change:** in `src/binsrv/gtids/gtid_set.cpp`, in
  `gtid_set_parser::parse_hexadecimal_pair()`, write
  `result = static_cast<std::uint8_t>(result << 4U);` instead of
  `result <<= 4U;` (or an equivalent that compiles cleanly).
- **Why:** with `-fsanitize=undefined`, g++-14 reports "conversion from 'int'
  to 'uint8_t' may change value" on that line, and the project's `-Werror`
  breaks the build. Without the workaround, a UBSan build of the whole
  project fails.

## T6 - ThreadSanitizer preset

- **Tests that need it:** the future multi-threaded properties: storage
  readers and writers sharing `storage_core` (shared mutex), the
  `pull` / `sender_context` tests, and layer D.
- **Change:**
  - add `tsan_gcc14` and `tsan_clang20` presets to `CMakePresets.json`, with
    `-fsanitize=thread` (and a `WITH_TSAN` option in `CMakeLists.txt`, like
    `WITH_ASAN`);
  - build Boost and the AWS SDK with the same presets
    (`boost-install-tsan_gcc14`, `aws-sdk-cpp-install-tsan_gcc14`).
- **Why:** TSan needs every library in the process instrumented, otherwise
  it reports false races in uninstrumented code. The current properties are
  single-threaded, so TSan would find nothing yet.
  `build_property_tests.sh --sanitize thread` can be used for an
  experiment, but expect noise from the uninstrumented libraries.
