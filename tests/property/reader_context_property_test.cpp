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

// Property-based tests for the replication stream state machine
// (src/binsrv/events/reader_context.cpp).
//
// reader_context validates every event PBS receives against the grammar
//   ( ROTATE(artificial) FORMAT_DESCRIPTION PREVIOUS_GTIDS_LOG?
//     ((ANONYMOUS_GTID_LOG | GTID_LOG | GTID_TAGGED_LOG) <ANY>*)*
//     (ROTATE | STOP)? )+
// plus positions (next_event_position = position + event_size),
// transaction lengths, the resume rules for both replication modes and
// the "info only" preamble events that must not be written. The collector
// (collector_context.cpp) relies on what it reports: info_only decides
// whether an event is stored, is_at_transaction_boundary() decides which
// part of the event buffer may be flushed, get_transaction_gtid() which
// GTIDs the storage records.
//
// Streams are generated as a model of what the MySQL dump thread sends
// (common/event_stream.hpp). Properties:
//  - every generated valid stream is accepted and reader_context reports
//    the expected info_only / boundary / GTID / sequence number for every
//    event (both replication modes, resuming inside a binlog, several
//    binlog files with different server versions and checksum settings);
//  - every single grammar / position / length violation is rejected with
//    std::logic_error, at or after the violating event, never before;
//  - the preamble of a binlog ends at a transaction boundary, GTID events
//    are never artificial, positions wrap around at 4 GiB like MySQL's.

#define BOOST_TEST_MODULE ReaderContextPropertyTests

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "binsrv/replication_mode_type.hpp"

#include "binsrv/gtids/gtid.hpp"

#include "binsrv/events/event_view.hpp"
#include "binsrv/events/reader_context.hpp"

#include "common/event_stream.hpp"
#include "common/pbt_helpers.hpp"

namespace {

namespace gs = hegel::generators;
namespace ev = binsrv::events;
namespace es = pbt::event_stream;
using es::ek;

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

void note_stream(hegel::TestCase &tc, const es::plan &items,
                 const es::encoded_stream &stream) {
  tc.note("events:\n" + es::describe(items, stream));
}

// processes one event the way collector_context does
bool process(ev::reader_context &context, const es::bytes &data) {
  const ev::event_view view{context, es::cspan(data)};
  return context.process_event_view(view);
}

} // namespace

// ===========================================================================
// ACCEPT direction
// ===========================================================================

// Every valid stream is accepted, and after every event reader_context
// reports what the collector needs:
//  - info_only exactly for the artificial ROTATE and for the
//    FORMAT_DESCRIPTION / PREVIOUS_GTIDS_LOG re-sent when resuming inside
//    an already stored binlog;
//  - is_at_transaction_boundary() false inside a transaction, true after
//    its last event and after ROTATE / STOP;
//  - the GTID (with tag; empty for anonymous transactions) and sequence
//    number of the current transaction;
//  - after FORMAT_DESCRIPTION: its server version and checksum setting.
BOOST_AUTO_TEST_CASE(ValidStreamIsAccepted) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto gen{draw_stream(tc)};
        note_stream(tc, gen.items, gen.stream);
        auto context{gen.spec.make_context()};
        pbt::require(context.is_fresh(), "a new context is fresh");
        for (std::size_t i{0U}; i < gen.items.size(); ++i) {
          const auto &item{gen.items[i]};
          const auto &expected{item.expected};
          tc.note("event #" + std::to_string(i));
          bool info_only{};
          try {
            info_only = process(context, gen.stream.events[i]);
          } catch (const std::exception &e) {
            pbt::fail(std::string{"valid event rejected: "} + e.what());
          }
          pbt::require(!context.is_fresh(),
                       "context is not fresh after an event");
          pbt::require_equal(info_only, expected.info_only, "info_only");
          if (expected.boundary) {
            pbt::require_equal(context.is_at_transaction_boundary(),
                               *expected.boundary,
                               "is_at_transaction_boundary()");
          }
          if (expected.gtid) {
            pbt::require_equal(context.get_transaction_gtid(), *expected.gtid,
                               "get_transaction_gtid()");
            pbt::require_equal(context.has_transaction_gtid(),
                               !expected.gtid->is_empty(),
                               "has_transaction_gtid()");
          }
          if (expected.sequence_number) {
            pbt::require_equal(context.get_transaction_sequence_number(),
                               *expected.sequence_number,
                               "get_transaction_sequence_number()");
          }
          if (item.kind == ek::format_description) {
            pbt::require_equal(context.get_current_encoded_server_version(),
                               es::encoded_version(item.server_version),
                               "server version taken from FORMAT_DESCRIPTION");
            pbt::require_equal(context.is_footer_expected(), item.fde_checksum,
                               "footer expectation taken from "
                               "FORMAT_DESCRIPTION");
          }
        }
      },
      {.test_cases = 3000U});
}

