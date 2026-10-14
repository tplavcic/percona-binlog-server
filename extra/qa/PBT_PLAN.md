# Property-based testing plan for Percona Binary Log Server

Status: draft 1 (2026-10-14). The first property suite (`gtid_set`) and the
run/report tooling are implemented; everything else below is a proposal.

## 1. Goals and constraints

- Find defects that example-based unit tests and MTR miss. The method is
  [Hegel](https://hegel.dev/cpp), a property-based testing engine based on
  Hypothesis. It generates thousands of inputs per property and shrinks every
  failure to a minimal counterexample.
- Server sources stay untouched. The only exception is the
  `WITH_PROPERTY_TESTS` CMake switch in the top-level `CMakeLists.txt`.
  Tests that need a source change are recorded in `extra/qa/todo.md` instead
  of being written.
- Tests live in `tests/property/`. Helper scripts live in `extra/qa/`.
- Every property can be run on its own. One script runs them all and writes
  a report: pass/fail counts, new vs known issues, and `junit.xml` for
  Jenkins.
- The S3 endpoint is SeaweedFS (`weed mini`), installed and started from
  `WORKDIR`.

## 2. Architecture

```
tests/property/
  CMakeLists.txt              pbs_add_property_test(); finds or fetches hegel-cpp
  common/pbt_helpers.hpp      Boost.Test <-> Hegel glue, assertions, env knobs
  common/gtid_model.hpp       reference GTID-set model + generators (shared)
  gtid_set_property_test.cpp  first suite (23+ properties)
  <area>_property_test.cpp    one executable per area (see section 4)
extra/qa/
  pbt_env.sh                  shared settings (WORKDIR, preset, versions, ports)
  setup_pbt_env.sh            libhegel + hegel-cpp + SeaweedFS into WORKDIR
  build_property_tests.sh     configure/build with -DWITH_PROPERTY_TESTS=ON
  run_property_tests.py       run all properties, write report + junit
  seaweedfs.sh                start/stop/status/env/uri of the local S3 endpoint
  known_bugs.md               known (old) issues, used to classify failures
  todo.md                     tests blocked on server source changes
```

Design decisions:

- **Boost.Test hosts the properties.** The project already uses it. Each
  property is a `BOOST_AUTO_TEST_CASE` whose body calls
  `pbt::check(lambda)`. Properties therefore run like the existing unit
  tests: `--run_test=<name>`, `--list_content`, ctest label `property`.
  Hegel prints the minimal counterexample and a reproduction blob to
  stderr, then rethrows so that Boost.Test marks the case as failed.
- **One process per property in the runner.** A crash, abort or hang in
  one property cannot hide the others. There is a per-property timeout,
  and each property gets its own log file.
- **Assertion messages are stable.** Messages start with
  `PROPERTY VIOLATED: <text>`. They do not include generated values, so
  `known_bugs.md` can match them with a regex.
- **Case counts.** Each property sets its own default (5,000 to 20,000).
  The whole run can be scaled with `--cases-scale`, e.g. 0.1 for a smoke
  run per commit and 10 for a nightly run.
- **Hegel example database** (`WORKDIR/testruns/hegel-db`). Previously
  failing inputs are replayed first, so a known failure is found again at
  once and the run stays fast.
- **Independent oracles.** Expected values come from reference models that
  share no code with `src/`, from MySQL server sources and documentation,
  and from the README. Properties are not derived from the implementation.
- **Hegel build.** hegel-cpp 0.13.1 is built with `HEGEL_REFLECTION=OFF`, so
  there is no reflect-cpp download. libhegel 0.42.4 is checked against its
  SHA-256.

## 3. Run / report flow

```
extra/qa/setup_pbt_env.sh            # once per WORKDIR (idempotent)
extra/qa/build_property_tests.sh     # after every pull
extra/qa/run_property_tests.py       # -> WORKDIR/testruns/<timestamp>/
```

The report directory contains:

- `summary.md`: totals (passed, failed, errors, new issues, known issues,
  regressions, possibly fixed). For each new issue it has the message, the
  minimal counterexample and a copy-paste reproduce command.
- `summary.json`
- `junit.xml`. Known issues show as failures with a `[KNOWN <issue>]`
  prefix, or as skipped with `--known-as skipped`.
- `run_info.json`: git revision, preset and knobs.
- `logs/`

`WORKDIR/testruns/latest` points to the newest run. The exit code is
non-zero only for new issues, regressions or infrastructure errors
(`--fail-on new`), which makes it Jenkins friendly.

Proposed Jenkins wiring (later):
`setup_pbt_env.sh && build_property_tests.sh && run_property_tests.py --cases-scale 1`,
followed by `junit 'testruns/latest/junit.xml'` and archiving
`testruns/latest/**`.

### Build variants

Property tests generate hostile inputs, so they are most valuable under
sanitizers. Plain `debug` adds nothing that `asan` does not already cover,
because the asan preset is a Debug build.

| Variant | When | Why |
|---|---|---|
| `asan_gcc14` | every commit | memory errors that do not crash; `assert`s active |
| `release_gcc14` | nightly (`--cases-scale 10`) | shipped configuration; `assert`s off; optimiser-dependent UB |
| `asan_gcc14-ubsan` | nightly | integer overflow, invalid shifts, `isalpha` on negative `char`, ... |
| `tsan` | later (todo.md T6) | needed once multi-threaded properties exist |

Each variant has its own build directory. Reports and junit suite names
carry the variant name (`property.<variant>.<executable>`).

## 4. Test areas (ranked by risk x feasibility)

Legend:
- ✅ implemented
- 🟢 can be written now, no source change
- 🟡 can be written now with a workaround (todo.md entry)
- 🔴 needs a source change (todo.md entry)

### Layer A: in-process library properties (no MySQL, no S3)

| # | Area | Key properties | Oracle | State |
|---|---|---|---|---|
| A1 | `binsrv::gtids` (uuid, tag, gtid, gtid_set) | text and binary roundtrip; byte-exact MySQL encoding; model agreement over add/subtract/clear sequences; union/difference laws; malformed text and binary rejected | reference model, `rpl_gtid_set.cc`, MySQL manual | ✅ |
| A2 | `util` byte-span codecs: fixed, packed (`pack.cc`) and varlen ints | roundtrip for every width and signedness; `calculate_*_size` equals the bytes written; truncated input returns false without consuming; NULL marker 251 / 255 handling; narrowing extraction fails on overflow | MySQL encodings, re-implemented in the test | 🟢 |
| A3 | `events::composite_binlog_name` | `parse(str())` roundtrip; `next()` increments the sequence number, and 999999 throws; reject an empty base, `/` in the base, a non-6-digit suffix; base names with `\n` (they corrupt `binlog.index`) | README, `storage_core` index format | 🟢 |
| A4 | `opensslpp::cipher_context` | `decrypt(encrypt(x)) == x` for every cipher/mode/padding; for CTR, `create_with_offset(k)` applied to a suffix equals the same suffix of a full encryption (this is how encrypted binlogs are read at random offsets) | OpenSSL as reference | 🟢 |
| A5 | `events::event` / `event_view` / `rewriter` | `create_event` → view → encode is byte-identical; flipping any byte makes CRC verification fail; truncation is rejected; checksum add then remove is the identity; rewritten tagged GTID events still parse, and `transaction_length` stays consistent | MySQL event format | 🟢 |
| A6 | `events::reader_context` | generated valid streams following the documented grammar (`ROTATE FDE PREV_GTIDS? (GTID ANY*)* (ROTATE\|STOP)?`) are accepted; each single grammar mutation (dropped or duplicated FDE, wrong `next_event_position`, wrong `transaction_length`, ANONYMOUS in GTID mode) throws | the grammar in `reader_context.hpp` | 🟢 (stateful) |
| A7 | `util`: semantic_version, hex_value, ctime_timestamp(+range), flag_set, composite_name, crc32 | roundtrips; range is a monoid; crc32 matches zlib and `crc32_combine` | docs and zlib | 🟢 |
| A8 | `util::nv_tuple` JSON + `models::*` | `from_json(to_json(x)) == x`; unknown keys are ignored; type mismatch throws and names the JSON path | — | 🟢 |
| A9 | `size_unit`, `time_unit` | `"N<suffix>"` value equals N × multiplier; `to_string` roundtrip; overflow rejected; malformed suffixes rejected | README grammar | 🟡 (todo T1) |
| A10 | `main_config` + keyring + `storage_config` URIs | generated valid configs load; every single-rule mutation is rejected; `get_masked_uri()` never contains the secret | README config reference | 🟡 (T1) |

### Layer B: in-process storage (filesystem and S3)

| # | Area | Key properties | State |
|---|---|---|---|
| B1 | `storage` with filesystem backend (stateful model test) | random sequences of open/write/discard/flush/close/reopen. Persisted bytes equal the transactions written up to the last boundary. Record `size` equals the object size. `added_gtids` is the union of the GTIDs. `previous_gtids(n+1) == previous ∪ added (n)`. min/max timestamps hold. Reopening reproduces `get_binlog_records()`. Any chunking of `fetch_event_block` concatenates to the written bytes. | 🟡 (T1) |
| B2 | B1 against SeaweedFS S3 | backend equivalence: the same operation sequence on fs and s3 gives identical observable state | 🟡 (T1) |
| B3 | encryption at rest | bytes on disk differ from the plaintext; `fetch_event_block` at arbitrary offsets/lengths returns the plaintext for every KEK cipher; documented 192-bit + ECB/CBC restriction | 🟡 (T1) |
| B4 | crash recovery | junk appended to the tail file is truncated back; a short tail file is rejected; a size mismatch in a non-tail file is rejected; orphan objects are rejected | 🟡 (T1); fault injection 🔴 (T3) |
| B5 | checkpointing by size/interval | flush points obey the configured size and interval | 🔴 (T2, needs a clock seam) |

### Layer C: `binlog_server` binary on generated storage (no MySQL)

The storage directory is generated in-process (B1 machinery) or by a
standalone generator. The binary is then run and its JSON output is checked
against a model built from README semantics.

| # | Mode | Property | State |
|---|---|---|---|
| C1 | `list` | the result is exactly the index order with sizes and GTIDs; empty storage gives `success` with an empty result | 🟡 (T1 for the generator) |
| C2 | `search_by_gtid_set` | the result is the minimal file set covering the query (README); an error iff the query is not a subset; an empty query gives an empty result | 🟡 |
| C3 | `search_by_timestamp` | README: "files that have at least one event with timestamp <= T"; the "too old" and "empty" errors | 🟡 |
| C4 | `purge_binlogs` | README "≤ sequence number" semantics vs exact-name behaviour; the tail file is never purged; `list` afterwards equals the survivors | 🟡 |
| C5 | CLI argument rejection | malformed timestamps, GTID sets and binlog names give `status: error` and never crash | 🟢 (text-only arguments) |

### Layer D: system-level with MySQL (later; needs a MySQL on the QA host)

Hegel stateful tests drive a real server with random workloads:
- DML/DDL with GTID and tagged GTID on 8.4
- `FLUSH BINARY LOGS`, `PURGE BINARY LOGS`
- server restarts

During the workload `binlog_server pull`/`fetch` runs, and it is killed
with `kill -9` at random points and restarted. Properties:
- the stored binlog events equal the server's events (modulo rewrite)
- no duplicated or lost transactions across restarts
- `search_by_gtid_set` agrees with `@@gtid_executed`

These complement the existing MTR suite rather than replacing it.

### Recommended order

1. A1 ✅
2. A2 → A4 → A3 → A5 → A6. These need no infrastructure, are high risk
   (hand-written codecs, crypto offsets) and are cheap.
3. Decide on todo T1 (the `lib_binsrv` target), then A9/A10 and B1 to B4.
4. Layer C, which reuses B1's generators.
5. Layer D, once a MySQL is available on the QA host.

## 5. First results (gtid_set suite, prototype run)

The suite has 32 properties, and 13 of them fail. They correspond to 8
distinct defects, all listed in `known_bugs.md` as `PBT-001`…`PBT-008`:
- the text parser accepts trailing garbage and silently drops later UUID
  sets
- gno overflow and out-of-range values are accepted
- `add_interval`/`subtract_interval` do not validate the upper bound
- tags are case-sensitive, whereas MySQL lower-cases them
- binary decoding creates empty entries, accepts intervals MySQL rejects,
  accepts the nil UUID, and throws `bad_alloc` for over-long tags

## 6. Open questions

See the hand-over message; the answers will be folded in here.
