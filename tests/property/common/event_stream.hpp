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

#ifndef TESTS_PROPERTY_COMMON_EVENT_STREAM_HPP
#define TESTS_PROPERTY_COMMON_EVENT_STREAM_HPP

// A model of the replication event stream that a MySQL source sends to PBS
// (COM_BINLOG_DUMP / COM_BINLOG_DUMP_GTID), used by the reader_context and
// rewriter properties.
//
// A stream is described on two levels:
//  - stream_spec: what happens (binlog files = "cycles", their server
//    versions and checksum settings, transactions and how each file ends),
//    drawn by stream_specs();
//  - plan: the flat list of events derived from a stream_spec by
//    make_plan(), together with what reader_context is expected to report
//    after each of them. Tests mutate the plan (drop / insert / change
//    events) to produce invalid streams.
// encode_plan() turns a plan into wire bytes the way the MySQL dump thread
// lays them out: checksum footers follow the last FORMAT_DESCRIPTION,
// next_event_position follows the file offsets, transaction_length covers
// the GTID event and every event up to the next GTID / ROTATE / STOP.
//
// Event bodies of interpreted events are produced by the library's own
// event::create_event (their encoding is covered by event_property_test);
// all other events ("other": QUERY, XID, ROWS, ...) are opaque bytes, as
// they are for PBS. Header fields are written by encode_plan() itself.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <hegel/hegel.h>

#include "binsrv/replication_mode_type.hpp"

#include "binsrv/gtids/common_types.hpp"
#include "binsrv/gtids/gtid.hpp"
#include "binsrv/gtids/gtid_set.hpp"
#include "binsrv/gtids/tag.hpp"
#include "binsrv/gtids/uuid.hpp"

#include "binsrv/events/checksum_algorithm_type.hpp"
#include "binsrv/events/code_type.hpp"
#include "binsrv/events/common_header_flag_type.hpp"
#include "binsrv/events/common_types.hpp"
#include "binsrv/events/composite_binlog_name.hpp"
#include "binsrv/events/event.hpp"
#include "binsrv/events/gtid_log_flag_type.hpp"
#include "binsrv/events/gtid_log_post_header.hpp"
#include "binsrv/events/protocol_traits.hpp"
#include "binsrv/events/reader_context.hpp"

#include "util/byte_span_fwd.hpp"
#include "util/crc_helpers.hpp"
#include "util/ctime_timestamp.hpp"
#include "util/semantic_version.hpp"
#include "util/timestamp_helpers.hpp"

#include "common/gtid_model.hpp"

