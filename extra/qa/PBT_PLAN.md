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
extra/qa/run_property_tests.py       # -> WORKDIR/testruns/<timestamp>-<variant>/
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
`testruns/latest/**`. Decision: known bugs show as failures in Jenkins
(`--known-as failure`, the default); the build result is still decided by
the exit code (`--fail-on new`), so a run with only known issues is
"unstable" (red test cases) but not "failed".

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
| A2 | `util` byte-span codecs: fixed, packed (`pack.cc`) and varlen ints | roundtrip for every width and signedness; `calculate_*_size` equals the bytes written; truncated input returns false without consuming; NULL marker 251 / 255 handling; narrowing extraction fails on overflow | MySQL encodings, re-implemented in the test | ✅ |
| A3 | `events::composite_binlog_name` | `parse(str())` roundtrip; `next()` increments the sequence number, and 999999 throws; reject an empty base, `/` in the base, a non-6-digit suffix; base names with `\n` (they corrupt `binlog.index`) | README, `storage_core` index format | ✅ |
| A4 | `opensslpp::cipher_context` | `decrypt(encrypt(x)) == x` for every cipher/mode/padding; for CTR, `create_with_offset(k)` applied to a suffix equals the same suffix of a full encryption (this is how encrypted binlogs are read at random offsets) | OpenSSL as reference | ✅ |
| A5 | `events::event` / `event_view` / `rewriter` | `create_event` → view → encode is byte-identical; flipping any byte makes CRC verification fail; truncation is rejected; checksum add then remove is the identity; rewritten tagged GTID events still parse, and `transaction_length` stays consistent | MySQL event format | ✅ |
| A6 | `events::reader_context` | generated valid streams following the documented grammar (`ROTATE FDE PREV_GTIDS? (GTID ANY*)* (ROTATE\|STOP)?`) are accepted; each single grammar mutation (dropped or duplicated FDE, wrong `next_event_position`, wrong `transaction_length`, ANONYMOUS in GTID mode) throws | the grammar in `reader_context.hpp` | ✅ |
| A7 | `util`: semantic_version, hex_value, ctime_timestamp(+range), flag_set, composite_name, crc32 | roundtrips; range is a monoid; crc32 matches zlib and `crc32_combine` | docs and zlib | 🟢 |
| A8 | `util::nv_tuple` JSON + `models::*` | `from_json(to_json(x)) == x`; unknown keys are ignored; type mismatch throws and names the JSON path | — | 🟢 |
| A9 | `size_unit`, `time_unit` | `"N<suffix>"` value equals N × multiplier; `to_string` roundtrip; overflow rejected; malformed suffixes rejected | README grammar | ✅ (`units_property_test`) |
| A10 | `main_config` + keyring + `storage_config` URIs | generated valid configs load; every single-rule mutation is rejected; `get_masked_uri()` never contains the secret | README config reference | ✅ |

### Layer B: in-process storage (filesystem and S3)

| # | Area | Key properties | State |
|---|---|---|---|
| B1 | `storage` with filesystem backend (stateful model test) | random sequences of open/write/discard/flush/close/reopen. Persisted bytes equal the transactions written up to the last boundary. Record `size` equals the object size. `added_gtids` is the union of the GTIDs. `previous_gtids(n+1) == previous ∪ added (n)`. min/max timestamps hold. Reopening reproduces `get_binlog_records()`. Any chunking of `fetch_event_block` concatenates to the written bytes. Also: `fetch_event_block` contract, `purge_binlogs` contract, `indexed_event_block` splitting | ✅ (T1 workaround; interval checkpoints need T2) |
| B2 | B1 against SeaweedFS S3 | backend equivalence: the same operation sequence on fs and s3 gives identical observable state | ✅ (`StorageMatchesModelOnS3`; the runner starts SeaweedFS) |
| B3 | encryption at rest | bytes on disk differ from the plaintext; `fetch_event_block` at arbitrary offsets/lengths returns the plaintext for every KEK cipher; documented 192-bit + ECB/CBC restriction | ✅ |
| B4 | crash recovery | junk appended to the tail file is truncated back; a short tail file is rejected; a size mismatch in a non-tail file is rejected; a crash while creating a binlog is recovered | ✅ (file-level); fault injection 🔴 (T3) |
| B5 | checkpointing by size/interval | flush points obey the configured size and interval | size ✅ (in B1); interval: only "not before it elapsed" ✅, the rest 🔴 (T2, needs a clock seam) |

