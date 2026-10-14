# QA tooling: property-based tests

Property-based tests live in `tests/property/`. They are built with
[Hegel for C++](https://hegel.dev/cpp) on top of Boost.Test. For the overall
plan see `PBT_PLAN.md`. For tests that are blocked on server changes see
`todo.md`.

All scripts keep their files inside `WORKDIR`. By default `WORKDIR` is the
parent directory of this repository, e.g. `~/ws`.

## Quick start

```bash
cd ~/ws/percona-binlog-server
export BUILD_PRESET=release_gcc14       # default; any preset from CMakePresets.json

extra/qa/setup_pbt_env.sh               # libhegel + hegel-cpp + SeaweedFS -> ~/ws
extra/qa/build_property_tests.sh        # -> ~/ws/percona-binlog-server-build-release_gcc14-pbt
extra/qa/run_property_tests.py          # -> ~/ws/testruns/<timestamp>/
```

Boost and the AWS SDK must already be installed in `WORKDIR` for the chosen
preset, as described in the top-level `README.md`.

## Running tests

| What | Command |
|---|---|
| all properties + report | `extra/qa/run_property_tests.py` |
| quick smoke run (10% of the cases) | `extra/qa/run_property_tests.py --cases-scale 0.1` |
| nightly run (10x cases) | `extra/qa/run_property_tests.py --cases-scale 10` |
| a subset | `extra/qa/run_property_tests.py --filter 'gtid_set.*Decode'` |
| list properties | `extra/qa/run_property_tests.py --list` |
| one executable | `~/ws/percona-binlog-server-build-<preset>-pbt/tests/property/gtid_set_property_test` |
| one property | `.../gtid_set_property_test --run_test=TextRoundTrip` |
| replay a failure | `PBT_REPRODUCE='<blob>' .../gtid_set_property_test --run_test=<name>` (the command is in `summary.md`) |
| via ctest | `ctest --test-dir <build dir> -L property` |

These environment variables are read by every property executable:

| Variable | Meaning |
|---|---|
| `PBT_TEST_CASES` | number of generated cases for every property |
| `PBT_CASES_SCALE` | multiplies each property's default case count |
| `PBT_SEED` | fixed seed |
| `PBT_DERANDOMIZE` | `1` makes the run deterministic |
| `PBT_DATABASE` | Hegel example database directory, or `off` |
| `PBT_REPORT_MULTIPLE` | `1` keeps searching after the first failure |
| `PBT_REPRODUCE` | failure blob to replay |
| `PBT_VERBOSITY` | `quiet`, `normal`, `verbose` or `debug` |

## Report

Each run writes these files to `WORKDIR/testruns/<YYYYmmdd-HHMMSS>/`.
`WORKDIR/testruns/latest` points to the newest run.

- `summary.md`: the counts of passing tests, failing tests, new issues,
  known issues, regressions and "possibly fixed". For every new issue it
  also has the minimal counterexample and the reproduce command.
- `summary.json`: the same information in machine-readable form.
- `junit.xml`: for Jenkins (`junit 'testruns/latest/junit.xml'`). Known
  issues are failures with a `[KNOWN <issue>]` prefix. Pass
  `--known-as skipped` to report them as skipped instead.
- `run_info.json`: the git revision, preset and knobs.
- `logs/<executable>/<property>.log`: the full output of each property.

The exit code (`--fail-on`) is set as follows. With `new` (the default) it
is `1` when there are new issues, regressions or infrastructure errors.
With `any` it is `1` on any failure. With `never` it is always `0`.

## Known bugs

`known_bugs.md` holds a table of failures that are already known. A failure
that matches a row is counted as a known (old) issue. Anything else is
counted as new. The format is described at the top of that file.

When a run reports a new issue:

1. Reproduce it with the command from `summary.md`.
2. File it in Jira.
3. Add a row to `known_bugs.md`.

## S3 endpoint (SeaweedFS)

```bash
extra/qa/seaweedfs.sh start          # weed mini on 127.0.0.1:8333, bucket pbs-pbt
eval "$(extra/qa/seaweedfs.sh env)"  # PBT_S3_ENDPOINT / _ACCESS_KEY / _SECRET_KEY / _BUCKET
extra/qa/seaweedfs.sh uri mydir      # storage.uri for main_config.json
extra/qa/seaweedfs.sh stop
```

Ports and credentials are set in `pbt_env.sh` (`SEAWEEDFS_*`).

## Adding a property test

1. Create `tests/property/<area>_property_test.cpp` with
   `#define BOOST_TEST_MODULE <Area>PropertyTests`. Include
   `common/pbt_helpers.hpp`.
2. Write one property per `BOOST_AUTO_TEST_CASE`. The body calls
   `pbt::check([](hegel::TestCase &tc) { ... }, {.test_cases = N});`.
   Use `pbt::require*` for assertions. Their messages must not contain
   generated values (put those into `tc.note()`), so that `known_bugs.md`
   can match them.
3. Register it in `tests/property/CMakeLists.txt` with
   `pbs_add_property_test(<name> SOURCES ... LIBRARIES ...)`.
4. Ground every property in a documented contract (README, MySQL behaviour,
   code comments) or a reference model. Cover both directions: valid input
   must give the right value, and invalid input must be rejected.
