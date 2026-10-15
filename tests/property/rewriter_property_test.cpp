// Copyright (c) 2026 Percona and/or its affiliates.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2.0,
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License, version 2.0, for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

// Property-based tests for event materialization and rewriting
// (src/binsrv/events/rewriter.cpp).
//
// In rewrite mode (replication.rewrite in the configuration) the collector
// stores the transactions of the source under new binlog names: every
// event is relocated (next_event_position follows the new file offset),
// gets a checksum footer, and the GTID events get new sequence_number /
// last_committed values (renumbered from 1 in every new file, keeping the
// dependency distance). Properties:
//  - materialize / materialize_and_relocate add or remove exactly the
//    footer and change exactly event_size / next_event_position (byte
//    level oracle);
//  - rewrite() renumbers GTID_LOG / ANONYMOUS_GTID_LOG / GTID_TAGGED_LOG
//    events: sequence_number = last + 1, last_committed below it and never
//    less conservative than the source dependency, every other field
//    unchanged, GTID_TAGGED_LOG transaction_length adjusted for the new
//    size of the event itself;
//  - a model of collector_context in rewrite mode (one reader_context per
//    connection, generated preamble, rewritten transactions, switching
//    files at rewrite.file_size) accepts every valid source stream.

#define BOOST_TEST_MODULE RewriterPropertyTests

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "binsrv/replication_mode_type.hpp"

#include "binsrv/gtids/gtid.hpp"
#include "binsrv/gtids/gtid_set.hpp"

#include "binsrv/events/code_type.hpp"
#include "binsrv/events/common_header_flag_type.hpp"
#include "binsrv/events/common_types.hpp"
#include "binsrv/events/event_view.hpp"
#include "binsrv/events/gtid_tagged_log_body_impl.hpp"
#include "binsrv/events/reader_context.hpp"
#include "binsrv/events/rewriter.hpp"
#include "binsrv/events/rewriter_fwd.hpp"

#include "util/semantic_version.hpp"

#include "common/event_stream.hpp"
#include "common/pbt_helpers.hpp"