namespace pbt::event_stream {

namespace gs = hegel::generators;
namespace ev = binsrv::events;
namespace model = pbt::gtid_model;

using bytes = std::vector<std::uint8_t>;

// ---------------------------------------------------------------------------
// byte helpers
// ---------------------------------------------------------------------------
inline std::string hex(const bytes &data) {
  static constexpr std::string_view digits{"0123456789abcdef"};
  std::string result;
  for (const auto value : data) {
    result += digits[value >> 4U];
    result += digits[value & 0x0FU];
  }
  return result;
}

inline bytes to_bytes(util::const_byte_span data) {
  bytes result(std::size(data));
  for (std::size_t i{0U}; i < std::size(data); ++i) {
    result[i] = std::to_integer<std::uint8_t>(data[i]);
  }
  return result;
}

inline util::const_byte_span cspan(const bytes &data) {
  return {// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
          reinterpret_cast<const std::byte *>(std::data(data)),
          std::size(data)};
}

inline void put_le(bytes &data, std::size_t offset, std::uint64_t value,
                   std::size_t width) {
  for (std::size_t i{0U}; i < width; ++i) {
    data[offset + i] = static_cast<std::uint8_t>(value >> (8U * i));
  }
}

inline std::uint64_t get_le(const bytes &data, std::size_t offset,
                            std::size_t width) {
  std::uint64_t result{0U};
  for (std::size_t i{0U}; i < width; ++i) {
    result |= std::uint64_t{data[offset + i]} << (8U * i);
  }
  return result;
}

// common header layout (MySQL binlog v4)
inline constexpr std::size_t header_size{19U};
inline constexpr std::size_t timestamp_offset{0U};
inline constexpr std::size_t type_code_offset{4U};
inline constexpr std::size_t event_size_offset{9U};
inline constexpr std::size_t next_position_offset{13U};
inline constexpr std::size_t flags_offset{17U};
inline constexpr std::size_t footer_size{4U};
inline constexpr std::uint16_t artificial_flag{0x0020U};

// recomputes the CRC32 footer
inline void fix_crc(bytes &data) {
  const auto crc{util::calculate_crc32(cspan(data).first(data.size() - 4U))};
  put_le(data, data.size() - 4U, crc, 4U);
}

inline binsrv::gtids::uuid to_uuid(const model::raw_uuid &raw) {
  binsrv::gtids::uuid_storage storage{};
  for (std::size_t i{0U}; i < raw.size(); ++i) {
    storage[i] = std::byte{raw[i]};
  }
  return binsrv::gtids::uuid{storage};
}

inline std::uint32_t encoded_version(const std::string &version) {
  return util::semantic_version{version}.get_encoded();
}

// ---------------------------------------------------------------------------
// stream specification
// ---------------------------------------------------------------------------
enum class ek : std::uint8_t {
  artificial_rotate,
  format_description,
  previous_gtids_log,
  gtid_log,
  anonymous_gtid_log,
  gtid_tagged_log,
  other,
  rotate,
  stop
};

inline constexpr std::array ek_names{
    "ROTATE(artificial)", "FORMAT_DESCRIPTION", "PREVIOUS_GTIDS_LOG",
    "GTID_LOG",           "ANONYMOUS_GTID_LOG", "GTID_TAGGED_LOG",
    "OTHER",              "ROTATE",             "STOP"};

inline bool is_transaction_head(ek kind) {
  return kind == ek::gtid_log || kind == ek::anonymous_gtid_log ||
         kind == ek::gtid_tagged_log;
}

// opaque events inside a transaction (QUERY, TABLE_MAP, ROWS, XID, ...)
struct other_spec {
  std::uint8_t code{};
  std::uint32_t timestamp{};
  bytes body{};
};

struct transaction_spec {
  ek kind{ek::gtid_log};
  model::raw_uuid uuid{};
  std::uint64_t gno{};
  std::string tag{};
  std::uint64_t last_committed{};
  std::uint64_t sequence_number{};
  std::uint64_t immediate_commit_us{};
  // present when the source is itself a replica
  std::optional<std::uint64_t> original_commit_us{};
  std::optional<std::string> original_server_version{};
  std::optional<std::uint64_t> commit_group_ticket{};
  std::uint32_t timestamp{};
  bool may_have_sbr{};
  std::vector<other_spec> others{};
};

enum class ending : std::uint8_t { none, rotate, stop };

// one binlog file
struct cycle_spec {
  std::uint32_t binlog_number{};
  std::string server_version{};
  bool checksum{};
  // binlog_checksum=NONE: the source changes log_pos / created of the
  // FORMAT_DESCRIPTION it sends without recomputing its CRC
  bool stale_fde_crc{};
  std::uint32_t server_id{};
  std::uint32_t timestamp{};
  std::vector<model::element> previous_gtids{};
  std::vector<transaction_spec> transactions{};
  ending end{ending::none};
};

struct stream_spec {
  binsrv::replication_mode_type mode{binsrv::replication_mode_type::gtid};
  std::string connection_version{};
  // reader_context(checksum_verification_enabled): whether the source was
  // asked for checksums (the first artificial ROTATE carries a footer)
  bool verify_checksum{};
  std::string base_name{};
  // what the storage tells reader_context (binlog name / position)
  std::string context_binlog_name{};
  std::uint32_t context_position{};
  // resuming inside the first binlog at context_position
  bool resume{};
  std::vector<cycle_spec> cycles{};

