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
- **Description** - one line; the minimal counterexample.

Workflow: when a run reports a new issue, reproduce it with the command
printed in `summary.md`, file it in Jira and add a row here (in the same
commit as any test change), so the next run counts it as known.

| Test | Match | Issue | Status | Description |
|---|---|---|---|---|
| `gtid_set_property_test::ParseRejectsTrailingGarbage` | | PBT-001 | open | GTID-set text parser ignores everything after the last valid token: `U:1abc` -> `U:1`, `U:1 , V:2` -> `U:1` (second UUID set silently lost). `gtid_set_parser::parse()` never checks that the input was fully consumed. |
| `gtid_set_property_test::ParseRejectsOutOfRangeGno` | | PBT-002 | open | gno not range-checked in text: `U:18446744073709551617` wraps to `U:1` (`parse_gno` has no overflow check, see TODO); `U:1-9223372036854775808` (> 2^63-1) accepted; `U:1-18446744073709551615` produces an entry without intervals (`str()` = `U`, not re-parseable). |
| `gtid_set_property_test::AddIntervalRejectsOutOfRangeUpperBound` | | PBT-003 | open | `gtid_set::add_interval()` validates only the lower bound: upper > 2^63-1 accepted, upper = 2^64-1 overflows `upper + 1` into an empty interval and leaves an empty TSID entry. `add(uuid, tag, gno)` rejects the same value. |
| `gtid_set_property_test::SubtractIntervalRejectsOutOfRangeUpperBound` | | PBT-003 | open | Same root cause as above for `subtract_interval()`: upper bound not validated, `upper = 2^64-1` turns the erase into a no-op. |
| `gtid_set_property_test::TagsAreCaseInsensitiveLikeMySQL` | | PBT-004 | open | `U:ABC:1` and `U:abc:1` are different sets in PBS, MySQL lower-cases tags (`mysql::gtid::Tag::replace`). An upper-case tag in a `search_by_gtid_set` argument never matches stored GTIDs. |
| `gtid_set_property_test::DecodeZeroIntervalTsidIsConsistent` | | PBT-005 | open | Binary decoding of a TSID with 0 intervals (MySQL accepts it) creates an empty map entry: `is_empty()` = false, `!= gtid_set{}`, `str()` = bare UUID which cannot be parsed back. `process_intervals()` uses `data_[uuid][tag]` before reading intervals. |
| `gtid_set_property_test::DecodeRejectsZeroLowerBound` | | PBT-006 | open | Binary decoding accepts interval start 0 (`U:0-4`); MySQL rejects `start <= last` with `last = 0`. |
| `gtid_set_property_test::DecodeRejectsBoundAboveMaxGno` | | PBT-006 | open | Binary decoding accepts interval bounds >= 2^63; MySQL reads them as signed and rejects them. |
| `gtid_set_property_test::DecodeRejectsUnsortedIntervals` | | PBT-006 | open | Binary decoding accepts unsorted / overlapping intervals (see `TODO: validate that interval boundary values are increasing` in `gtid_set.cpp`); MySQL rejects them. |
| `gtid_set_property_test::DecodeRejectsAdjacentIntervals` | | PBT-006 | open | Binary decoding accepts adjacent intervals `[a,b)[b,c)`; MySQL requires `start > previous end`. |
| `gtid_set_property_test::DecodeNilUuidIsConsistent` | | PBT-007 | open | Binary decoding accepts the nil UUID; the resulting set prints `00000000-0000-0000-0000-000000000000:1`, which the text parser (and `gtid` constructor) rejects. |
| `gtid_set_property_test::DecodeRejectsTooLongTag` | | PBT-008 | open | Encoded tag length > 32 throws `boost::container::bad_alloc` from `tag_storage::resize()` instead of `std::invalid_argument`. |
| `gtid_set_property_test::DecodeArbitraryBytesIsRejectedOrConsistent` | `bad_alloc` | PBT-008 | open | Same as above, reached through random byte mutation. |
| `gtid_set_property_test::DecodeArbitraryBytesIsRejectedOrConsistent` | `cannot be parsed back\|is_empty` | PBT-005 | open | Mutated encodings decoding into empty TSID entries / nil UUID (PBT-005, PBT-007). |