namespace {

namespace gs = hegel::generators;
namespace ev = binsrv::events;
namespace es = pbt::event_stream;
using es::ek;
using ev::materialization_type;

struct generated {
  es::stream_spec spec;
  es::plan items;
  es::encoded_stream stream;
};

generated draw_stream(hegel::TestCase &tc, es::stream_options options = {}) {
  generated result;
  result.spec = tc.draw("stream", es::stream_specs(options));
  result.items = es::make_plan(result.spec);
  result.stream = es::encode_plan(result.items, result.spec.verify_checksum,
                                  result.spec.connection_version);
  return result;
}

bool process(ev::reader_context &context, const es::bytes &data) {
  const ev::event_view view{context, es::cspan(data)};
  return context.process_event_view(view);
}

// the view of event #index as the collector sees it (the context has
// processed every earlier event)
ev::event_view view_of(const generated &gen, std::size_t index) {
  auto context{gen.spec.make_context()};
  for (std::size_t i{0U}; i < index; ++i) {
    std::ignore = process(context, gen.stream.events[i]);
  }
  return ev::event_view{context, es::cspan(gen.stream.events[index])};
}

std::size_t pick(hegel::TestCase &tc, const generated &gen,
                 const std::vector<ek> &kinds) {
  std::vector<std::size_t> candidates;
  for (std::size_t i{0U}; i < gen.items.size(); ++i) {
    if (std::ranges::find(kinds, gen.items[i].kind) != std::end(kinds)) {
      candidates.push_back(i);
    }
  }
  tc.assume(!candidates.empty());
  const auto index{candidates[tc.draw(
      "event", gs::integers<std::size_t>(
                   {.max_value = candidates.size() - 1U}))]};
  tc.note("events:\n" + es::describe(gen.items, gen.stream));
  tc.note("picked event #" + std::to_string(index));
  return index;
}

const std::vector<ek> all_kinds{
    ek::artificial_rotate, ek::format_description, ek::previous_gtids_log,
    ek::gtid_log,          ek::anonymous_gtid_log, ek::gtid_tagged_log,
    ek::other,             ek::rotate,             ek::stop};

// independent byte-level oracle for materialization: the event with the
// footer added / removed and event_size updated
es::bytes expected_materialized(const es::bytes &original, bool has_footer,
                                materialization_type mode) {
  es::bytes result{original};
  if (mode == materialization_type::force_add_checksum && !has_footer) {
    result.resize(result.size() + es::footer_size);
    es::put_le(result, es::event_size_offset, result.size(), 4U);
    es::fix_crc(result);
  } else if (mode == materialization_type::force_remove_checksum &&
             has_footer) {
    result.resize(result.size() - es::footer_size);
    es::put_le(result, es::event_size_offset, result.size(), 4U);
    return result;
  }
  // a copy that keeps its footer gets a freshly computed CRC (a source
  // FORMAT_DESCRIPTION with checksum_algorithm OFF may carry a stale one)
  if (has_footer) {
    es::fix_crc(result);
  }
  return result;
}

gs::Generator<materialization_type> materialization_types() {
  return gs::sampled_from<materialization_type>(
      {materialization_type::force_add_checksum,
       materialization_type::force_remove_checksum,
       materialization_type::leave_checksum_as_is});
}

gs::Generator<std::uint64_t> offsets() {
  return gs::one_of<std::uint64_t>(
      {gs::integers<std::uint64_t>({.min_value = 4U, .max_value = 1000U}),
       gs::integers<std::uint64_t>({.min_value = 4U, .max_value = 0x7FFFFFFFU})});
}

// sequence numbers of a local binlog file (MySQL: signed 64-bit, so far
// from any wrap-around)
gs::Generator<std::uint64_t> last_local_sequence_numbers() {
  return gs::one_of<std::uint64_t>(
      {gs::integers<std::uint64_t>({.max_value = 10U}),
       gs::integers<std::uint64_t>({.max_value = 1ULL << 40U})});
}

// The rules for the renumbered last_committed (lc) given the source
// sequence_number / last_committed and the new sequence_number:
//  - lc < sequence_number (MySQL's logical clock invariant);
//  - lc >= new_seq - (seq - source_lc): the rewritten transaction never
//    waits for less than the source said it must wait for;
//  - lc == new_seq - (seq - source_lc) when that is at least 1 (the
//    dependency distance is kept), otherwise 1 (0 for the first
//    transaction of the file).
void check_renumbering(std::uint64_t source_seq, std::uint64_t source_lc,
                       std::uint64_t last_local, std::uint64_t new_seq,
                       std::uint64_t new_lc) {
  pbt::require_equal(new_seq, last_local + 1U,
                     "sequence_number continues the local file");
  pbt::require(new_lc < new_seq, "last_committed < sequence_number");
  const auto distance{source_seq - source_lc};
  if (new_seq > distance) {
    pbt::require_equal(new_lc, new_seq - distance,
                       "last_committed keeps the dependency distance");
  } else {
    pbt::require_equal(new_lc, new_seq == 1U ? 0U : 1U,
                       "last_committed is 1 (0 for the first transaction) "
                       "when the dependency is before the file");
  }
}

} // namespace

// ===========================================================================
// materialize / materialize_and_relocate
// ===========================================================================

// materialize() copies the event, adding or removing exactly the 4-byte
// CRC32 footer as requested (leaving it when it is already as requested),
// with event_size updated and nothing else changed.
BOOST_AUTO_TEST_CASE(MaterializeChangesOnlyTheFooter) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto gen{draw_stream(tc)};
        const auto index{pick(tc, gen, all_kinds)};
        const auto mode{tc.draw("mode", materialization_types())};
        const auto view{view_of(gen, index)};
        ev::event_storage buffer;
        const ev::event_view result{
            ev::rewriter::materialize(view, buffer, mode)};
        const auto &original{gen.stream.events[index]};
        pbt::require_equal(
            es::hex(es::to_bytes(result.get_portion())),
            es::hex(expected_materialized(original, view.has_footer(), mode)),
            "materialized bytes");
      },
      {.test_cases = 2000U});
}