  [[nodiscard]] std::string binlog_name(std::uint32_t number) const {
    return ev::composite_binlog_name{base_name, number}.str();
  }
  [[nodiscard]] ev::reader_context make_context() const {
    return ev::reader_context{encoded_version(connection_version),
                              verify_checksum, mode, context_binlog_name,
                              context_position};
  }
};

inline std::ostream &operator<<(std::ostream &output, const stream_spec &obj) {
  output << "{mode="
         << (obj.mode == binsrv::replication_mode_type::gtid ? "gtid"
                                                             : "position")
         << ", connection " << obj.connection_version
         << ", verify=" << obj.verify_checksum << ", context='"
         << obj.context_binlog_name << "':" << obj.context_position
         << (obj.resume ? " (resume)" : "") << ", cycles=[";
  for (const auto &cycle : obj.cycles) {
    output << " {" << obj.binlog_name(cycle.binlog_number) << " "
           << cycle.server_version << " crc=" << cycle.checksum << " trx=[";
    for (const auto &trx : cycle.transactions) {
      output << " " << ek_names[static_cast<std::size_t>(trx.kind)] << "("
             << trx.sequence_number << "/" << trx.last_committed << ")+"
             << trx.others.size();
    }
    output << " ] end="
           << (cycle.end == ending::none
                   ? "none"
                   : (cycle.end == ending::rotate ? "rotate" : "stop"))
           << "}";
  }
  return output << " ]}";
}

// ---------------------------------------------------------------------------
// generators
// ---------------------------------------------------------------------------
inline const std::vector<std::string> &cycle_versions() {
  static const std::vector<std::string> versions{"8.0.43", "8.4.8", "9.1.0"};
  return versions;
}

inline bool supports_tagged(const std::string &version) {
  return encoded_version(version) >= encoded_version("8.4.0");
}

// codes of events PBS does not interpret, with the same post header length
// in every supported version
inline const std::vector<std::uint8_t> &other_codes() {
  static const std::vector<std::uint8_t> codes{2U,  5U,  13U, 14U, 16U, 19U,
                                               29U, 30U, 31U, 32U, 39U, 40U};
  return codes;
}

inline gs::Generator<other_spec> other_specs() {
  return gs::compose([](const hegel::TestCase &tc) {
    other_spec result;
    result.code = tc.draw(gs::sampled_from(other_codes()));
    result.timestamp = tc.draw(gs::integers<std::uint32_t>());
    result.body = tc.draw(gs::binary({.max_size = 24U}));
    return result;
  });
}

// Draws a valid stream: 1..3 binlog files, 0..4 transactions each, MySQL
// rules for positions, sequence numbers (restart at 1 in every file,
// last_committed < sequence_number), GTID kinds (no ANONYMOUS_GTID_LOG in
// GTID mode, GTID_TAGGED_LOG only from 8.4), server versions (never
// above the connection version, never downgraded).
// options.require_checksums makes every file use CRC32 (rewrite mode).
struct stream_options {
  bool gtid_mode_only{false};
  bool position_mode_only{false};
  bool require_checksums{false};
  bool allow_resume{true};
  bool force_resume{false};
  std::size_t max_cycles{3U};
  std::size_t max_transactions{4U};
  std::size_t max_others{4U};
};

inline gs::Generator<stream_spec> stream_specs(stream_options options = {}) {
  return gs::compose([options](const hegel::TestCase &tc) {
    stream_spec result;
    result.mode =
        (options.gtid_mode_only ||
         (!options.position_mode_only && tc.draw(gs::booleans())))
            ? binsrv::replication_mode_type::gtid
            : binsrv::replication_mode_type::position;
    const bool gtid_mode{result.mode == binsrv::replication_mode_type::gtid};
    result.base_name = tc.draw(gs::sampled_from<std::string>(
        {"binlog", "mysql-bin", "host_1-bin", "b"}));
    result.verify_checksum = tc.draw(gs::booleans());
    result.resume =
        options.force_resume || (options.allow_resume && tc.draw(gs::booleans()));

    const auto number_of_cycles{tc.draw(gs::integers<std::size_t>(
        {.min_value = 1U, .max_value = options.max_cycles}))};
    auto binlog_number{tc.draw(
        gs::integers<std::uint32_t>({.min_value = 1U, .max_value = 999990U}))};
    std::size_t version_index{
        tc.draw(gs::integers<std::size_t>({.max_value = 2U}))};
    for (std::size_t cycle_index{0U}; cycle_index < number_of_cycles;
         ++cycle_index) {
      cycle_spec cycle;
      cycle.binlog_number = binlog_number;
      binlog_number += 1U;
      // upgrades only: the version never goes down
      version_index = tc.draw(gs::integers<std::size_t>(
          {.min_value = version_index, .max_value = 2U}));
      cycle.server_version = cycle_versions()[version_index];
      cycle.checksum = options.require_checksums || tc.draw(gs::booleans());
      cycle.stale_fde_crc = !cycle.checksum && tc.draw(gs::booleans());
      cycle.server_id = tc.draw(gs::integers<std::uint32_t>());
      cycle.timestamp = tc.draw(gs::integers<std::uint32_t>(
          {.min_value = 1U}));
      cycle.previous_gtids = tc.draw(model::element_lists(3U));

      std::vector<ek> kinds{ek::gtid_log};
      if (!gtid_mode) {
        kinds.push_back(ek::anonymous_gtid_log);
      }
      if (supports_tagged(cycle.server_version)) {
        kinds.push_back(ek::gtid_tagged_log);
      }
      const auto number_of_transactions{tc.draw(gs::integers<std::size_t>(
          {.max_value = options.max_transactions}))};
      // sequence numbers restart at 1 in every file; a resumed file
      // continues where the stored part ended
      const std::uint64_t first_sequence_number{
          result.resume && cycle_index == 0U
              ? tc.draw(gs::integers<std::uint64_t>(
                    {.min_value = 2U, .max_value = 1000U}))
              : 1U};
      for (std::size_t i{0U}; i < number_of_transactions; ++i) {
        transaction_spec trx;
        trx.kind = tc.draw(gs::sampled_from(kinds));
        if (trx.kind != ek::anonymous_gtid_log) {
          trx.uuid = tc.draw(model::uuids());
          trx.gno = tc.draw(model::gnos());
        }
        if (trx.kind == ek::gtid_tagged_log) {
          // MySQL writes GTID_TAGGED_LOG only for tagged GTIDs
          trx.tag = tc.draw(model::tags().filter(
              [](const std::string &tag) { return !tag.empty(); }));
        }
        trx.sequence_number = first_sequence_number + i;
        trx.last_committed = tc.draw(
            gs::integers<std::uint64_t>({.max_value = trx.sequence_number - 1U}));
        trx.immediate_commit_us = tc.draw(gs::integers<std::uint64_t>(
            {.min_value = 946684800000000ULL, .max_value = 4102444800000000ULL}));
        if (tc.draw(gs::booleans())) {
          trx.original_commit_us = tc.draw(gs::integers<std::uint64_t>(
              {.min_value = 946684800000000ULL,
               .max_value = trx.immediate_commit_us}));
        }
        if (tc.draw(gs::booleans())) {
          trx.original_server_version = tc.draw(gs::sampled_from(
              std::vector<std::string>(cycle_versions().begin(),
                                       cycle_versions().begin() +
                                           static_cast<std::ptrdiff_t>(
                                               version_index + 1U))));
        }
        if (tc.draw(gs::booleans())) {
          trx.commit_group_ticket =
              tc.draw(gs::integers<std::uint64_t>({.min_value = 1U}));
        }
        trx.timestamp = tc.draw(gs::integers<std::uint32_t>({.min_value = 1U}));
        trx.may_have_sbr = tc.draw(gs::booleans());
        // every MySQL transaction has at least one more event (QUERY(BEGIN)
        // / XID, ...)
        trx.others = tc.draw(gs::vectors(
            other_specs(), {.min_size = 1U, .max_size = options.max_others}));
        cycle.transactions.push_back(std::move(trx));
      }
      cycle.end = tc.draw(gs::sampled_from<ending>(
          {ending::none, ending::rotate, ending::stop}));
      result.cycles.push_back(std::move(cycle));
    }

    // the connection version is the newest one (the source after upgrades)
    const auto &newest{result.cycles.back().server_version};
    std::vector<std::string> connection_versions{newest};
    for (const auto &candidate : {"8.4.8", "9.1.0", "9.4.0"}) {
      if (encoded_version(candidate) > encoded_version(newest)) {
        connection_versions.emplace_back(candidate);
      }
    }
    result.connection_version = tc.draw(gs::sampled_from(connection_versions));

    const auto &first{result.cycles.front()};
    const auto first_name{result.binlog_name(first.binlog_number)};
    if (result.resume) {
      // behind the stored preamble of the file
      result.context_binlog_name = first_name;
      result.context_position = tc.draw(gs::integers<std::uint32_t>(
          {.min_value = 1024U, .max_value = 0x7FFFFFFFU}));
    } else {
      // in position mode the source always starts with the requested file,
      // so only GTID mode can start at a file other than the stored one
      switch (tc.draw(gs::integers<int>(
          {.min_value = 0, .max_value = gtid_mode ? 2 : 1}))) {
      case 0: // empty storage
        result.context_binlog_name = "";
        result.context_position = tc.draw(gs::integers<std::uint32_t>(
            {.max_value = 0x7FFFFFFFU}));
        break;
      case 1: // the storage has an empty file with this name
        result.context_binlog_name = first_name;
        result.context_position =
            tc.draw(gs::sampled_from<std::uint32_t>({0U, 4U}));
        break;
      default: // the storage stopped at an older file
        result.context_binlog_name =
            first.binlog_number > 1U
                ? result.binlog_name(first.binlog_number - 1U)
                : std::string{"other-bin.000001"};
        result.context_position = tc.draw(gs::integers<std::uint32_t>(
            {.max_value = 0x7FFFFFFFU}));
        break;
      }
    }
    return result;
  });
}

// ---------------------------------------------------------------------------
// plan
// ---------------------------------------------------------------------------

// What reader_context must report after processing an event of a valid
// stream (std::nullopt: not checked).
struct expectation {
  bool info_only{false};
  std::optional<bool> boundary{};
  std::optional<binsrv::gtids::gtid> gtid{};
  std::optional<ev::seq_no_t> sequence_number{};
  // the event completes the preamble (FORMAT_DESCRIPTION /
  // PREVIOUS_GTIDS_LOG) of a binlog file
  bool ends_preamble{false};
};

struct plan_event {
  ek kind{};
  std::uint32_t timestamp{};
  std::uint32_t server_id{};
  std::uint16_t flags{};
  // ROTATE (both kinds)
  std::string rotate_base{};
  std::uint32_t rotate_number{};
  std::uint64_t rotate_position{ev::magic_binlog_offset};
  // FORMAT_DESCRIPTION
  std::string server_version{};
  bool fde_checksum{};
  bool stale_fde_crc{};
  // PREVIOUS_GTIDS_LOG (previous_gtid_set, when set, takes precedence)
  std::vector<model::element> previous_gtids{};
  std::optional<binsrv::gtids::gtid_set> previous_gtid_set{};
  // GTID events
  transaction_spec trx{};
  // added to the correct transaction_length
  std::int64_t length_delta{0};
  // transaction the event belongs to (-1: none); a head's
  // transaction_length covers every later event with the same id
  int trx_id{-1};
  // OTHER
  other_spec other{};