// The preamble of a binlog file (FORMAT_DESCRIPTION, PREVIOUS_GTIDS_LOG)
// ends between transactions: is_at_transaction_boundary() must be true
// after it, as it is after ROTATE / STOP. The collector passes this flag
// to storage::write_event(); events written while it is false are not
// flushed by checkpoints and are dropped by
// discard_incomplete_transaction_events() when the connection times out.
BOOST_AUTO_TEST_CASE(PreambleEndsAtTransactionBoundary) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto gen{draw_stream(tc, {.allow_resume = false})};
        note_stream(tc, gen.items, gen.stream);
        auto context{gen.spec.make_context()};
        for (std::size_t i{0U}; i < gen.items.size(); ++i) {
          std::ignore = process(context, gen.stream.events[i]);
          if (gen.items[i].expected.ends_preamble) {
            tc.note("event #" + std::to_string(i));
            pbt::require(context.is_at_transaction_boundary(),
                         "is_at_transaction_boundary() after the preamble of "
                         "a binlog file");
          }
        }
      },
      {.test_cases = 500U});
}

// ===========================================================================
// REJECT direction
// ===========================================================================
namespace {

enum class mutation_kind : std::uint8_t {
  drop_format_description,
  drop_previous_gtids,
  duplicate_format_description,
  duplicate_previous_gtids,
  drop_transaction_head,
  wrong_transaction_length,
  wrong_next_position,
  anonymous_in_gtid_mode,
  artificial_flag_set,
  artificial_flag_cleared,
  artificial_rotate_timestamp,
  server_version_above_connection,
  rotate_position_not_magic,
  artificial_rotate_wrong_position,
  terminator_inside_transaction,
  orphan_event,
  tagged_gtid_in_old_version,
  empty_or_unexpected_gtid
};

constexpr std::array mutation_names{"drop_format_description",
                                    "drop_previous_gtids",
                                    "duplicate_format_description",
                                    "duplicate_previous_gtids",
                                    "drop_transaction_head",
                                    "wrong_transaction_length",
                                    "wrong_next_position",
                                    "anonymous_in_gtid_mode",
                                    "artificial_flag_set",
                                    "artificial_flag_cleared",
                                    "artificial_rotate_timestamp",
                                    "server_version_above_connection",
                                    "rotate_position_not_magic",
                                    "artificial_rotate_wrong_position",
                                    "terminator_inside_transaction",
                                    "orphan_event",
                                    "tagged_gtid_in_old_version",
                                    "empty_or_unexpected_gtid"};

struct mutation {
  mutation_kind kind{};
  std::size_t index{};
};

// last event index of the transaction with the given id
std::size_t transaction_end(const es::plan &items, std::size_t head) {
  std::size_t end{head};
  for (std::size_t j{head + 1U}; j < items.size(); ++j) {
    if (items[j].trx_id == items[head].trx_id) {
      end = j;
    }
  }
  return end;
}

std::vector<mutation> applicable_mutations(const es::stream_spec &spec,
                                           const es::plan &items) {
  std::vector<mutation> result;
  const auto count{items.size()};
  const bool gtid_mode{spec.mode == binsrv::replication_mode_type::gtid};
  const auto add{[&result](mutation_kind kind, std::size_t index) {
    result.push_back({kind, index});
  }};
  for (std::size_t i{0U}; i < count; ++i) {
    const auto &item{items[i]};
    const bool last{i + 1U == count};
    const bool in_transaction{item.trx_id >= 0};
    switch (item.kind) {
    case ek::artificial_rotate:
      add(mutation_kind::artificial_flag_cleared, i);
      add(mutation_kind::artificial_rotate_timestamp, i);
      add(mutation_kind::artificial_rotate_wrong_position, i);
      break;
    case ek::format_description:
      if (!last) {
        add(mutation_kind::drop_format_description, i);
      }
      add(mutation_kind::artificial_flag_set, i);
      add(mutation_kind::server_version_above_connection, i);
      for (std::size_t j{i + 1U};
           j < count && items[j].kind != ek::artificial_rotate; ++j) {
        if (items[j].kind == ek::gtid_tagged_log) {
          add(mutation_kind::tagged_gtid_in_old_version, i);
          break;
        }
      }
      break;
    case ek::previous_gtids_log:
      if (!last) {
        add(mutation_kind::drop_previous_gtids, i);
      }
      add(mutation_kind::artificial_flag_set, i);
      break;
    case ek::gtid_log:
    case ek::anonymous_gtid_log:
    case ek::gtid_tagged_log: {
      const auto end{transaction_end(items, i)};
      if (end != i) {
        add(mutation_kind::drop_transaction_head, i);
        add(mutation_kind::terminator_inside_transaction, i);
      }
      if (end + 1U < count) {
        add(mutation_kind::wrong_transaction_length, i);
      }
      if (gtid_mode && item.kind != ek::anonymous_gtid_log) {
        add(mutation_kind::anonymous_in_gtid_mode, i);
      }
      if (item.kind != ek::gtid_tagged_log) {
        add(mutation_kind::empty_or_unexpected_gtid, i);
      }
      if (end == i) {
        add(mutation_kind::orphan_event, i);
      }
      break;
    }
    case ek::other:
      add(mutation_kind::artificial_flag_set, i);
      if (transaction_end(items, i) != i) {
        add(mutation_kind::terminator_inside_transaction, i);
      } else {
        add(mutation_kind::orphan_event, i);
      }
      break;
    case ek::rotate:
      add(mutation_kind::rotate_position_not_magic, i);
      add(mutation_kind::artificial_flag_set, i);
      break;
    case ek::stop:
      add(mutation_kind::artificial_flag_set, i);
      break;
    }
    if (in_transaction) {
      add(mutation_kind::duplicate_format_description, i);
      add(mutation_kind::duplicate_previous_gtids, i);
    }
    if (item.expected.ends_preamble) {
      add(mutation_kind::orphan_event, i);
    }
    // next_event_position is validated for every event except the
    // FORMAT_DESCRIPTION / PREVIOUS_GTIDS_LOG re-sent when resuming in GTID
    // mode (real file positions that are not written again)
    const bool unvalidated_preamble{item.expected.info_only &&
                                    item.kind != ek::artificial_rotate &&
                                    !item.zero_next_position};
    if (!unvalidated_preamble) {
      add(mutation_kind::wrong_next_position, i);
    }
  }
  return result;
}

es::plan_event other_event(const es::plan_event &neighbour) {
  es::plan_event result;
  result.kind = ek::other;
  result.server_id = neighbour.server_id;
  result.timestamp = neighbour.timestamp;
  result.other.code = 16U; // XID
  result.other.body = es::bytes(8U, 0x42U);
  return result;
}

// Applies the mutation; returns the index of the first event that differs
// from the valid stream (all events before it must still be accepted).
std::size_t apply_mutation(hegel::TestCase &tc, const es::stream_spec &spec,
                           es::plan &items, const mutation &chosen) {
  const auto index{chosen.index};
  auto &item{items[index]};
  const auto insert_after{[&items](std::size_t position,
                                   const es::plan_event &event) {
    items.insert(items.begin() + static_cast<std::ptrdiff_t>(position + 1U),
                 event);
    return position + 1U;
  }};
  switch (chosen.kind) {
  case mutation_kind::drop_format_description:
  case mutation_kind::drop_previous_gtids:
  case mutation_kind::drop_transaction_head:
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
    return index;
  case mutation_kind::duplicate_format_description:
  case mutation_kind::duplicate_previous_gtids: {
    const auto wanted{chosen.kind ==
                              mutation_kind::duplicate_format_description
                          ? ek::format_description
                          : ek::previous_gtids_log};
    es::plan_event copy;
    for (std::size_t j{index + 1U}; j-- > 0U;) {
      if (items[j].kind == wanted) {
        copy = items[j];
        break;
      }
    }
    copy.kind = wanted;
    copy.server_id = item.server_id;
    if (copy.server_version.empty()) {
      copy.server_version = spec.cycles.front().server_version;
    }
    copy.offset_before.reset();
    copy.zero_next_position = false;
    copy.advances = true;
    copy.trx_id = -1;
    copy.expected = {};
    return insert_after(index, copy);
  }
  case mutation_kind::wrong_transaction_length: {
    const auto delta{tc.draw("length_delta",
                             gs::one_of<std::int64_t>(
                                 {gs::integers<std::int64_t>(
                                      {.min_value = -64, .max_value = 64}),
                                  gs::sampled_from<std::int64_t>(
                                      {-4, 4, 1LL << 32, -(1LL << 20)})}))};
    tc.assume(delta != 0);
    item.length_delta = delta;
    return index;
  }
  case mutation_kind::wrong_next_position: {
    const auto delta{tc.draw(
        "next_position_delta",
        gs::one_of<std::uint32_t>(
            {gs::sampled_from<std::uint32_t>({1U, 4U, 0xFFFFFFFFU, 0xFFFFFFFCU}),
             gs::integers<std::uint32_t>({.min_value = 1U})}))};
    item.next_position_delta = delta;
    return index;
  }
  case mutation_kind::anonymous_in_gtid_mode:
    item.kind = ek::anonymous_gtid_log;
    item.trx.kind = ek::anonymous_gtid_log;
    item.trx.uuid = {};
    item.trx.gno = 0U;
    item.trx.tag.clear();
    return index;
  case mutation_kind::artificial_flag_set:
    item.flags = static_cast<std::uint16_t>(item.flags | es::artificial_flag);
    return index;
  case mutation_kind::artificial_flag_cleared:
    item.flags = static_cast<std::uint16_t>(item.flags & ~es::artificial_flag);
    return index;
  case mutation_kind::artificial_rotate_timestamp:
    item.timestamp =
        tc.draw("timestamp", gs::integers<std::uint32_t>({.min_value = 1U}));
    return index;
  case mutation_kind::server_version_above_connection:
    item.server_version = "9.9.0";
    return index;
  case mutation_kind::rotate_position_not_magic:
  case mutation_kind::artificial_rotate_wrong_position: {
    const auto delta{tc.draw("position_delta",
                             gs::integers<std::uint64_t>({.min_value = 1U}))};
    item.rotate_position += delta;
    return index;
  }
  case mutation_kind::terminator_inside_transaction: {
    es::plan_event terminator;
    terminator.kind = tc.draw("terminator", gs::booleans()) ? ek::stop
                                                            : ek::rotate;
    terminator.server_id = item.server_id;
    terminator.timestamp = 1U;
    terminator.rotate_base = spec.base_name;
    terminator.rotate_number = 999999U;
    return insert_after(index, terminator);
  }
  case mutation_kind::orphan_event:
    return insert_after(index, other_event(item));
  case mutation_kind::tagged_gtid_in_old_version:
    item.server_version = "8.0.43";
    return index;
  case mutation_kind::empty_or_unexpected_gtid:
    if (item.kind == ek::gtid_log) {
      item.trx.gno = 0U;
    } else {
      item.trx.uuid = es::model::raw_uuid{0x11, 0x22};
      item.trx.gno = 1U;
    }
    return index;
  }
  return index;
}

} // namespace