### Layer C: `binlog_server` binary on generated storage (no MySQL)

The storage directory is generated in-process with `binsrv::storage`
(`tests/property/cli_property_test.cpp`: 0-4 binlogs, some without events,
GTIDs unique across the storage, optional purged GTIDs, non-decreasing
timestamps). The real `binlog_server` binary is then run (fork/exec) and its
JSON output is checked against a model built from README semantics. Every
run must print one JSON object, never crash, and exit 0 iff `status` is
`success`.

| # | Mode | Property | State |
|---|---|---|---|
| C1 | `list` | the result is exactly the index order with sizes and GTIDs; empty storage gives `success` with an empty result | ✅ |
| C2 | `search_by_gtid_set` | the result is the minimal file set covering the query (README); an error iff the query is not a subset; an empty query gives an empty result | ✅ |
| C3 | `search_by_timestamp` | README: "files that have at least one event with timestamp <= T"; the "too old" and "empty" errors | ✅ (PBT-031) |
| C4 | `purge_binlogs` | README "≤ sequence number" semantics vs exact-name behaviour; the tail file is never purged; `list` afterwards equals the survivors | ✅ |
| C5 | CLI argument rejection | malformed timestamps, GTID sets and binlog names give `status: error` and never crash | ✅ (PBT-032) |

### Layer D: system-level with MySQL

`tests/property/mysql_property_test.cpp` runs against a private MySQL
server (`extra/qa/mysqld.sh`: log_bin, gtid_mode=ON, CRC32, under
WORKDIR). Every case resets the binary logs and runs a random workload:
multi-row inserts, multi-statement and rolled back transactions, updates,
deletes, rows up to 300 KB, CREATE / DROP TABLE, `FLUSH BINARY LOGS`,
`max_binlog_size` 4 KB / 16 KB / 1 GB (rotations inside the workload),
compressed transactions (`binlog_transaction_compression`), tagged GTIDs
(8.3+). `binlog_server` runs with a random configuration: position / GTID
mode, `verify_checksum`, `checkpoint_size`, `checkpoint_interval`.

The oracle is the server itself: the stored binlogs must be byte-identical
to the server's binlog files (with the in-use flag of the open binlog
cleared, as in the MTR test `binsrv`), and `list` must report the server's
binlogs, sizes, per-file previous / added GTIDs (`SHOW BINLOG EVENTS`,
`gtid_executed`) and min / max timestamps (those of the events in the
stored file). Rewritten binlogs are checked with an independent event
walker (magic, sizes, log_pos, CRC32, GTID order).

