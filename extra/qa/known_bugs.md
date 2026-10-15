# Known bugs found by the property-based tests

`extra/qa/run_property_tests.py` reads the table below to tell **old**
(known) issues from **new** ones:

- a failing property that matches a row with status `open` (or `wontfix`)
  is reported as a *known issue*;
- a failing property that matches no row is a *new issue* (the run fails
  with `--fail-on new`, the default);
- a failing property that matches a row with status `fixed` is a
  *regression*;
- a row with status `open` whose properties all passed is listed as
  *possibly fixed* - verify and set its status to `fixed`.

Columns:

- **Test** - `<executable>::<property>`; shell wildcards (`*`, `?`) allowed.
- **Match** - optional regular expression searched in the failure message and
  the Hegel counterexample report; empty matches any failure of the test.
  Use it when one property can fail for several different reasons.
- **Issue** - Jira key. `PBT-NNN` placeholders are local ids for findings
  that still have to be filed; replace them with the Jira key once filed.
- **Status** - `open`, `fixed` or `wontfix`.
- **Severity** - `high`, `medium` or `low` (see below). Reports show it next
  to every known issue and count known issues per severity; new issues are
  `untriaged` until a row is added here.
- **Description** - one line; the minimal counterexample.
- **Variant** - optional shell wildcard on the build variant (e.g.
  `*ubsan*` for a finding only a UBSan build can see); empty means every
  variant. Rows for other variants are ignored, so they are neither matched
  nor reported as "possibly fixed".

Severity scale (impact on a production PBS deployment, judged by how the
defect can be reached):

- **high** - crash / abort of the server process, data loss or corruption,
  or replication stopping, reachable with valid input (upstream MySQL
  server, stored binlogs, valid configuration).
- **medium** - wrong results or wrongly accepted / rejected input that a
  user can reach (command line arguments, search queries, configuration),
  or a failure that only unusual but valid conditions trigger.
- **low** - not reachable through current callers (API contract only),
  malformed input that MySQL never produces, undefined behaviour without an
  observable effect, or conditions that are astronomically unlikely.

Workflow: when a run reports a new issue, reproduce it with the command
printed in `summary.md`, file it in Jira and add a row here (in the same
commit as any test change), so the next run counts it as known.