// A stream with a single violation is rejected: a missing / duplicated
// FORMAT_DESCRIPTION or PREVIOUS_GTIDS_LOG, a transaction without its GTID
// event, a wrong transaction_length, a wrong next_event_position, an
// ANONYMOUS_GTID_LOG in GTID mode, a misplaced artificial flag, an
// artificial ROTATE with a timestamp or an unexpected position, a
// FORMAT_DESCRIPTION newer than the server, a terminating ROTATE whose
// position is not 4, ROTATE / STOP inside a transaction, an event outside
// any transaction, a GTID_TAGGED_LOG in a binlog written by 8.0, an empty
// GTID in GTID_LOG / a non-empty one in ANONYMOUS_GTID_LOG.
// Every event before the violation is still accepted; the violation is
// reported with an exception (never accepted, never a crash).
BOOST_AUTO_TEST_CASE(MutatedStreamIsRejected) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto spec{tc.draw("stream", es::stream_specs())};
        auto items{es::make_plan(spec)};
        const auto candidates{applicable_mutations(spec, items)};
        // first the kind (uniformly among the applicable ones), then the
        // event, so that rare kinds are not drowned by frequent ones
        std::vector<mutation_kind> kinds;
        for (const auto &candidate : candidates) {
          if (std::ranges::find(kinds, candidate.kind) == std::end(kinds)) {
            kinds.push_back(candidate.kind);
          }
        }
        const auto kind{kinds[tc.draw(
            "mutation_kind",
            gs::integers<std::size_t>({.max_value = kinds.size() - 1U}))]};
        std::vector<mutation> of_kind;
        std::ranges::copy_if(
            candidates, std::back_inserter(of_kind),
            [kind](const mutation &candidate) { return candidate.kind == kind; });
        const auto chosen{of_kind[tc.draw(
            "mutation_event",
            gs::integers<std::size_t>({.max_value = of_kind.size() - 1U}))]};
        tc.note(std::string{"mutation: "} +
                mutation_names[static_cast<std::size_t>(chosen.kind)] +
                " at event #" + std::to_string(chosen.index));
        const auto first_changed{apply_mutation(tc, spec, items, chosen)};
        const auto stream{es::encode_plan(items, spec.verify_checksum,
                                          spec.connection_version)};
        if (chosen.kind == mutation_kind::wrong_transaction_length) {
          // the head's size may change with the value; skip the rare case
          // where the wrong value happens to be right again
          tc.assume(stream.declared_length[chosen.index] !=
                    stream.real_length[chosen.index]);
        }
        note_stream(tc, items, stream);

        auto context{spec.make_context()};
        for (std::size_t i{0U}; i < items.size(); ++i) {
          try {
            std::ignore = process(context, stream.events[i]);
          } catch (const std::logic_error &e) {
            // PBS reports invalid streams with std::logic_error /
            // std::invalid_argument
            tc.note("rejected at event #" + std::to_string(i) + ": " +
                    e.what());
            pbt::require(i >= first_changed,
                         "an event before the violation was rejected");
            return;
          } catch (const std::exception &e) {
            pbt::fail(std::string{"violation reported with an unexpected "
                                  "exception type: "} +
                      e.what());
          }
        }
        pbt::fail("stream with a violation was accepted");
      },
      {.test_cases = 5000U});
}