| # | Property | State |
|---|---|---|
| D1 | `FetchMatchesServerBinlogsMySQL`: one `fetch` copies the server's binlogs | ✅ |
| D1a | `FetchStoresBinlogWithoutTransactionsMySQL`: also a newest binlog without transactions | ✅ (PBT-020) |
| D2 | `RepeatedFetchResumesMySQL`: workload and `fetch` alternate; the storage matches after each fetch | ✅ |
| D2a | `GtidResumeStoresRotateEventMySQL`: GTID-mode resume after a rotation keeps the ROTATE | ✅ (PBT-033) |
| D3 | `KilledPullResumesMySQL`: `pull` killed (SIGKILL) / stopped (SIGTERM) and restarted during the workload; never exits on its own, stops on SIGTERM with exit code 0, its storage matches the server, also after a `fetch` | ✅ |
| D4 | `SearchByGtidSetMatchesServerMySQL`: the binlogs holding the queried GTIDs (from the server's GTID_LOG events) | ✅ |
| D5 | `RewriteModeKeepsTransactionsMySQL`: rewritten binlogs are well-formed, reach file_size, hold every transaction in the server's order | ✅ |
| D5a | `RewriteModeWithoutChecksumVerificationMySQL`: the same with verify_checksum = false | ✅ (PBT-022 confirmed) |
| D6 | `ListenerServesServerEventsMySQL`: a binlog dump (libmysqlclient MYSQL_RPL, COM_BINLOG_DUMP, CRC32 / NONE) from the `pull` listener at any event of any stored binlog receives exactly the events the MySQL server sends from there | ✅ |
| D7 | `ReplicaOfListenerMatchesSourceMySQL`: a position-based MySQL replica (`mysqld.sh --replica`, gtid_mode=ON, so the storage is in GTID mode: the listener reports GTID_MODE from it) of the listener catches up with the source (gtid_executed, CHECKSUM TABLE) | ✅ (PBT-034 fixed upstream) |
| D8 | `ServerRestartDuringPullMySQL`: the server is restarted (STOP) or killed with SIGKILL and restarted (crash recovery) while `pull` runs; `pull` reconnects, never exits, and its storage matches the server | ✅ |
| D8a | `CrashInBinlogWithoutTransactionsMySQL`: the server is killed while its newest binlog has no transaction; fetch afterwards | ✅ (PBT-035) |
| D10 | `FetchMatchesServerBinlogsOnS3MySQL`, `RepeatedFetchResumesOnS3MySQL`, `KilledPullResumesOnS3MySQL`, `ListenerServesServerEventsOnS3MySQL`: D1, D2, D3 and D6 with the S3 storage backend (SeaweedFS); the stored objects are read back independently of PBS through the SeaweedFS filer (plain HTTP, `PBT_S3_FILER`), each case in its own prefix (removed afterwards) | ✅ |
| D11 | encrypted storage: every MySQL property draws `storage.encryption` in a third of its cases (KEK AES-128-ECB / AES-256-CBC / AES-256-GCM / AES-192-CTR / CAMELLIA-128-CBC; data cipher AES-128/192/256-CTR, ARIA-128-CTR, CAMELLIA-256-CTR, within the README's KEK / key-length rule); the oracle decrypts every stored binlog independently (OpenSSL EVP, the test's keyring, the envelopes in `<binlog>.json`) before comparing it with the server's file. Covers fetch, resume, killed `pull`, server restarts, purge, rewrite, S3, the listener serving an encrypted storage and the real replica | ✅ |
| D12 | S3 outages (TCP proxy in the test between PBS and SeaweedFS, switched off / on): `S3OutageWhileCollectingStopsPullMySQL` (pull writing to S3 exits on its own with a failure exit code, no crash / hang; afterwards fetch resumes and the storage matches), `S3OutageAtStartupFailsMySQL` (fetch / pull / list fail cleanly, storage unchanged), `S3OutageWhileServingKeepsPullRunningMySQL` (a dump that needs S3 fails, pull keeps running, a new dump after the outage is exact). Expected behaviour: see "S3 outages" below | ✅ |
| D13 | `ListenerRejectsInvalidDumpRequestsMySQL`: a dump of a binlog the storage does not have, from a position beyond the end of a binlog or inside an event gets an error packet like from MySQL (1236), not a dropped connection or an empty successful dump | ✅ (PBT-037) |
| D14 | GTID_MODE other than ON (the server's mode is set per case at runtime, one step at a time; no extra server instance): `FetchMatchesServerBinlogsGtidOffMySQL`, `RepeatedFetchResumesGtidOffMySQL`, `KilledPullResumesGtidOffMySQL` (D1-D3 with a GTID_MODE=OFF server, anonymous transactions only, position-mode storage); `GtidModeTransitionsMySQL` (the mode changes between rounds through OFF / OFF_PERMISSIVE / ON_PERMISSIVE / ON, GTID and anonymous transactions mixed; position-mode storage stays a byte copy); `GtidStorageNeedsGtidOnServerMySQL` (a GTID-mode storage against a server not in ON: fetch / pull exit with a failure code, no crash, no endless retry, storage unchanged); `ReplicaOfPositionStorageGtidOffMySQL` (source, position-mode storage and replica all OFF: the replica reaches the source's binlog position, same CHECKSUM TABLE) | ✅ |
| D9 | `PurgeOnServerMySQL`: `PURGE BINARY LOGS TO` before / after the storage got the binlogs; fetch succeeds exactly when the server still has what the storage needs (position: its newest binlog; GTID: gtid_purged covered; empty storage), else exits 1 leaving the storage unchanged | ✅ |

GTID auto-positioning: the server starts a GTID dump at the newest binlog
whose PREVIOUS_GTIDS is a subset of the client's set, so binlogs without
transactions before that point are not sent. That is MySQL behaviour and
accepted by the oracle. PBT-020 / PBT-033 are tolerated in D1-D5 (exactly
those shapes, noted in the counterexample) and checked strictly by D1a /
D2a, so they do not hide other findings.

Not covered yet (later):
- GTID dumps (COM_BINLOG_DUMP_GTID) from the listener, once it supports
  them (PBT-034);

### Recommended order

1. A1 ✅
2. A2 → A4 → A3 → A5 → A6. These need no infrastructure, are high risk
   (hand-written codecs, crypto offsets) and are cheap.
3. T1 workaround in place (`pbs_test_binsrv`): B1 ✅, then B4, B3, B2,
   A9/A10.
4. Layer C, which reuses B1's generators. ✅
5. Layer D ✅ (first set; see the list of what is not covered yet).

## 5. Results

Findings so far, by severity (scale defined in `known_bugs.md`; the table
there is authoritative):

| Severity | Issues |
|---|---|
| high | PBT-015, PBT-017, PBT-022 |
| medium | PBT-001, PBT-002, PBT-004, PBT-012, PBT-013, PBT-016, PBT-019, PBT-026, PBT-029, PBT-030, PBT-031, PBT-032, PBT-033, PBT-035, PBT-036, PBT-037 |
| fixed | PBT-034 (upstream PBS-10) |
| low | PBT-003, PBT-005, PBT-006, PBT-007, PBT-008, PBT-009, PBT-010, PBT-011, PBT-014, PBT-018, PBT-020, PBT-021, PBT-023, PBT-024, PBT-025, PBT-027, PBT-028 |

### GTID sets (prototype run)

The suite has 32 properties, and 13 of them fail. They correspond to 8
distinct defects, all listed in `known_bugs.md` as `PBT-001`…`PBT-008`:
- [medium] PBT-001: the text parser accepts trailing garbage and silently
  drops later UUID sets
- [medium] PBT-002: gno overflow and out-of-range values are accepted
- [low] PBT-003: `add_interval`/`subtract_interval` do not validate the
  upper bound
- [medium] PBT-004: tags are case-sensitive, whereas MySQL lower-cases them
- [low] PBT-005…008: binary decoding creates empty entries, accepts
  intervals MySQL rejects, accepts the nil UUID, and throws `bad_alloc` for
  over-long tags

### Byte-span codecs and cipher context (sandbox run)

`byte_span_codec_property_test` has 31 properties: fixed, packed and
variable-length integers for every integer width, plus raw byte copies.
`cipher_context_property_test` has 14 properties: equivalence with
OpenSSL's EVP API, round trips, CTR random access through
`create_with_offset()`, GCM tampering, and validation. The two suites have
6 new findings, `PBT-009`…`PBT-014`:
- [low] PBT-009: the checked fixed-int functions don't validate the width
- [low] PBT-010: the byte-span copy functions pass a null pointer to
  `memcpy` for empty spans (found by UBSan)
- [low] PBT-011: the CTR offset context doesn't carry into the upper 64
  bits of the IV
- [medium] PBT-012: the CTR offset context rejects mixed-case cipher names
  that storage configuration accepts
- [medium] PBT-013: "supported" legacy ciphers can't be initialised in
  OpenSSL 3
- [low] PBT-014: `create_with_offset(0)` accepts non-CTR ciphers

The packed and variable-length integer codecs matched the MySQL reference
everywhere.

### Binlog names and events (sandbox run)

`composite_binlog_name_property_test` has 6 properties. `event_property_test`
has 12. The event properties cover:
- create → bytes → view → event round trips for 7 event types, both server
  versions, with and without checksums
- common header consistency
- CRC detection of every single-bit corruption
- rejection of truncated and extended events
- parse/encode round trips of mutated bodies
- forward compatibility of tagged GTID events

The two suites have 5 new findings, `PBT-015`…`PBT-019`:
- [high] PBT-015: 7-digit binlog names are rejected
- [medium] PBT-016: base names with line breaks or NUL are accepted
- [high] PBT-017: an unparseable FORMAT_DESCRIPTION server version
  terminates the process
- [low] PBT-018: `create_event<format_description>(include_checksum=false)`
  fails
- [medium] PBT-019: unknown trailing fields in tagged GTID events are
  rejected

From the adversarial review, waiting for later layers:
- `indexed_event_block` loops forever on `event_size < 19` (B4, needs T1)
- `binlog.index` order is not validated (B1)
- 32-bit position truncation for binlogs larger than 4 GiB: the event
  ending at 2^32 is PBT-023; resuming beyond 4 GiB is still open, because
  `collector_context` casts the storage position to 32 bits (B1 / D)
- `last_committed < sequence_number`: `rewriter` rejects violations;
  `reader_context` does not check it (low, MySQL never writes it)

### Encryption, S3, units (sandbox run)

The storage suite grew to 15 properties (`units_property_test` adds 4):
- encryption at rest:
  - the stateful model with every KEK cipher (ECB, CBC, GCM, CTR; AES,
    ARIA, CAMELLIA) and data cipher (128 / 192 / 256-bit CTR);
  - on-disk files decrypted independently with OpenSSL and the envelopes
    in the binlog metadata;
  - random-range reads;
  - the documented ECB/CBC + 192-bit rejection;
  - switching encryption on and off for an existing storage;
  - refusing to open without the KEK;
- the S3 backend (SeaweedFS): the same stateful model, with and without
  encryption. `run_property_tests.py --s3 auto` (the default) uses
  `PBT_S3_*` or starts SeaweedFS with `extra/qa/seaweedfs.sh`, then stops
  and wipes it. `--s3 off` skips these properties. A missing endpoint is
  reported as an infrastructure error (`PBT_INFRA:`), not as a finding;
- `size_unit` / `time_unit` (A9): the documented spellings, 64-bit
  overflow and the rejection of anything else;
- a long `checkpoint_interval`.

Encryption, S3 and the unit parsers matched their documentation.
There is 1 new finding:
- [low] PBT-028: a `checkpoint_interval` of 106752 days or more overflows
  the nanosecond clock duration, and every transaction is then flushed
  immediately. This is undefined behaviour.

### First full-scale QA run (asan_gcc14-ubsan, `--cases-scale 1`)

Earlier QA runs used `--cases-scale 0.1`. At the full case count,
`event_property_test::FormatDescriptionMutationsRoundTrip` found one more
defect:
- [low] PBT-027: re-encoding a parsed event keeps its old CRC footer, so a
  FORMAT_DESCRIPTION whose `server_version` has bytes after the NUL is
  re-encoded with an invalid checksum. No production path is affected.

Nightly runs should use scale ≥ 1; scale 0.1 is a smoke test.

### Storage (sandbox run)

`storage_property_test` is the first suite linked against
`pbs_test_binsrv`. That is a test-only library that compiles the
`binlog_server` sources (except `src/app.cpp`) a second time. It is the T1
workaround: no server change, and the source list, include directories and
link libraries are read from the `binlog_server` target.

The suite has 10 properties, all on the filesystem backend in a temporary
directory under the run directory:
- a stateful model of open / write / flush / discard / close / restart,
  checking the reported state after every step and the bytes on disk after
  every restart;
- the documented `fetch_event_block()` contract;
- the `purge_binlogs()` contract;
- purged GTIDs across a restart;
- crash recovery:
  - unrecorded bytes at the tail are truncated;
  - other size mismatches are rejected;
  - a crash while a binlog is being created;
- `querying_only` mode with broken metadata;
- `indexed_event_block` splitting;
- `indexed_event_block` on damaged data, run in a forked child with a
  50 ms timer and default signal handlers.

There are 3 new findings, `PBT-024`…`PBT-026`:
- [low] PBT-024: after a restart `get_purged_gtids()` returns the first
  binlog's added GTIDs instead of its previous GTIDs
- [low] PBT-025: `indexed_event_block` loops forever (Debug: aborts) on an
  `event_size` of 0 and indexes "events" of 1..18 bytes. A damaged stored
  binlog makes the 'pull' source hang and exhaust memory.
- [medium] PBT-026: a crash while a new binlog is being created leaves an
  unlisted binlog file, and the storage can no longer be opened

From the review, not turned into properties yet:
- `close_binlog()` with an incomplete transaction keeps its timestamps for
  the next binlog (Debug: an assert in `open_binlog`). The collector closes
  only at a transaction boundary, so this is unreachable today.
- `querying_only` mode calls `binlog_records_.front()` after every record
  was dropped for broken metadata (`storage_core.cpp`, end of
  `load_and_validate_binlog_metadata_set`). This is undefined behaviour,
  but harmless with libstdc++, because the vector keeps its memory and
  the destroyed optional reads as empty. The property passes, ASan
  included.
- `binlog.index` order, base names and the `previous_gtids` chain are not
  validated when loading. This is only reachable with a damaged storage.
- The filesystem backend never fsyncs streamed binlog data, only metadata
  and the index. After a power loss the metadata can be larger than the
  file, which is rejected. This needs layer D / fault injection (T3).
- Timestamp 0 (epoch) is ignored by `ctime_timestamp_range`. The model
  draws timestamps ≥ 1.

### Stream state machine and rewriter (sandbox run)

`tests/property/common/event_stream.hpp` models what the MySQL dump thread
sends (checked against `rpl_binlog_sender.cc` of MySQL 8.4):
- one to three binlog files, each with an artificial ROTATE,
  FORMAT_DESCRIPTION and PREVIOUS_GTIDS_LOG;
- transactions with GTID_LOG, ANONYMOUS_GTID_LOG or GTID_TAGGED_LOG plus
  opaque events, and ROTATE / STOP / nothing at the end of a file;
- server versions that only go up across files;
- per-file CRC32 or none;
- for the footer of the artificial ROTATE: the client setting first, then
  the previous FORMAT_DESCRIPTION;
- resuming inside a file in both replication modes: the pseudo
  FORMAT_DESCRIPTION with log_pos 0, and the stale FORMAT_DESCRIPTION CRC
  sent with `binlog_checksum=NONE`.

`reader_context_property_test` has 5 properties:
- valid streams are accepted with the expected `info_only` / boundary /
  GTID / sequence number after every event;
- 18 kinds of single violations are each rejected at or after the
  violating event, with `std::logic_error`;
- the preamble ends at a transaction boundary;
- artificial GTID events are rejected;
- positions wrap at 4 GiB.

`rewriter_property_test` has 8 properties:
- byte-level oracles for `materialize` / `materialize_and_relocate` /
  `rewrite`;
- sequence number renumbering;
- GTID_TAGGED_LOG `transaction_length` adjustment;
- a model of `collector_context` in rewrite mode (one `reader_context`
  per connection, generated preamble, file switching at
  `rewrite.file_size`).

There are 4 new findings, `PBT-020`…`PBT-023`:
- [low] PBT-020: `is_at_transaction_boundary()` is false after the preamble
  of a new binlog, so it is not checkpointed until the first transaction
- [low] PBT-021: the artificial flag is accepted on GTID events
- [high] PBT-022: rewrite mode with `verify_checksum = false` stops at the
  first transaction of every connection. Found with the collector model;
  confirm against a real server (layer D).
- [low] PBT-023: the event that ends exactly at 4 GiB is rejected

Not modelled, so for later:
- MySQL skipping transactions in GTID mode. The client already has them,
  and heartbeats are skipped (`MYSQL_RPL_SKIP_HEARTBEAT`). The next event's
  position then jumps, which `reader_context` rejects outside rewrite
  mode. That is expected for a source with the same file layout, but it
  needs a decision for failover to a source with a different layout (D).
- Reconnects in the collector model (fresh context, non-empty storage)
  (B / D).
- Transactions larger than 4 GiB are rejected ("too large"). MySQL itself
  recommends `max_binlog_cache_size` ≤ 4 GiB, so this is documented, not
  tested.

### Configuration and command line (sandbox run)

`config_property_test` (A10) builds valid configurations from the README
reference (every section, optional keys present or absent, file and S3
backends, rewrite and encryption) and applies one rule violation at a time
(missing required key, wrong JSON type, number out of range, unknown enum,
host / port / DNS combinations, rewrite in position mode, `file_size`
< 1024, malformed checkpoint values, non-CTR ciphers). All valid
configurations load and all 5000 single violations are rejected, and
`get_masked_uri()` never contains the key id or secret. Two gaps:
- [medium] PBT-029: `rewrite.base_file_name` is not validated on load
  (`""`, `a/b`, `/abs`); the collector fails only at the first transaction.
- [medium] PBT-030: unknown / misspelled keys are silently ignored, so
  `storage.encrypton` means plaintext binlogs.

`cli_property_test` (layer C) runs the real binary on generated storages.
`list`, `search_by_gtid_set` and `purge_binlogs` agree with the README
model (minimal covering set in storage order, "cannot be covered" for
purged / unknown GTIDs, purge up to but not including the given binlog, the
last binlog kept). Malformed GTID sets and binlog names are always rejected
with `status: error`. Two findings:
- [medium] PBT-031: `search_by_timestamp` returns binlogs without events
  (min timestamp 1970-01-01) for every query.
- [medium] PBT-032: invalid ISO timestamps are normalised instead of
  rejected (`T25:00:00` is the next day 01:00, `T14:60:00`, date only, `/`
  separators).

### MySQL server (sandbox run, MySQL 8.0.46)

All layer D properties pass apart from three known shapes:
- [low] PBT-020 seen for real: a `fetch` that ends before the first
  transaction of the newest binlog stores it as the 4-byte magic. In GTID
  mode this binlog is never completed (the next dump starts at a newer
  binlog after a rotation).
- [high] PBT-022 confirmed: rewrite mode with `verify_checksum = false`
  stops at the first transaction ("extra bytes in the gtid_log event
  body").
- [medium] PBT-033 (new): GTID mode, the storage has a binlog up to its
  last transaction, the server rotates: the next `fetch` / `pull` never
  stores that binlog's ROTATE (auto-positioning starts at the new binlog),
  so the stored file is 44 bytes shorter than the server's. MTR
  `resume_streaming` runs this sequence but does not compare files.

- [medium] PBT-033 also with a server shutdown: the old binlog's final
  STOP is never stored in GTID mode (D8).
- [medium] PBT-034 (new): a MySQL replica cannot replicate from the
  listener: its I/O thread stops at "get the value of SERVER_ID variable
  from source ... Syntax error" (the listener answers only a few fixed
  statements, no COM_REGISTER_SLAVE / COM_BINLOG_DUMP_GTID).
  `mysqlbinlog`-style dumps work, and D6 shows they are exact.
- [medium] PBT-035 (new): the server is killed while its newest binlog has
  no transaction; fetch / pull from it then opens the next binlog with the
  old preamble still pending: Debug builds abort (assertion
  `incomplete_transaction_timestamps_.is_empty()`, `storage.cpp:138`),
  Release builds give the next binlog the old binlog's `min_timestamp`.

Byte-for-byte copies otherwise hold in position and GTID mode with every
checkpoint setting, through rotations, large and compressed transactions,
repeated fetches, `pull` killed with SIGKILL at random points, server
restarts and crashes during `pull`, and binlogs purged on the server. The
listener serves exactly the server's event stream from any position. The
same holds with the S3 backend (fetch, repeated fetch, `pull` killed and
restarted, the listener serving from S3): no S3-specific difference found.

### After the rebase on upstream main (PBS-10: replica support in the listener)

Upstream added the required `replication_source.server_id` /
`server_uuid` keys and the optional `auth_rsa_private_key`, and the
listener now always runs `caching_sha2_password` full authentication (its
auth cache is a stub), so clients without TLS need the RSA key. The tests'
configurations were updated (that, not a product change, caused the 28
"new issues" of the first run after the rebase).
- [fixed] PBT-034: a position-based MySQL replica now replicates from the
  listener (D7 passes). GTID replicas are documented as not supported yet.
- [medium] PBT-036 (new): `server_uuid` with upper-case hex digits is
  accepted, although the README says only the lowercase form is
  (`is_canonical_uuid()` uses `isxdigit`).
- `config_property_test` covers the new keys (valid values load, malformed
  UUIDs, an empty `auth_rsa_private_key`, wrong types are rejected).

### Encrypted storage with MySQL (sandbox run)

No encryption-specific difference: decrypted stored binlogs are
byte-identical to the server's in every MySQL property, and the listener
serves exactly the server's events from an encrypted storage. Two known
bugs showed up in MySQL properties for the first time (the new encryption
draw changed the generated sequences):
- [medium] PBT-026 with the real binary: `pull` killed while it creates a
  new binlog cannot be restarted ("storage is not empty but does not
  contain binlog index"). This was the cause of the earlier `FLAKY`
  `KilledPullResumesMySQL` failures on QA.
- [medium] PBT-035 through a server crash right after `FLUSH BINARY LOGS`
  (`ServerRestartDuringPullMySQL`).

### S3 outages (expected behaviour, agreed with the project)

PBS relies on the AWS SDK's retries; there is no application-level loop
that waits for S3 to come back. After the SDK gives up (about 20-25 s in
the sandbox):

| Where S3 fails | What happens |
|---|---|
| Reading binlogs from S3 to serve a replication client | The exception is logged and that client's session is closed. The process keeps running (if the collector does not fail too); the client must reconnect. |
| Collecting binlogs from MySQL and writing them to S3 | The error is logged, the process stops and exits with a failure exit code. |
| Reading S3 during startup | Startup fails, the process exits with a failure exit code. |

D12 checks exactly this, plus that the storage stays consistent (a fetch
after the outage resumes and matches the server). All three properties
pass (sandbox). The README does not describe it yet (todo.md T8).

Observation (not filed as a bug, within the agreed behaviour): the
listener runs every session on the one `io_context` thread of `pull` and
reads S3 synchronously, so while the SDK retries one session's read (an
outage), every other replication client waits too; new sessions work again
once the SDK gives up on that read. The D12 serving property therefore
retries its dump after the outage (up to the SDK give-up time).

- [medium] PBT-037 (new, found while writing D12): the listener answers
  invalid dump requests (unknown binlog, position beyond the end, position
  inside an event) with a dropped connection or an empty successful dump
  instead of MySQL's error 1236 (D13).

### GTID_MODE other than ON (sandbox run)

No new finding. Position-mode storages are byte-for-byte copies with
anonymous transactions and across GTID mode changes; a GTID-mode storage
against a server that is not in ON fails cleanly with the server's error
("cannot start in AUTO_POSITION mode ...", exit code 1, storage
unchanged); a GTID_MODE=OFF replica replicates from a position-mode
storage. PBT-026 (`pull` killed while creating a binlog) also shows up in
`KilledPullResumesGtidOffMySQL` (covered by the `KilledPullResumes*MySQL`
row).

Test fix from the debug asan+ubsan sandbox run:
`ListenerServesServerEventsOnS3MySQL` failed once because the listener sent
3 events fewer than the server. The workload's final insert had itself
rotated the binlog (max_binlog_size 4096), so the newest binlog had no
transaction and was stored as the 4-byte magic only (PBT-020, known). The
listener cannot send a preamble it does not have. The listener properties
now add small inserts until the newest server binlog has a transaction,
which the property's comment already intended.

## 6. Open questions

See the hand-over message; the answers will be folded in here.