| Test | Match | Issue | Status | Severity | Description | Variant |
|---|---|---|---|---|---|---|
| `gtid_set_property_test::ParseRejectsTrailingGarbage` | | PBT-001 | open | medium | GTID-set text parser ignores everything after the last valid token: `U:1abc` -> `U:1`, `U:1 , V:2` -> `U:1` (second UUID set silently lost). `gtid_set_parser::parse()` never checks that the input was fully consumed. | |
| `gtid_set_property_test::ParseRejectsOutOfRangeGno` | | PBT-002 | open | medium | gno not range-checked in text: `U:18446744073709551617` wraps to `U:1` (`parse_gno` has no overflow check, see TODO); `U:1-9223372036854775808` (> 2^63-1) accepted; `U:1-18446744073709551615` produces an entry without intervals (`str()` = `U`, not re-parseable). | |
| `gtid_set_property_test::AddIntervalRejectsOutOfRangeUpperBound` | | PBT-003 | open | low | `gtid_set::add_interval()` validates only the lower bound: upper > 2^63-1 accepted, upper = 2^64-1 overflows `upper + 1` into an empty interval and leaves an empty TSID entry. `add(uuid, tag, gno)` rejects the same value. | |
| `gtid_set_property_test::SubtractIntervalRejectsOutOfRangeUpperBound` | | PBT-003 | open | low | Same root cause as above for `subtract_interval()`: upper bound not validated, `upper = 2^64-1` turns the erase into a no-op. | |
| `gtid_set_property_test::TagsAreCaseInsensitiveLikeMySQL` | | PBT-004 | open | medium | `U:ABC:1` and `U:abc:1` are different sets in PBS, MySQL lower-cases tags (`mysql::gtid::Tag::replace`). An upper-case tag in a `search_by_gtid_set` argument never matches stored GTIDs. | |
| `gtid_set_property_test::DecodeZeroIntervalTsidIsConsistent` | | PBT-005 | open | low | Binary decoding of a TSID with 0 intervals (MySQL accepts it) creates an empty map entry: `is_empty()` = false, `!= gtid_set{}`, `str()` = bare UUID which cannot be parsed back. `process_intervals()` uses `data_[uuid][tag]` before reading intervals. | |
| `gtid_set_property_test::DecodeRejectsZeroLowerBound` | | PBT-006 | open | low | Binary decoding accepts interval start 0 (`U:0-4`); MySQL rejects `start <= last` with `last = 0`. | |
| `gtid_set_property_test::DecodeRejectsBoundAboveMaxGno` | | PBT-006 | open | low | Binary decoding accepts interval bounds >= 2^63; MySQL reads them as signed and rejects them. | |
| `gtid_set_property_test::DecodeRejectsUnsortedIntervals` | | PBT-006 | open | low | Binary decoding accepts unsorted / overlapping intervals (see `TODO: validate that interval boundary values are increasing` in `gtid_set.cpp`); MySQL rejects them. | |
| `gtid_set_property_test::DecodeRejectsAdjacentIntervals` | | PBT-006 | open | low | Binary decoding accepts adjacent intervals `[a,b)[b,c)`; MySQL requires `start > previous end`. | |
| `gtid_set_property_test::DecodeNilUuidIsConsistent` | | PBT-007 | open | low | Binary decoding accepts the nil UUID; the resulting set prints `00000000-0000-0000-0000-000000000000:1`, which the text parser (and `gtid` constructor) rejects. | |
| `gtid_set_property_test::DecodeRejectsTooLongTag` | | PBT-008 | open | low | Encoded tag length > 32 throws `boost::container::bad_alloc` from `tag_storage::resize()` instead of `std::invalid_argument`. | |
| `gtid_set_property_test::DecodeArbitraryBytesIsRejectedOrConsistent` | `bad_alloc` | PBT-008 | open | low | Same as above, reached through random byte mutation. | |
| `gtid_set_property_test::DecodeArbitraryBytesIsRejectedOrConsistent` | `cannot be parsed back\|is_empty` | PBT-005 | open | low | Mutated encodings decoding into empty TSID entries / nil UUID (PBT-005, PBT-007). | |
| `byte_span_codec_property_test::FixedIntCheckedRejectsZeroWidth` | | PBT-009 | open | low | `insert/extract_fixed_int_*_checked<unsigned T>` do not validate the width (only an `assert` in the unchecked function): width 0 returns `true` (Debug: assertion `bytes_to_insert != 0U`). | |
| `byte_span_codec_property_test::FixedIntCheckedRejectsOversizedWidth` | | PBT-009 | open | low | Same functions with width > `sizeof(T)` return `true` and copy past the integer / advance the span further than they copied (`uint8_t` width 2); Debug: assertion `bytes_to_insert <= sizeof(T)`. The signed variants reject this correctly. | |
| `byte_span_codec_property_test::ByteSpanCopyRoundTrip` | `null pointer` | PBT-010 | open | low | UBSan: `insert/extract_byte_span_*` call `memcpy` with a null pointer for empty spans (`std::span` of an empty `std::vector`), undefined behaviour even with length 0. Only visible in UBSan builds. | `*ubsan*` |
| `cipher_context_property_test::CtrOffsetHandlesCounterCarry` | | PBT-011 | open | low | `cipher_context::create_with_offset()` adds the block offset to the low 64 bits of the IV only; OpenSSL's CTR counter is the whole 128-bit IV, so when the low 64 bits wrap inside a file the offset context produces a different keystream than the full stream (encrypted binlog read/written at an offset decrypts to garbage). Needs an IV whose low 64 bits are within (file size / 16) of 2^64: astronomically unlikely with random IVs, but a correctness bug. | |
| `cipher_context_property_test::CtrOffsetAcceptsSameCipherNamesAsConstructor` | | PBT-012 | open | medium | `create_with_offset()` rejects mode spellings other than `CTR` / `ctr` (e.g. `AES-128-cTR`, `Aes-256-Ctr`) that OpenSSL, the constructor and `encryption_config` validation accept. With such a `storage.encryption.cipher` every event write (offset >= 4) throws "does not have the expected format". | |
| `cipher_context_property_test::SupportedCipherNamesAreUsable` | | PBT-013 | open | medium | `is_cipher_name_supported()` returns `true` for ciphers OpenSSL 3 only provides in the legacy provider (DES-CBC, BF-CBC, CAST5, SEED, RC2, ...), whose initialisation then fails ("digital envelope routines::unsupported"). The keyring validates KEK ciphers with it, so such a KEK is accepted and fails at the first encrypted binlog. | |
| `cipher_context_property_test::OffsetRejectsNonCtrCiphers` | | PBT-014 | open | low | `create_with_offset(0, ...)` accepts any cipher (early return before the CTR check) while every non-zero offset rejects non-CTR ciphers, so acceptance depends on the offset. | |
| `composite_binlog_name_property_test::ParseAcceptsEveryMySQLBinlogName` | | PBT-015 | open | high | `composite_binlog_name::parse()` accepts exactly 6 sequence digits, but MySQL continues `binlog.999999` with `binlog.1000000` (`%06lu`, up to 2^31-1). Upstream ROTATE event names are parsed with it (`rotate_body_impl.cpp`), so replication would stop at the millionth binlog. |  |
| `composite_binlog_name_property_test::ConstructorRejectsLineBreaksAndNul` | | PBT-016 | open | medium | Base names containing `\n`, `\r` or NUL are accepted (only `/` is checked). Names are stored one per line in `binlog.index` and used as file / object names, so such a name (e.g. from `rewrite.base_file_name`) corrupts the index. |  |
| `event_property_test::FormatDescriptionWithOddServerVersionNeverAborts` | | PBT-017 | open | high | A FORMAT_DESCRIPTION event whose `server_version` is not a parseable `major.minor.patch` (non-numeric, component > 255, e.g. `8.0.300`) terminates the process: `get_encoded_server_version()` is `noexcept` but `semantic_version(string)` throws (`format_description_post_header_impl.cpp:166`, called from the post header constructor). Should be an exception. |  |
| `event_property_test::FormatDescriptionMutationsRoundTrip` | `TERMINATE` | PBT-017 | open | high | Same as above, reached through random mutation of the FORMAT_DESCRIPTION post header. |  |
| `event_property_test::FormatDescriptionIgnoresChecksumFlag` | | PBT-018 | open | low | `event::create_event<format_description>(..., include_checksum=false, ...)` throws "invalid event footer length": the event size is computed with `include_checksum` before the constructor forces it to `true` for FORMAT_DESCRIPTION (`event.hpp`). All current callers pass `true`. |  |
| `event_property_test::GtidTaggedLogSkipsUnknownTrailingFields` | | PBT-019 | open | medium | GTID_TAGGED_LOG bodies with extra fields after the known ones (field id > 11, as a newer MySQL may write) are rejected ("unknown field_id"), although MySQL's serialization format says such ignorable fields are skipped using `serializable_field_size` (`last_non_ignorable_field_id` is 0). Forward-compatibility issue with newer upstream servers. |  |
| `reader_context_property_test::PreambleEndsAtTransactionBoundary` | | PBT-020 | open | low | After the FORMAT_DESCRIPTION / PREVIOUS_GTIDS_LOG of a new binlog `is_at_transaction_boundary()` is false (state `gtid_log_expected`), although it is true after ROTATE / STOP. The collector passes the flag to `storage::write_event()`, so the preamble of a new file is not checkpointed until its first transaction completes and `discard_incomplete_transaction_events()` drops it on a read timeout; the source re-sends it after reconnecting. |  |
| `reader_context_property_test::ArtificialGtidEventIsRejected` | | PBT-021 | open | low | GTID_LOG / ANONYMOUS_GTID_LOG / GTID_TAGGED_LOG events with the artificial flag are accepted (and written), while the flag is rejected on every other event of a transaction (`process_event_in_gtid_log_expected_state` does not check it). MySQL never sends such events. |  |
| `rewriter_property_test::CollectorRewriteFlowIsAccepted` | | PBT-022 | open | high | Rewrite mode with `replication.verify_checksum = false` (source with CRC32, as README requires) stops at the first transaction of every connection: PBS sends `@source_binlog_checksum='NONE'`, MySQL still sends the CRC footers (`rpl_binlog_sender.cc` only fails when the variable is unset), the source FORMAT_DESCRIPTION is skipped in rewrite mode, so the fresh `reader_context` still expects no footer and the first GTID event's CRC is parsed as body ("extra bytes in the gtid_log event body"). Found with a model of `collector_context` (rewrite_and_process_binlog_event); confirm against a real server. |  |
| `reader_context_property_test::PositionsWrapAroundAt4GiB` | | PBT-023 | open | low | In a binlog file larger than 4 GiB the event that ends exactly at offset 2^32 has `next_event_position` 0 (MySQL stores log_pos modulo 2^32) and `validate_position()` rejects it ("cannot be zero"); every other wrapped position is accepted. Needs a > 4 GiB binlog file (a transaction larger than `max_binlog_size`). |  |
| `storage_property_test::PurgedGtidsSurviveRestart` | | PBT-024 | open | low | After a restart `storage::get_purged_gtids()` returns the `added_gtids` of the first stored binlog instead of the GTIDs purged before it (its `previous_gtids`): `storage_core.cpp` `load_and_validate_binlog_metadata_set()` ("GTIDs from the first binlog metadata as purged GTIDs"). No caller uses the value after a restart today. |  |
| `storage_property_test::IndexedEventBlockTerminatesOnAnyInput` | | PBT-025 | open | low | `indexed_event_block` loops forever on a block whose next common header has `event_size` 0 (no progress, the index grows until memory is exhausted; Debug builds abort on an `assert`). Stored binlogs are not validated when read back, so a damaged binlog file makes the 'pull' replication source hang. Event sizes 1..18 are also accepted and misparse the block. |  |
| `storage_property_test::CrashWhileCreatingBinlogIsRecovered` | | PBT-026 | open | medium | A process killed while a new binlog is being created (`open_new_binlog_file_internal`: file created, magic written, metadata saved, then `binlog.index` saved) leaves a binlog file (with or without its `.json`) that `binlog.index` does not list, and the storage can no longer be opened: "storage is not empty but does not contain binlog index" for the first binlog, "storage contains an object that is not referenced in the binlog index" / "found metadata for a non-existing binlog" later. PBS cannot restart until the file is removed by hand. |  |
| `event_property_test::FormatDescriptionMutationsRoundTrip` | `checksum mismatch` | PBT-027 | open | low | `event::encode_to()` of a parsed event writes the CRC footer it was parsed with instead of recomputing it, so an event whose fields are normalised by parsing is re-encoded with an invalid checksum. Example: a FORMAT_DESCRIPTION with bytes after the NUL in `server_version` (MySQL zero-pads the field) re-encodes with those bytes cleared and the old CRC. No production path is affected today: the rewriter works on raw bytes, and `sender_context::transform_fde_to_artificial()` re-encodes the FDE post header through a write proxy, which recomputes the CRC. Found only at `--cases-scale 1`. |  |
| `storage_property_test::LongCheckpointIntervalDoesNotFlushEarly` | | PBT-028 | open | low | `storage.checkpoint_interval` of 106752 days (about 292 years) or more is accepted by `time_unit` but overflows when the storage converts it (`std::chrono::seconds{...}` assigned to the nanosecond `steady_clock::duration`, `storage.cpp`): the interval becomes negative / wraps and every complete transaction is flushed immediately (on S3: the whole binlog object is re-uploaded per transaction). Signed overflow, so undefined behaviour (UBSan reports it). |  |