// GTID events are never artificial (MySQL sets LOG_EVENT_ARTIFICIAL_F only
// on the ROTATE it generates); reader_context rejects the artificial flag
// on every other event of a transaction, so it must reject it on the
// GTID_LOG / ANONYMOUS_GTID_LOG / GTID_TAGGED_LOG that starts one too.
BOOST_AUTO_TEST_CASE(ArtificialGtidEventIsRejected) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto spec{tc.draw("stream", es::stream_specs())};
        auto items{es::make_plan(spec)};
        std::vector<std::size_t> heads;
        for (std::size_t i{0U}; i < items.size(); ++i) {
          if (es::is_transaction_head(items[i].kind)) {
            heads.push_back(i);
          }
        }
        tc.assume(!heads.empty());
        const auto head{heads[tc.draw(
            "head", gs::integers<std::size_t>({.max_value = heads.size() - 1U}))]};
        items[head].flags = es::artificial_flag;
        const auto stream{es::encode_plan(items, spec.verify_checksum,
                                          spec.connection_version)};
        note_stream(tc, items, stream);
        tc.note("artificial GTID event #" + std::to_string(head));
        auto context{spec.make_context()};
        for (std::size_t i{0U}; i <= head; ++i) {
          try {
            std::ignore = process(context, stream.events[i]);
          } catch (const std::exception &) {
            pbt::require(i == head, "an event before the GTID was rejected");
            return;
          }
        }
        pbt::fail("artificial GTID event was accepted");
      },
      {.test_cases = 500U});
}