// Adding a footer and removing it again gives back the original event.
BOOST_AUTO_TEST_CASE(MaterializeAddThenRemoveIsIdentity) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto gen{draw_stream(tc)};
        const auto index{pick(tc, gen, all_kinds)};
        const auto view{view_of(gen, index)};
        tc.assume(!view.has_footer());
        ev::event_storage with_footer;
        const ev::event_view added{ev::rewriter::materialize(
            view, with_footer, materialization_type::force_add_checksum)};
        pbt::require(added.has_footer(), "footer added");
        pbt::require_equal(added.get_footer_view().get_crc_raw(),
                           added.calculate_crc(), "added footer CRC32");
        ev::event_storage without_footer;
        const ev::event_view removed{ev::rewriter::materialize(
            added, without_footer, materialization_type::force_remove_checksum)};
        pbt::require_equal(es::hex(es::to_bytes(removed.get_portion())),
                           es::hex(gen.stream.events[index]),
                           "add + remove footer is the identity");
      },
      {.test_cases = 1000U});
}

// materialize_and_relocate() additionally sets next_event_position to
// offset + new event size (and the CRC accordingly).
BOOST_AUTO_TEST_CASE(RelocateSetsNextEventPosition) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto gen{draw_stream(tc)};
        const auto index{pick(tc, gen, all_kinds)};
        const auto mode{tc.draw("mode", materialization_types())};
        const auto offset{tc.draw("offset", offsets())};
        const auto view{view_of(gen, index)};
        ev::event_storage buffer;
        const ev::event_view result{
            ev::rewriter::materialize_and_relocate(view, buffer, mode, offset)};
        auto expected{expected_materialized(gen.stream.events[index],
                                            view.has_footer(), mode)};
        es::put_le(expected, es::next_position_offset,
                   offset + expected.size(), 4U);
        if (result.has_footer()) {
          es::fix_crc(expected);
        }
        pbt::require_equal(es::hex(es::to_bytes(result.get_portion())),
                           es::hex(expected), "relocated bytes");
      },
      {.test_cases = 2000U});
}

// ===========================================================================
// rewrite
// ===========================================================================

// Non-GTID events are only relocated (with a footer added).
BOOST_AUTO_TEST_CASE(RewriteRelocatesNonGtidEvents) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto gen{draw_stream(tc)};
        const auto index{pick(
            tc, gen,
            {ek::artificial_rotate, ek::format_description,
             ek::previous_gtids_log, ek::other, ek::rotate, ek::stop})};
        const auto offset{tc.draw("offset", offsets())};
        const auto last{tc.draw("last_local", last_local_sequence_numbers())};
        const auto view{view_of(gen, index)};
        ev::event_storage buffer;
        const ev::event_view rewritten{
            ev::rewriter::rewrite(last, view, buffer, offset)};
        auto expected{expected_materialized(
            gen.stream.events[index], view.has_footer(),
            materialization_type::force_add_checksum)};
        es::put_le(expected, es::next_position_offset,
                   offset + expected.size(), 4U);
        es::fix_crc(expected);
        pbt::require_equal(es::hex(es::to_bytes(rewritten.get_portion())),
                           es::hex(expected),
                           "rewrite of a non-GTID event (footer added, "
                           "relocated)");
      },
      {.test_cases = 1000U});
}

// GTID_LOG / ANONYMOUS_GTID_LOG (fixed-size post header): the rewritten
// event equals the source event with new sequence_number / last_committed
// (see check_renumbering), next_event_position = offset + size and a valid
// CRC; transaction_length and every other byte are unchanged.
// Source events have footers (rewrite mode requires checksums).
BOOST_AUTO_TEST_CASE(RewriteRenumbersGtidLog) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto gen{draw_stream(tc, {.require_checksums = true})};
        const auto index{
            pick(tc, gen, {ek::gtid_log, ek::anonymous_gtid_log})};
        const auto offset{tc.draw("offset", offsets())};
        const auto last{tc.draw("last_local", last_local_sequence_numbers())};
        const auto view{view_of(gen, index)};
        ev::event_storage buffer;
        const ev::event_view result{
            ev::rewriter::rewrite(last, view, buffer, offset)};
        const auto actual{es::to_bytes(result.get_portion())};
        const auto &original{gen.stream.events[index]};
        pbt::require_equal(actual.size(), original.size(), "event size");

        // post header: flags(1) uuid(16) gno(8) ts_code(1) lc(8) seq(8)
        constexpr std::size_t lc_offset{es::header_size + 26U};
        constexpr std::size_t seq_offset{es::header_size + 34U};
        const auto &trx{gen.items[index].trx};
        const auto new_lc{es::get_le(actual, lc_offset, 8U)};
        const auto new_seq{es::get_le(actual, seq_offset, 8U)};
        check_renumbering(trx.sequence_number, trx.last_committed, last,
                          new_seq, new_lc);

        auto expected{original};
        es::put_le(expected, lc_offset, new_lc, 8U);
        es::put_le(expected, seq_offset, new_seq, 8U);
        es::put_le(expected, es::next_position_offset,
                   offset + expected.size(), 4U);
        es::fix_crc(expected);
        pbt::require_equal(es::hex(actual), es::hex(expected),
                           "rewritten bytes (only lc / seq / next position / "
                           "CRC may change)");
      },
      {.test_cases = 2000U});
}

