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
extra/qa/build_property_tests.sh        # -> ~/ws/percona-binlog-server-build-release_gcc14-pbt (see "Build variants")
extra/qa/run_property_tests.py          # -> ~/ws/testruns/<timestamp>-<variant>/
```

Boost and the AWS SDK must already be installed in `WORKDIR` for the chosen
preset, as described in the top-level `README.md`.

## Build variants

A build variant is a CMake preset, optionally with extra sanitizers. Each
variant gets its own build directory,
`~/ws/percona-binlog-server-build-<variant>-pbt`, so several can coexist.

| Variant | Build | Run | Use |
|---|---|---|---|
| `asan_gcc14` | `build_property_tests.sh --preset asan_gcc14` | `run_property_tests.py --preset asan_gcc14` | main variant: Debug + AddressSanitizer, `assert`s active |
| `release_gcc14` | `build_property_tests.sh --preset release_gcc14` | `run_property_tests.py --preset release_gcc14` | what ships: optimised, `assert`s compiled out |
| `asan_gcc14-ubsan` | `build_property_tests.sh --preset asan_gcc14 --sanitize undefined` | `run_property_tests.py --preset asan_gcc14 --sanitize undefined` | adds UndefinedBehaviorSanitizer |

- `--sanitize` takes a comma-separated `-fsanitize=` list. Each entry adds a
  suffix to the variant name: `undefined` → `-ubsan`, `address` → `-asan`,
  `thread` → `-tsan`.
- UBSan builds keep running after a report. Each distinct source location
  is reported once.
- ThreadSanitizer is not useful yet (todo.md T6).
- The build script writes `pbt_build_info.json` into the build directory.
  The runner reads it, so reports carry the variant name:
  - `summary.md` has the variant in its header;
  - `junit.xml` suites are named `property.<variant>.<executable>`;
  - `testruns/latest-<variant>` points to the newest run of that variant
    (`testruns/latest` points to the newest run of any variant).
- Sanitizer reports (AddressSanitizer, LeakSanitizer, UBSan
  `runtime error:`, ThreadSanitizer) always fail the property, with kind
  `sanitizer` and the first report line as the message.
  `known_bugs.md` can match them like any other failure.

## Running tests

| What | Command |
|---|---|
| all properties + report | `extra/qa/run_property_tests.py` |
| quick smoke run (10% of the cases, 10% scale-probe size) | `extra/qa/run_property_tests.py --cases-scale 0.1` |
| nightly run (10x cases) | `extra/qa/run_property_tests.py --cases-scale 10` |
| a subset | `extra/qa/run_property_tests.py --filter 'gtid_set.*Decode'` |
| list properties | `extra/qa/run_property_tests.py --list` |
| one executable | `~/ws/percona-binlog-server-build-<variant>-pbt/tests/property/gtid_set_property_test` |
| one property | `.../gtid_set_property_test --run_test=TextRoundTrip` |
| replay a failure | `PBT_REPRODUCE='<blob>' .../gtid_set_property_test --run_test=<name>` (the command is in `summary.md`) |
| via ctest | `ctest --test-dir <build dir> -L property` |

These environment variables are read by every property executable:

| Variable | Meaning |
|---|---|
| `PBT_TEST_CASES` | number of generated cases for every property |
| `PBT_CASES_SCALE` | multiplies each property's default case count and the scale-probe input size |
| `PBT_SEED` | fixed seed |
| `PBT_DERANDOMIZE` | `1` makes the run deterministic |
| `PBT_DATABASE` | Hegel example database directory, or `off` |
| `PBT_REPORT_MULTIPLE` | `1` keeps searching after the first failure |
| `PBT_REPRODUCE` | failure blob to replay |
| `PBT_VERBOSITY` | `quiet`, `normal`, `verbose` or `debug` |

## Report

Each run writes these files to `WORKDIR/testruns/<YYYYmmdd-HHMMSS>-<variant>/`,
for example `testruns/20261014-112341-asan_gcc14-ubsan/`.
`WORKDIR/testruns/latest` points to the newest run.

- `summary.md`: the counts of passing tests, failing tests, new issues,
  known issues, regressions and "possibly fixed", plus the known issues per
  severity (high / medium / low). Known issues are listed most severe
  first. For every new issue it also has the minimal counterexample and the
  reproduce command; new issues are `untriaged` until they get a
  `known_bugs.md` row.
- `summary.json`: the same information in machine-readable form.
- `junit.xml`: for Jenkins (`junit 'testruns/latest/junit.xml'`). Known
  issues are failures with a `[KNOWN <issue> <severity>]` prefix, new ones
  `[NEW untriaged]`. Pass
  `--known-as skipped` to report them as skipped instead.
- `run_info.json`: the git revision, preset and knobs.
- `logs/<executable>/<property>.log`: the full output of each property.

Properties that need temporary files (storage tests) create them under
`$TMPDIR`. The runner sets it to `<run dir>/tmp` and removes that directory
at the end, so nothing is written outside `WORKDIR`. When you run a test
executable by hand, set `TMPDIR` yourself.

Properties whose name contains `S3` need an S3 endpoint. By default
(`--s3 auto`) the runner uses the `PBT_S3_*` variables from the
environment. If they are not set, it starts SeaweedFS
(`extra/qa/seaweedfs.sh start`, installed by `setup_pbt_env.sh`), then
stops and wipes it after the run. `--s3 off` skips these properties. To
run one of them by hand: `eval "$(extra/qa/seaweedfs.sh env)"`.

`storage_property_test`, `config_property_test`, `cli_property_test` and
`mysql_property_test` link the server sources through the
`pbs_test_binsrv` test library (todo.md T1). They need the same dependencies as `binlog_server` (MySQL client
library, AWS SDK, Boost). `cli_property_test` and `mysql_property_test` also
run the `binlog_server` binary of the same build directory (it is built as
a dependency), on storages under `$TMPDIR`.

Known bugs are reported as failures in JUnit (`--known-as failure`, the
default), so they stay visible in Jenkins; `--known-as skipped` hides them.

A failure that depends on timing can pass when Hegel replays it, and
Hegel then reports only "Flaky test detected". `pbt::check` therefore
writes the first failing test case of every property to the log right
away (`PBT_FIRST_FAILURE:` with its `pbt::note` lines), and the report
shows such a property as `FLAKY: <first failure>` with those notes.

Every property runs in its own process group. When the runner is
terminated (SIGTERM / SIGINT / SIGHUP, e.g. an aborted Jenkins build) it
kills the running properties, so none keeps running (or restarting the
MySQL server) on its own; servers it started are left running then.

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
3. Add a row to `known_bugs.md`, with its severity (the scale is defined at
   the top of that file).

## S3 endpoint (SeaweedFS)

```bash
extra/qa/seaweedfs.sh start          # weed mini on 127.0.0.1:8333, bucket pbs-pbt
eval "$(extra/qa/seaweedfs.sh env)"  # PBT_S3_ENDPOINT / _ACCESS_KEY / _SECRET_KEY / _BUCKET
extra/qa/seaweedfs.sh uri mydir      # storage.uri for main_config.json
extra/qa/seaweedfs.sh stop
```

Ports and credentials are set in `pbt_env.sh` (`SEAWEEDFS_*`).

## MySQL server (layer D)

Properties whose name contains `MySQL` (`mysql_property_test`) run the
`binlog_server` binary against a real MySQL server and compare the storage
with the server's binlog files. They reset the server's binary logs and
GTIDs, so they use a private, throw-away server: by default
(`--mysql auto`) the runner starts it with `extra/qa/mysqld.sh` when
`PBT_MYSQL_PORT` is not set, and stops it after the run (its data
directory `$WORKDIR/mysql-pbt` is kept, so the next start is quick).
`--mysql off` skips these properties. The MySQL properties never run in
parallel with each other (`--jobs`). A second instance (`mysqld.sh
--replica`, port 13307, `$WORKDIR/mysql-pbt-replica`) is started as well:
it acts as a MySQL replica of the Binlog Server's replication source
listener. Properties that restart or crash the server use
`PBT_MYSQLD_CTL` (= this script), so they need the private server.

```bash
extra/qa/mysqld.sh start            # mysqld on 127.0.0.1:13306, data in $WORKDIR/mysql-pbt
eval "$(extra/qa/mysqld.sh env)"    # PBT_MYSQL_HOST / _PORT / _USER / _PASSWORD
extra/qa/mysqld.sh restart          # graceful restart / 'crash': kill -9 and start
extra/qa/mysqld.sh --replica start  # the replica instance (same commands)
extra/qa/mysqld.sh stop
extra/qa/mysqld.sh wipe             # stop and delete the instance
```

The server runs with `log_bin`, `gtid_mode=ON`, `binlog_checksum=CRC32`
and binds to 127.0.0.1 only. `mysqld` is taken from `$MYSQLD_BASEDIR/bin`,
`$WORKDIR/mysql/bin` (`setup_pbt_env.sh --with-mysql` unpacks a Percona
Server tarball there, `MYSQLD_TARBALL_URL`), `PATH` or `/usr/sbin`. On
Ubuntu the AppArmor profile of `/usr/sbin/mysqld` may refuse a data
directory outside `/var/lib/mysql`; use a tarball then. Tagged GTIDs are
part of the workload only with MySQL 8.3 or newer. Settings: `MYSQLD_*` in
`pbt_env.sh`.

To point the tests at another server, set `PBT_MYSQL_*` yourself. Its
binlog files must be readable by the test process, and its binary logs
are reset.

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