// ===========================================================================
// 32-bit positions
// ===========================================================================

// next_event_position is a 32-bit field: MySQL writes the end offset of the
// event modulo 2^32, so in a binlog file larger than 4 GiB the positions
// wrap around (binlog files grow past max_binlog_size with a large
// transaction). Resuming in position mode at an offset just below 4 GiB,
// every event must be accepted, including the one that ends exactly at
// 4 GiB (next_event_position 0).
BOOST_AUTO_TEST_CASE(PositionsWrapAroundAt4GiB) {
  pbt::check(
      [](hegel::TestCase &tc) {
        auto spec{tc.draw("stream", es::stream_specs({.position_mode_only = true,
                                                     .force_resume = true,
                                                     .max_cycles = 1U}))};
        tc.assume(!spec.cycles.front().transactions.empty());
        auto items{es::make_plan(spec)};
        auto stream{es::encode_plan(items, spec.verify_checksum,
                                    spec.connection_version)};
        // events after the pseudo preamble (ROTATE(artificial),
        // FORMAT_DESCRIPTION), up to the end of the stream
        std::vector<std::uint64_t> ends;
        std::uint64_t total{0U};
        for (std::size_t i{2U}; i < items.size(); ++i) {
          total += stream.events[i].size();
          ends.push_back(total);
        }
        const auto chosen_end{ends[tc.draw(
            "event", gs::integers<std::size_t>({.max_value = ends.size() - 1U}))]};
        const auto shift{tc.draw("shift", gs::sampled_from<std::int64_t>(
                                              {0, 0, 0, 1, -1, 7, -13}))};
        // the chosen event ends at 2^32 + shift
        const auto position{static_cast<std::uint32_t>(
            (1ULL << 32U) - chosen_end + static_cast<std::uint64_t>(shift))};
        tc.assume(position > 1024U);
        spec.context_position = position;
        items = es::make_plan(spec);
        stream = es::encode_plan(items, spec.verify_checksum,
                                 spec.connection_version);
        note_stream(tc, items, stream);
        auto context{spec.make_context()};
        for (std::size_t i{0U}; i < items.size(); ++i) {
          try {
            std::ignore = process(context, stream.events[i]);
          } catch (const std::exception &e) {
            tc.note("event #" + std::to_string(i));
            pbt::fail(std::string{"event around the 4 GiB boundary rejected: "} +
                      e.what());
          }
        }
      },
      {.test_cases = 500U});
}