// GTID_TAGGED_LOG (variable-length fields): the rewritten body equals the
// source body except sequence_number / last_committed (see
// check_renumbering) and transaction_length, which changes exactly by the
// change of the event's own size; event_size / next_event_position match
// the new size and the CRC is valid.
BOOST_AUTO_TEST_CASE(RewriteRenumbersGtidTaggedLog) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto gen{draw_stream(tc, {.require_checksums = true})};
        const auto index{pick(tc, gen, {ek::gtid_tagged_log})};
        const auto offset{tc.draw("offset", offsets())};
        const auto last{tc.draw("last_local", last_local_sequence_numbers())};
        const auto view{view_of(gen, index)};
        ev::event_storage buffer;
        const ev::event_view result{
            ev::rewriter::rewrite(last, view, buffer, offset)};
        const auto actual{es::to_bytes(result.get_portion())};
        const auto &original{gen.stream.events[index]};

        pbt::require_equal(es::get_le(actual, es::event_size_offset, 4U),
                           actual.size(), "event_size field");
        pbt::require_equal(es::get_le(actual, es::next_position_offset, 4U),
                           offset + actual.size(), "next_event_position");
        pbt::require(result.has_footer(), "footer present");
        pbt::require_equal(result.get_footer_view().get_crc_raw(),
                           result.calculate_crc(), "footer CRC32");
        constexpr auto prefix{
            static_cast<std::ptrdiff_t>(es::event_size_offset)};
        pbt::require_equal(
            es::hex(es::bytes(actual.begin(), actual.begin() + prefix)),
            es::hex(es::bytes(original.begin(), original.begin() + prefix)),
            "timestamp / type / server_id unchanged");
        pbt::require_equal(es::get_le(actual, es::flags_offset, 2U),
                           es::get_le(original, es::flags_offset, 2U),
                           "flags unchanged");

        const ev::generic_body_impl<ev::code_type::gtid_tagged_log> source{
            view.get_body_raw()};
        const ev::generic_body_impl<ev::code_type::gtid_tagged_log> rewritten{
            result.get_body_raw()};
        check_renumbering(source.get_sequence_number(),
                          source.get_last_committed(), last,
                          rewritten.get_sequence_number(),
                          rewritten.get_last_committed());
        pbt::require_equal(rewritten.get_transaction_length_raw() -
                               actual.size(),
                           source.get_transaction_length_raw() -
                               original.size(),
                           "transaction_length - event size (the rest of the "
                           "transaction) unchanged");
        auto expected{source};
        expected.set_sequence_number(rewritten.get_sequence_number());
        expected.set_last_committed(rewritten.get_last_committed());
        expected.set_transaction_length_raw(
            rewritten.get_transaction_length_raw());
        pbt::require(expected == rewritten,
                     "every other body field unchanged");
      },
      {.test_cases = 2000U});
}

// Source events whose last_committed is not below sequence_number violate
// MySQL's logical clock; rewrite() rejects them with std::logic_error.
BOOST_AUTO_TEST_CASE(RewriteRejectsLastCommittedNotBelowSequenceNumber) {
  pbt::check(
      [](hegel::TestCase &tc) {
        auto gen{draw_stream(tc, {.require_checksums = true})};
        const auto index{pick(
            tc, gen, {ek::gtid_log, ek::anonymous_gtid_log, ek::gtid_tagged_log})};
        auto &trx{gen.items[index].trx};
        trx.last_committed =
            trx.sequence_number +
            tc.draw("excess", gs::integers<std::uint64_t>({.max_value = 1000U}));
        gen.stream = es::encode_plan(gen.items, gen.spec.verify_checksum,
                                     gen.spec.connection_version);
        const auto view{view_of(gen, index)};
        const auto last{tc.draw("last_local", last_local_sequence_numbers())};
        pbt::require_throws<std::logic_error>(
            [&] {
              ev::event_storage buffer;
              std::ignore = ev::rewriter::rewrite(last, view, buffer, 4U);
            },
            "last_committed >= sequence_number must be rejected");
      },
      {.test_cases = 500U});
}

