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