  // layout
  // sets the running file offset before placing this event
  std::optional<std::uint32_t> offset_before{};
  // next_event_position is 0 (artificial ROTATE, pseudo FORMAT_DESCRIPTION)
  bool zero_next_position{false};
  // whether the running file offset moves past this event
  bool advances{true};
  // added to the correct next_event_position
  std::uint32_t next_position_delta{0U};

  expectation expected{};
};

using plan = std::vector<plan_event>;

inline binsrv::gtids::gtid expected_gtid(const transaction_spec &trx) {
  if (trx.kind == ek::anonymous_gtid_log) {
    return {};
  }
  return binsrv::gtids::gtid{to_uuid(trx.uuid), binsrv::gtids::tag{trx.tag},
                             trx.gno};
}

// The events the source sends for a stream_spec.
inline plan make_plan(const stream_spec &spec) {
  plan result;
  const bool gtid_mode{spec.mode == binsrv::replication_mode_type::gtid};
  int next_trx_id{0};
  for (std::size_t cycle_index{0U}; cycle_index < spec.cycles.size();
       ++cycle_index) {
    const auto &cycle{spec.cycles[cycle_index]};
    const bool resumed{spec.resume && cycle_index == 0U};
    const bool position_resume{resumed && !gtid_mode};

    plan_event rotate;
    rotate.kind = ek::artificial_rotate;
    rotate.timestamp = 0U;
    rotate.server_id = cycle.server_id;
    rotate.flags = artificial_flag;
    rotate.rotate_base = spec.base_name;
    rotate.rotate_number = cycle.binlog_number;
    rotate.rotate_position =
        position_resume ? spec.context_position : ev::magic_binlog_offset;
    rotate.zero_next_position = true;
    rotate.advances = false;
    rotate.expected.info_only = true;
    result.push_back(rotate);

    plan_event fde;
    fde.kind = ek::format_description;
    fde.timestamp = cycle.timestamp;
    fde.server_id = cycle.server_id;
    fde.server_version = cycle.server_version;
    fde.fde_checksum = cycle.checksum;
    fde.stale_fde_crc = cycle.stale_fde_crc;
    fde.offset_before = static_cast<std::uint32_t>(ev::magic_binlog_offset);
    fde.expected.info_only = resumed;
    if (position_resume) {
      // the pseudo FORMAT_DESCRIPTION sent when resuming inside a file
      fde.zero_next_position = true;
      fde.advances = false;
      fde.expected.ends_preamble = true;
    }
    result.push_back(fde);

    if (!position_resume) {
      plan_event previous;
      previous.kind = ek::previous_gtids_log;
      previous.timestamp = cycle.timestamp;
      previous.server_id = cycle.server_id;
      previous.previous_gtids = cycle.previous_gtids;
      previous.expected.info_only = resumed;
      previous.expected.ends_preamble = true;
      result.push_back(previous);
    }

    // the first event after the preamble of a resumed file is at the
    // resume position
    std::optional<std::uint32_t> pending_offset;
    if (resumed) {
      pending_offset = spec.context_position;
    }

    for (const auto &trx : cycle.transactions) {
      const auto gtid{expected_gtid(trx)};
      const int trx_id{next_trx_id++};
      plan_event head;
      head.kind = trx.kind;
      head.trx_id = trx_id;
      head.timestamp = trx.timestamp;
      head.server_id = cycle.server_id;
      head.server_version = cycle.server_version;
      head.trx = trx;
      head.offset_before = std::exchange(pending_offset, std::nullopt);
      head.expected.boundary = trx.others.empty();
      head.expected.gtid = gtid;
      head.expected.sequence_number = trx.sequence_number;
      result.push_back(head);
      for (std::size_t i{0U}; i < trx.others.size(); ++i) {
        plan_event other;
        other.kind = ek::other;
        other.trx_id = trx_id;
        other.timestamp = trx.others[i].timestamp;
        other.server_id = cycle.server_id;
        other.other = trx.others[i];
        other.expected.boundary = (i + 1U == trx.others.size());
        other.expected.gtid = gtid;
        other.expected.sequence_number = trx.sequence_number;
        result.push_back(other);
      }
    }

    if (cycle.end != ending::none) {
      plan_event last;
      last.kind = cycle.end == ending::rotate ? ek::rotate : ek::stop;
      last.timestamp = cycle.timestamp;
      last.server_id = cycle.server_id;
      last.rotate_base = spec.base_name;
      last.rotate_number = cycle.binlog_number + 1U;
      last.offset_before = std::exchange(pending_offset, std::nullopt);
      last.expected.boundary = true;
      last.expected.gtid = binsrv::gtids::gtid{};
      last.expected.sequence_number = 0U;
      result.push_back(last);
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// encoding
// ---------------------------------------------------------------------------
struct encoded_stream {
  std::vector<bytes> events{};
  // file offset of every event
  std::vector<std::uint32_t> offsets{};
  // for transaction heads: the declared and the real transaction length
  std::vector<std::uint64_t> declared_length{};
  std::vector<std::uint64_t> real_length{};
};

namespace detail {

inline util::optional_high_resolution_time_point
original_commit(const transaction_spec &trx) {
  if (!trx.original_commit_us) {
    return std::nullopt;
  }
  return util::microseconds_to_high_resolution_time_point(
      *trx.original_commit_us);
}

inline util::optional_semantic_version
original_version(const transaction_spec &trx) {
  if (!trx.original_server_version) {
    return std::nullopt;
  }
  return util::semantic_version{*trx.original_server_version};
}

inline bytes create_typed(const plan_event &item, bool footer,
                          const std::string &version,
                          std::uint64_t transaction_length) {
  ev::event_storage buffer;
  const util::ctime_timestamp timestamp{
      static_cast<std::time_t>(item.timestamp)};
  const ev::common_header_flag_set no_flags{};
  const auto &trx{item.trx};
  switch (item.kind) {
  case ek::artificial_rotate:
  case ek::rotate:
    std::ignore = ev::event::create_event<ev::code_type::rotate>(
        0U, timestamp, item.server_id, no_flags,
        ev::generic_post_header<ev::code_type::rotate>{item.rotate_position},
        ev::generic_body<ev::code_type::rotate>{
            ev::composite_binlog_name{item.rotate_base, item.rotate_number}},
        footer, buffer);
    break;
  case ek::format_description: {
    const util::semantic_version server_version{item.server_version};
    std::ignore = ev::event::create_event<ev::code_type::format_description>(
        0U, timestamp, item.server_id, no_flags,
        ev::generic_post_header<ev::code_type::format_description>{
            ev::default_binlog_version, server_version, timestamp,
            ev::default_common_header_length,
            ev::reader_context::get_hardcoded_post_header_lengths(
                server_version.get_encoded())},
        ev::generic_body<ev::code_type::format_description>{
            item.fde_checksum ? ev::checksum_algorithm_type::crc32
                              : ev::checksum_algorithm_type::off},
        true, buffer);
    break;
  }
  case ek::previous_gtids_log: {
    binsrv::gtids::gtid_set gtids{item.previous_gtid_set.value_or(
        binsrv::gtids::gtid_set{})};
    for (const auto &element : item.previous_gtids) {
      gtids.add_interval(to_uuid(element.key.uuid),
                         binsrv::gtids::tag{element.key.tag},
                         element.range.lower, element.range.upper);
    }
    std::ignore = ev::event::create_event<ev::code_type::previous_gtids_log>(
        0U, timestamp, item.server_id, no_flags,
        ev::generic_post_header<ev::code_type::previous_gtids_log>{},
        ev::generic_body<ev::code_type::previous_gtids_log>{gtids}, footer,
        buffer);
    break;
  }
  case ek::gtid_log:
  case ek::anonymous_gtid_log: {
    ev::gtid_log_flag_set flags{};
    if (trx.may_have_sbr) {
      flags.set_element(ev::gtid_log_flag_type::may_have_sbr);
    }
    const ev::gtid_log_post_header post_header{
        flags,
        to_uuid(trx.uuid),
        trx.gno,
        ev::gtid_log_post_header::known_logical_ts_code,
        trx.last_committed,
        trx.sequence_number};
    const ev::gtid_log_body body{
        util::microseconds_to_high_resolution_time_point(
            trx.immediate_commit_us),
        original_commit(trx),
        transaction_length,
        util::semantic_version{version},
        original_version(trx),
        trx.commit_group_ticket};
    if (item.kind == ek::gtid_log) {
      std::ignore = ev::event::create_event<ev::code_type::gtid_log>(
          0U, timestamp, item.server_id, no_flags,
          ev::generic_post_header<ev::code_type::gtid_log>{post_header},
          ev::generic_body<ev::code_type::gtid_log>{body}, footer, buffer);
    } else {
      std::ignore = ev::event::create_event<ev::code_type::anonymous_gtid_log>(
          0U, timestamp, item.server_id, no_flags,
          ev::generic_post_header<ev::code_type::anonymous_gtid_log>{
              post_header},
          ev::generic_body<ev::code_type::anonymous_gtid_log>{body}, footer,
          buffer);
    }
    break;
  }
  case ek::gtid_tagged_log: {
    ev::gtid_log_flag_set flags{};
    if (trx.may_have_sbr) {
      flags.set_element(ev::gtid_log_flag_type::may_have_sbr);
    }
    std::ignore = ev::event::create_event<ev::code_type::gtid_tagged_log>(
        0U, timestamp, item.server_id, no_flags,
        ev::generic_post_header<ev::code_type::gtid_tagged_log>{},
        ev::generic_body<ev::code_type::gtid_tagged_log>{
            flags, to_uuid(trx.uuid), trx.gno, binsrv::gtids::tag{trx.tag},
            trx.last_committed, trx.sequence_number,
            util::microseconds_to_high_resolution_time_point(
                trx.immediate_commit_us),
            original_commit(trx), transaction_length,
            util::semantic_version{version}, original_version(trx),
            trx.commit_group_ticket},
        footer, buffer);
    break;
  }
  case ek::stop:
    std::ignore = ev::event::create_event<ev::code_type::stop>(
        0U, timestamp, item.server_id, no_flags,
        ev::generic_post_header<ev::code_type::stop>{},
        ev::generic_body<ev::code_type::stop>{}, footer, buffer);
    break;
  case ek::other:
    break;
  }
  return to_bytes(buffer);
}

inline bytes create_other(const plan_event &item, bool footer,
                          const std::string &version) {
  const auto encoded{encoded_version(version)};
  const auto post_header_length{ev::get_post_header_length_for_code(
      encoded, ev::reader_context::get_hardcoded_post_header_lengths(encoded),
      static_cast<ev::code_type>(item.other.code))};
  const std::size_t size{header_size + post_header_length +
                         item.other.body.size() + (footer ? footer_size : 0U)};
  bytes result(size, 0U);
  put_le(result, timestamp_offset, item.timestamp, 4U);
  result[type_code_offset] = item.other.code;
  put_le(result, 5U, item.server_id, 4U);
  put_le(result, event_size_offset, size, 4U);
  std::ranges::copy(item.other.body, std::begin(result) +
                                         static_cast<std::ptrdiff_t>(
                                             header_size + post_header_length));
  return result;
}

} // namespace detail

// Lays out and encodes a plan. initial_footer: whether events before the
// first FORMAT_DESCRIPTION carry a footer; initial_version: the version
// used for them (the connection version).
inline encoded_stream encode_plan(const plan &items, bool initial_footer,
                                  const std::string &initial_version) {
  const auto count{items.size()};
  encoded_stream result;
  result.events.resize(count);
  result.offsets.resize(count, 0U);
  result.declared_length.resize(count, 0U);
  result.real_length.resize(count, 0U);

  // footer and server version in effect for every event
  std::vector<bool> footers(count);
  std::vector<std::string> versions(count);
  {
    bool footer{initial_footer};
    std::string version{initial_version};
    for (std::size_t i{0U}; i < count; ++i) {
      footers[i] = items[i].kind == ek::format_description || footer;
      versions[i] = version;
      if (items[i].kind == ek::format_description) {
        footer = items[i].fde_checksum;
        version = items[i].server_version;
      }
    }
  }

  // every event except the transaction heads
  for (std::size_t i{0U}; i < count; ++i) {
    if (items[i].kind == ek::other) {
      result.events[i] = detail::create_other(items[i], footers[i], versions[i]);
    } else if (!is_transaction_head(items[i].kind)) {
      result.events[i] =
          detail::create_typed(items[i], footers[i], versions[i], 0U);
    }
  }
  // transaction heads: transaction_length = own size + the later events of
  // the same transaction; the head's own size depends on the (variable-length encoded)
  // value, so iterate until it is stable
  for (std::size_t i{0U}; i < count; ++i) {
    if (!is_transaction_head(items[i].kind)) {
      continue;
    }
    std::uint64_t tail{0U};
    for (std::size_t j{i + 1U}; j < count; ++j) {
      if (items[j].trx_id == items[i].trx_id && items[i].trx_id >= 0) {
        tail += result.events[j].size();
      }
    }
    std::uint64_t length{tail};
    bytes encoded;
    for (int iteration{0}; iteration < 8; ++iteration) {
      const auto declared{static_cast<std::uint64_t>(
          static_cast<std::int64_t>(length) + items[i].length_delta)};
      encoded = detail::create_typed(items[i], footers[i], versions[i],
                                     declared);
      result.declared_length[i] = declared;
      result.real_length[i] = encoded.size() + tail;
      if (length == encoded.size() + tail) {
        break;
      }
      length = encoded.size() + tail;
    }
    result.events[i] = std::move(encoded);
  }

  // header fields: offsets, next_event_position, timestamp, flags, CRC
  std::uint32_t offset{static_cast<std::uint32_t>(ev::magic_binlog_offset)};
  for (std::size_t i{0U}; i < count; ++i) {
    const auto &item{items[i]};
    auto &data{result.events[i]};
    if (item.offset_before) {
      offset = *item.offset_before;
    }
    result.offsets[i] = offset;
    const auto size{static_cast<std::uint32_t>(data.size())};
    const std::uint32_t next_position{
        (item.zero_next_position ? 0U : offset + size) +
        item.next_position_delta};
    put_le(data, timestamp_offset, item.timestamp, 4U);
    put_le(data, next_position_offset, next_position, 4U);
    put_le(data, flags_offset, item.flags, 2U);
    if (footers[i]) {
      fix_crc(data);
      if (item.stale_fde_crc) {
        data[data.size() - 1U] ^= 0xFFU;
      }
    }
    if (item.advances) {
      offset += size;
    }
  }
  return result;
}

inline std::string describe(const plan &items, const encoded_stream &stream) {
  std::ostringstream output;
  for (std::size_t i{0U}; i < items.size(); ++i) {
    const auto &item{items[i]};
    const auto &data{stream.events[i]};
    output << "  #" << i << " " << ek_names[static_cast<std::size_t>(item.kind)];
    if (item.kind == ek::other) {
      output << "(code " << int{item.other.code} << ")";
    }
    output << " @" << stream.offsets[i] << " size=" << data.size()
           << " next=" << get_le(data, next_position_offset, 4U) << " flags=0x"
           << std::hex << item.flags << std::dec;
    switch (item.kind) {
    case ek::artificial_rotate:
    case ek::rotate:
      output << " -> " << item.rotate_base << "."
             << std::string(6U - std::min<std::size_t>(
                                     6U, std::to_string(item.rotate_number)
                                             .size()),
                            '0')
             << item.rotate_number << ":" << item.rotate_position;
      break;
    case ek::format_description:
      output << " " << item.server_version << " crc=" << item.fde_checksum;
      break;
    case ek::gtid_log:
    case ek::anonymous_gtid_log:
    case ek::gtid_tagged_log:
      output << " " << model::uuid_to_string(item.trx.uuid) << ":"
             << item.trx.tag << ":" << item.trx.gno
             << " seq=" << item.trx.sequence_number
             << " lc=" << item.trx.last_committed
             << " len=" << stream.declared_length[i] << " (real "
             << stream.real_length[i] << ")";
      break;
    default:
      break;
    }
    output << "\n";
  }
  return output.str();
}

} // namespace pbt::event_stream

#endif // TESTS_PROPERTY_COMMON_EVENT_STREAM_HPP