// ===========================================================================
// a whole rewritten stream
// ===========================================================================

namespace {

// A model of the storage as far as collector_context uses it in rewrite
// mode (binsrv::storage: open_binlog, write_event, positions and the last
// sequence number of the current file).
struct stored_file {
  std::uint32_t number{};
  std::vector<es::bytes> events{};
  std::vector<ev::seq_no_t> sequence_numbers{};
};

struct simulated_storage {
  std::vector<stored_file> files{};
  std::size_t current{0U};
  std::uint64_t position{0U};
  ev::seq_no_t last_sequence_number{0U};
  binsrv::gtids::gtid_set gtids{};
  std::vector<binsrv::gtids::gtid> stored_gtids{};

  void open(std::uint32_t number) {
    for (std::size_t i{0U}; i < files.size(); ++i) {
      if (files[i].number == number) {
        current = i;
        position = ev::magic_binlog_offset;
        for (const auto &event : files[i].events) {
          position += event.size();
        }
        last_sequence_number = files[i].sequence_numbers.empty()
                                   ? 0U
                                   : files[i].sequence_numbers.back();
        return;
      }
    }
    files.push_back(stored_file{.number = number});
    current = files.size() - 1U;
    position = ev::magic_binlog_offset;
    last_sequence_number = 0U;
  }
};

// collector_context::process_binlog_event()
void store(ev::reader_context &context, const ev::event_view &view,
           simulated_storage &storage, std::uint32_t opened_number = 0U) {
  const bool info_only{context.process_event_view(view)};
  const auto header{view.get_common_header_view()};
  const auto code{header.get_type_code()};
  const bool artificial{header.get_flags().has_element(
      ev::common_header_flag_type::artificial)};
  if (code == ev::code_type::rotate && artificial) {
    storage.open(opened_number);
  }
  if (info_only) {
    return;
  }
  auto &file{storage.files[storage.current]};
  file.events.push_back(es::to_bytes(view.get_portion()));
  storage.position += view.get_total_size();
  if (code == ev::code_type::gtid_log ||
      code == ev::code_type::anonymous_gtid_log ||
      code == ev::code_type::gtid_tagged_log) {
    file.sequence_numbers.push_back(context.get_transaction_sequence_number());
    storage.stored_gtids.push_back(context.get_transaction_gtid());
  }
  if (context.get_transaction_sequence_number() != 0U) {
    storage.last_sequence_number = context.get_transaction_sequence_number();
  }
  if (context.is_at_transaction_boundary() && context.has_transaction_gtid()) {
    storage.gtids += context.get_transaction_gtid();
  }
}

// one generated event (event_generation_helpers.cpp)
es::bytes generate(es::plan_event event, bool footer,
                   const std::string &version, std::uint64_t offset) {
  event.offset_before = static_cast<std::uint32_t>(offset);
  return es::encode_plan({event}, footer, version).events.front();
}

} // namespace

// A model of collector_context in rewrite mode (GTID replication, the
// source writes CRC32 checksums as README.md requires) with one
// reader_context for the connection, as in collector_context::fetch:
//  - every received event is viewed with that context;
//  - source ROTATE / FORMAT_DESCRIPTION / PREVIOUS_GTIDS_LOG / STOP events
//    are skipped;
//  - before the first transaction, and at a transaction boundary once the
//    file reached rewrite.file_size, ROTATE (non-artificial, not for the
//    first file), ROTATE(artificial), FORMAT_DESCRIPTION (CRC32) and
//    PREVIOUS_GTIDS_LOG (the stored GTIDs) are generated with
//    is_footer_expected() / the current server version of the context;
//  - every transaction event is rewritten at the end of the current file
//    with the file's last sequence number.
// The context must accept everything (otherwise the collector stops), for
// replication.verify_checksum both true and false; every file holds
// sequence numbers 1, 2, 3, ... and the stored GTIDs are the source GTIDs
// in order.
BOOST_AUTO_TEST_CASE(CollectorRewriteFlowIsAccepted) {
  pbt::check(
      [](hegel::TestCase &tc) {
        auto gen{draw_stream(tc, {.gtid_mode_only = true,
                                  .require_checksums = true,
                                  .allow_resume = false})};
        // empty storage: no binlog name, position 0
        gen.spec.context_binlog_name.clear();
        gen.spec.context_position = 0U;
        const auto file_size{tc.draw(
            "file_size",
            gs::sampled_from<std::uint64_t>({1024U, 2048U, 4096U, 1U << 20U}))};
        tc.note("source events:\n" + es::describe(gen.items, gen.stream));

        const std::string base_name{"rewritten"};
        simulated_storage storage;
        auto context{gen.spec.make_context()};
        std::vector<binsrv::gtids::gtid> source_gtids;
        for (std::size_t i{0U}; i < gen.items.size(); ++i) {
          const auto &item{gen.items[i]};
          if (es::is_transaction_head(item.kind)) {
            source_gtids.push_back(es::expected_gtid(item.trx));
          }
          try {
            const ev::event_view view{context, es::cspan(gen.stream.events[i])};
            if (item.kind != ek::other && !es::is_transaction_head(item.kind)) {
              continue;
            }
            if (context.is_fresh() || (context.is_at_transaction_boundary() &&
                                       storage.position >= file_size)) {
              const bool fresh{context.is_fresh()};
              const std::uint32_t number{
                  storage.files.empty()
                      ? 1U
                      : storage.files[storage.current].number +
                            (fresh ? 0U : 1U)};
              const auto version{
                  util::semantic_version{
                      context.get_current_encoded_server_version()}
                      .get_string()};
              if (!fresh) {
                es::plan_event rotate;
                rotate.kind = ek::rotate;
                rotate.timestamp = 1U;
                rotate.rotate_base = base_name;
                rotate.rotate_number = number;
                const auto data{generate(rotate, context.is_footer_expected(),
                                         version, storage.position)};
                store(context, ev::event_view{context, es::cspan(data)},
                      storage);
              }
              es::plan_event artificial;
              artificial.kind = ek::artificial_rotate;
              artificial.flags = es::artificial_flag;
              artificial.rotate_base = base_name;
              artificial.rotate_number = number;
              artificial.zero_next_position = true;
              const auto rotate_data{generate(artificial,
                                              context.is_footer_expected(),
                                              version, 4U)};
              store(context, ev::event_view{context, es::cspan(rotate_data)},
                    storage, number);
              es::plan_event fde;
              fde.kind = ek::format_description;
              fde.timestamp = 1U;
              fde.server_version = version;
              fde.fde_checksum = true;
              const auto fde_data{generate(fde, true, version, 4U)};
              store(context, ev::event_view{context, es::cspan(fde_data)},
                    storage);
              es::plan_event previous;
              previous.kind = ek::previous_gtids_log;
              previous.timestamp = 1U;
              previous.previous_gtid_set = storage.gtids;
              const auto previous_data{
                  generate(previous, true, version, 4U + fde_data.size())};
              store(context, ev::event_view{context, es::cspan(previous_data)},
                    storage);
            }
            ev::event_storage buffer;
            const ev::event_view rewritten{ev::rewriter::rewrite(
                storage.last_sequence_number, view, buffer, storage.position)};
            store(context, rewritten, storage);
          } catch (const std::exception &e) {
            tc.note("source event #" + std::to_string(i));
            pbt::fail(std::string{"rewrite flow rejected a valid source "
                                  "stream: "} +
                      e.what());
          }
        }

        pbt::require(storage.stored_gtids == source_gtids,
                     "stored GTIDs are the source GTIDs in order");
        for (const auto &file : storage.files) {
          for (std::size_t i{0U}; i < file.sequence_numbers.size(); ++i) {
            pbt::require_equal(file.sequence_numbers[i], ev::seq_no_t{i + 1U},
                               "sequence numbers 1, 2, 3, ... in every file");
          }
        }
      },
      {.test_cases = 2000U});
}
