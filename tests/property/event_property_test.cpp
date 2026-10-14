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

// Property-based tests for binlog event serialization
// (src/binsrv/events: event, event_view, the post header / body
// implementations of every event type PBS interprets).
//
// Every event PBS receives is parsed with event_view / event and every
// event it generates (artificial ROTATE, FORMAT_DESCRIPTION,
// PREVIOUS_GTIDS_LOG, rewritten GTID events) is built with
// event::create_event. Properties:
//  - create -> bytes -> event_view -> event is the identity, and encoding
//    the parsed event reproduces the bytes (for every generated field
//    value, both server versions, with and without checksums);
//  - the common header is consistent (event size, next event position);
//  - CRC32 checksums detect every single-bit corruption;
//  - truncated / extended events are rejected;
//  - any event bytes the parser accepts (body mutations with a valid CRC)
//    survive a parse / encode / parse round trip;
//  - malformed fields make parsing fail with an exception, never abort
//    the process.

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#define BOOST_TEST_MODULE EventPropertyTests
// this include is needed as it provides the 'main()' function
// NOLINTNEXTLINE(misc-include-cleaner)
#include <boost/test/unit_test.hpp>

#include <boost/core/demangle.hpp>

#include "binsrv/replication_mode_type.hpp"

#include "binsrv/gtids/common_types.hpp"
#include "binsrv/gtids/gtid_set.hpp"
#include "binsrv/gtids/tag.hpp"
#include "binsrv/gtids/uuid.hpp"

#include "binsrv/events/checksum_algorithm_type.hpp"
#include "binsrv/events/code_type.hpp"
#include "binsrv/events/common_header.hpp"
#include "binsrv/events/common_header_flag_type.hpp"
#include "binsrv/events/common_types.hpp"
#include "binsrv/events/composite_binlog_name.hpp"
#include "binsrv/events/event.hpp"
#include "binsrv/events/event_view.hpp"
#include "binsrv/events/gtid_log_flag_type.hpp"
#include "binsrv/events/gtid_log_post_header.hpp"
#include "binsrv/events/protocol_traits_fwd.hpp"
#include "binsrv/events/reader_context.hpp"

#include "util/byte_span_fwd.hpp"
#include "util/crc_helpers.hpp"
#include "util/ctime_timestamp.hpp"
#include "util/semantic_version.hpp"
#include "util/timestamp_helpers.hpp"

#include "common/gtid_model.hpp"
#include "common/pbt_helpers.hpp"

namespace {

namespace gs = hegel::generators;
namespace ev = binsrv::events;
namespace model = pbt::gtid_model;
using ev::code_type;

using bytes = std::vector<std::uint8_t>;

std::string hex(const bytes &data) {
  static constexpr std::string_view digits{"0123456789abcdef"};
  std::string result;
  for (const auto value : data) {
    result += digits[value >> 4U];
    result += digits[value & 0x0FU];
  }
  return result;
}

bytes to_bytes(util::const_byte_span data) {
  bytes result(std::size(data));
  for (std::size_t i{0U}; i < std::size(data); ++i) {
    result[i] = std::to_integer<std::uint8_t>(data[i]);
  }
  return result;
}

util::const_byte_span cspan(const bytes &data) {
  return {// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
          reinterpret_cast<const std::byte *>(std::data(data)),
          std::size(data)};
}

binsrv::gtids::uuid to_uuid(const model::raw_uuid &raw) {
  binsrv::gtids::uuid_storage storage{};
  for (std::size_t i{0U}; i < raw.size(); ++i) {
    storage[i] = std::byte{raw[i]};
  }
  return binsrv::gtids::uuid{storage};
}

// ---------------------------------------------------------------------------
// event specification (what to generate)
// ---------------------------------------------------------------------------
enum class kind : std::uint8_t {
  rotate,
  format_description,
  previous_gtids_log,
  gtid_log,
  anonymous_gtid_log,
  gtid_tagged_log,
  stop
};

constexpr std::array kind_names{
    "rotate",   "format_description", "previous_gtids_log",
    "gtid_log", "anonymous_gtid_log", "gtid_tagged_log",
    "stop"};

struct gtid_fields {
  bool may_have_sbr{};
  model::raw_uuid uuid{};
  std::uint64_t gno{};
  std::string tag{};
  std::uint64_t last_committed{};
  std::uint64_t sequence_number{};
  std::uint64_t immediate_commit_us{};
  std::optional<std::uint64_t> original_commit_us{};
  std::uint64_t transaction_length{};
  std::optional<std::string> immediate_server_version{};
  std::optional<std::string> original_server_version{};
  std::optional<std::uint64_t> commit_group_ticket{};
};

struct event_spec {
  kind event_kind{};
  std::string server_version{};
  bool include_checksum{};
  std::uint32_t offset{};
  std::uint32_t timestamp{};
  std::uint32_t server_id{};
  std::uint16_t flags{};
  // rotate
  std::string binlog_base{};
  std::uint32_t binlog_number{};
  std::uint64_t position{};
  // format description
  bool fde_checksum{};
  // previous gtids
  std::vector<model::element> gtids{};
  // gtid events
  gtid_fields gtid{};
};

std::ostream &operator<<(std::ostream &output, const event_spec &obj) {
  output << "{" << kind_names[static_cast<std::size_t>(obj.event_kind)]
         << ", server " << obj.server_version
         << ", checksum=" << obj.include_checksum << ", offset=" << obj.offset
         << ", ts=" << obj.timestamp << ", server_id=" << obj.server_id
         << ", flags=0x" << std::hex << obj.flags << std::dec;
  switch (obj.event_kind) {
  case kind::rotate:
    output << ", " << obj.binlog_base << "." << obj.binlog_number << " @"
           << obj.position;
    break;
  case kind::format_description:
    output << ", fde_checksum=" << obj.fde_checksum;
    break;
  case kind::previous_gtids_log:
    output << ", gtids=" << model::to_model(obj.gtids).str();
    break;
  case kind::gtid_log:
  case kind::anonymous_gtid_log:
  case kind::gtid_tagged_log:
    output << ", uuid=" << model::uuid_to_string(obj.gtid.uuid)
           << ", gno=" << obj.gtid.gno << ", tag='" << obj.gtid.tag
           << "', lc=" << obj.gtid.last_committed
           << ", seq=" << obj.gtid.sequence_number
           << ", ict=" << obj.gtid.immediate_commit_us << ", oct="
           << (obj.gtid.original_commit_us
                   ? std::to_string(*obj.gtid.original_commit_us)
                   : "-")
           << ", len=" << obj.gtid.transaction_length
           << ", isv=" << obj.gtid.immediate_server_version.value_or("-")
           << ", osv=" << obj.gtid.original_server_version.value_or("-")
           << ", ticket="
           << (obj.gtid.commit_group_ticket
                   ? std::to_string(*obj.gtid.commit_group_ticket)
                   : "-");
    break;
  case kind::stop:
    break;
  }
  return output << "}";
}

gs::Generator<std::string> server_versions() {
  return gs::sampled_from<std::string>({"8.0.43", "8.0.46", "8.4.6", "8.4.8"});
}

gs::Generator<std::uint64_t> microseconds() {
  // 2000-01-01 .. 2100-01-01 in microseconds
  return gs::integers<std::uint64_t>(
      {.min_value = 946684800000000ULL, .max_value = 4102444800000000ULL});
}

gs::Generator<gtid_fields> gtid_field_values() {
  return gs::compose([](const hegel::TestCase &tc) {
    gtid_fields result;
    result.may_have_sbr = tc.draw(gs::booleans());
    result.uuid = tc.draw(model::uuids());
    result.gno = tc.draw(model::gnos());
    result.tag = tc.draw(model::tags());
    result.sequence_number = tc.draw(gs::integers<std::uint64_t>(
        {.min_value = 1U, .max_value = 1ULL << 62U}));
    result.last_committed = tc.draw(gs::integers<std::uint64_t>(
        {.min_value = 0U, .max_value = result.sequence_number - 1U}));
    result.immediate_commit_us = tc.draw(microseconds());
    if (tc.draw(gs::booleans())) {
      result.original_commit_us = tc.draw(microseconds());
    }
    result.transaction_length = tc.draw(gs::integers<std::uint64_t>(
        {.min_value = 0U, .max_value = 1ULL << 40U}));
    const auto versions{gs::sampled_from<std::string>(
        {"8.0.43", "8.0.46", "8.4.0", "8.4.8", "9.1.0"})};
    // MySQL writes the optional tail in order: immediate server version,
    // [original server version], [commit group ticket]; the later fields
    // exist only together with the immediate server version
    if (tc.draw(gs::booleans())) {
      result.immediate_server_version = tc.draw(versions);
      if (tc.draw(gs::booleans())) {
        result.original_server_version = tc.draw(versions);
      }
      if (tc.draw(gs::booleans())) {
        result.commit_group_ticket =
            tc.draw(gs::integers<std::uint64_t>({.min_value = 1U}));
      }
    }
    return result;
  });
}

// events of the given kinds (all kinds by default)
gs::Generator<event_spec>
event_specs(std::vector<kind> kinds = {kind::rotate, kind::format_description,
                                       kind::previous_gtids_log, kind::gtid_log,
                                       kind::anonymous_gtid_log,
                                       kind::gtid_tagged_log, kind::stop}) {
  return gs::compose([kinds](const hegel::TestCase &tc) {
    event_spec spec;
    spec.event_kind = tc.draw(gs::sampled_from(kinds));
    spec.server_version = tc.draw(server_versions());
    if (spec.event_kind == kind::gtid_tagged_log &&
        spec.server_version.starts_with("8.0")) {
      // tagged GTIDs exist only since MySQL 8.4
      spec.server_version = "8.4.8";
    }
    // FORMAT_DESCRIPTION always carries a footer; create_event() is asked
    // for it explicitly here (see FormatDescriptionIgnoresChecksumFlag)
    spec.include_checksum =
        spec.event_kind == kind::format_description || tc.draw(gs::booleans());
    spec.offset = tc.draw(gs::integers<std::uint32_t>(
        {.min_value = 4U, .max_value = 0xFFFF0000U}));
    spec.timestamp = tc.draw(gs::integers<std::uint32_t>());
    spec.server_id = tc.draw(gs::integers<std::uint32_t>());
    spec.flags = tc.draw(gs::sampled_from<std::uint16_t>(
        {0U, 0x001U, 0x004U, 0x008U, 0x020U, 0x040U, 0x080U, 0x100U, 0x200U,
         0x0A8U}));
    switch (spec.event_kind) {
    case kind::rotate:
      spec.binlog_base = tc.draw(gs::from_regex("[a-z][a-z0-9_-]{0,20}", true));
      spec.binlog_number = tc.draw(
          gs::integers<std::uint32_t>({.min_value = 1U, .max_value = 999999U}));
      spec.position = tc.draw(gs::integers<std::uint64_t>());
      break;
    case kind::format_description:
      spec.fde_checksum = tc.draw(gs::booleans());
      break;
    case kind::previous_gtids_log:
      spec.gtids = tc.draw(model::element_lists(6U));
      break;
    case kind::gtid_log:
    case kind::gtid_tagged_log:
      spec.gtid = tc.draw(gtid_field_values());
      break;
    case kind::anonymous_gtid_log:
      spec.gtid = tc.draw(gtid_field_values());
      spec.gtid.uuid = model::raw_uuid{};
      spec.gtid.gno = 0U;
      break;
    case kind::stop:
      break;
    }
    if (spec.event_kind != kind::gtid_tagged_log) {
      spec.gtid.tag.clear();
    } else if (!spec.gtid.immediate_server_version) {
      // the tagged GTID event always carries the immediate server version
      spec.gtid.immediate_server_version = spec.server_version;
    }
    return spec;
  });
}

// ---------------------------------------------------------------------------
// building events
// ---------------------------------------------------------------------------
util::optional_semantic_version
to_version(const std::optional<std::string> &value) {
  if (!value) {
    return std::nullopt;
  }
  return util::semantic_version{*value};
}

util::optional_high_resolution_time_point
to_time_point(const std::optional<std::uint64_t> &value) {
  if (!value) {
    return std::nullopt;
  }
  return util::microseconds_to_high_resolution_time_point(*value);
}

ev::gtid_log_flag_set gtid_flags(const gtid_fields &fields) {
  ev::gtid_log_flag_set result{};
  if (fields.may_have_sbr) {
    result.set_element(ev::gtid_log_flag_type::may_have_sbr);
  }
  return result;
}

struct built_event {
  ev::event created;
  bytes encoded;
};

built_event build(const event_spec &spec) {
  ev::event_storage buffer;
  const util::ctime_timestamp timestamp{
      static_cast<std::time_t>(spec.timestamp)};
  const ev::common_header_flag_set flags{spec.flags};
  const util::semantic_version server_version{spec.server_version};
  std::optional<ev::event> created;
  const auto &g{spec.gtid};
  switch (spec.event_kind) {
  case kind::rotate:
    created = ev::event::create_event<code_type::rotate>(
        spec.offset, timestamp, spec.server_id, flags,
        ev::generic_post_header<code_type::rotate>{spec.position},
        ev::generic_body<code_type::rotate>{
            ev::composite_binlog_name{spec.binlog_base, spec.binlog_number}},
        spec.include_checksum, buffer);
    break;
  case kind::format_description:
    created = ev::event::create_event<code_type::format_description>(
        spec.offset, timestamp, spec.server_id, flags,
        ev::generic_post_header<code_type::format_description>{
            ev::default_binlog_version, server_version, timestamp,
            ev::default_common_header_length,
            ev::reader_context::get_hardcoded_post_header_lengths(
                server_version.get_encoded())},
        ev::generic_body<code_type::format_description>{
            spec.fde_checksum ? ev::checksum_algorithm_type::crc32
                              : ev::checksum_algorithm_type::off},
        spec.include_checksum, buffer);
    break;
  case kind::previous_gtids_log: {
    binsrv::gtids::gtid_set gtids;
    for (const auto &item : spec.gtids) {
      gtids.add_interval(to_uuid(item.key.uuid),
                         binsrv::gtids::tag{item.key.tag}, item.range.lower,
                         item.range.upper);
    }
    created = ev::event::create_event<code_type::previous_gtids_log>(
        spec.offset, timestamp, spec.server_id, flags,
        ev::generic_post_header<code_type::previous_gtids_log>{},
        ev::generic_body<code_type::previous_gtids_log>{gtids},
        spec.include_checksum, buffer);
    break;
  }
  case kind::gtid_log:
  case kind::anonymous_gtid_log: {
    const ev::gtid_log_post_header post_header{
        gtid_flags(g),
        spec.event_kind == kind::gtid_log ? to_uuid(g.uuid)
                                          : binsrv::gtids::uuid{},
        g.gno,
        ev::gtid_log_post_header::known_logical_ts_code,
        g.last_committed,
        g.sequence_number};
    const ev::gtid_log_body body{
        util::microseconds_to_high_resolution_time_point(g.immediate_commit_us),
        to_time_point(g.original_commit_us),
        g.transaction_length,
        to_version(g.immediate_server_version),
        to_version(g.original_server_version),
        g.commit_group_ticket};
    if (spec.event_kind == kind::gtid_log) {
      created = ev::event::create_event<code_type::gtid_log>(
          spec.offset, timestamp, spec.server_id, flags,
          ev::generic_post_header<code_type::gtid_log>{post_header},
          ev::generic_body<code_type::gtid_log>{body}, spec.include_checksum,
          buffer);
    } else {
      created = ev::event::create_event<code_type::anonymous_gtid_log>(
          spec.offset, timestamp, spec.server_id, flags,
          ev::generic_post_header<code_type::anonymous_gtid_log>{post_header},
          ev::generic_body<code_type::anonymous_gtid_log>{body},
          spec.include_checksum, buffer);
    }
    break;
  }
  case kind::gtid_tagged_log:
    created = ev::event::create_event<code_type::gtid_tagged_log>(
        spec.offset, timestamp, spec.server_id, flags,
        ev::generic_post_header<code_type::gtid_tagged_log>{},
        ev::generic_body<code_type::gtid_tagged_log>{
            gtid_flags(g), to_uuid(g.uuid), g.gno, binsrv::gtids::tag{g.tag},
            g.last_committed, g.sequence_number,
            util::microseconds_to_high_resolution_time_point(
                g.immediate_commit_us),
            to_time_point(g.original_commit_us), g.transaction_length,
            util::semantic_version{*g.immediate_server_version},
            to_version(g.original_server_version), g.commit_group_ticket},
        spec.include_checksum, buffer);
    break;
  case kind::stop:
    created = ev::event::create_event<code_type::stop>(
        spec.offset, timestamp, spec.server_id, flags,
        ev::generic_post_header<code_type::stop>{},
        ev::generic_body<code_type::stop>{}, spec.include_checksum, buffer);
    break;
  }
  return built_event{*created, to_bytes(buffer)};
}

// A reader context for parsing the event in isolation: checksums are
// verified exactly when the event was generated with one.
ev::reader_context make_context(const event_spec &spec) {
  const util::semantic_version server_version{spec.server_version};
  return ev::reader_context{server_version.get_encoded(), spec.include_checksum,
                            binsrv::replication_mode_type::gtid, "",
                            spec.offset};
}

bool has_footer(const event_spec &spec) {
  return spec.include_checksum || spec.event_kind == kind::format_description;
}

// recomputes the CRC32 footer after a mutation
void fix_crc(bytes &data) {
  if (data.size() < 4U) {
    return;
  }
  const auto crc{util::calculate_crc32(cspan(data).first(data.size() - 4U))};
  for (std::size_t i{0U}; i < 4U; ++i) {
    data[data.size() - 4U + i] = static_cast<std::uint8_t>(crc >> (8U * i));
  }
}

bytes encode(const ev::event &event) {
  std::vector<std::byte> buffer(event.calculate_encoded_size() + 64U);
  util::byte_span destination{buffer};
  event.encode_to(destination);
  const auto written{buffer.size() - std::size(destination)};
  return to_bytes(util::const_byte_span{buffer}.first(written));
}

} // namespace

// ===========================================================================
// ACCEPT direction
// ===========================================================================

// create_event -> bytes -> event_view -> event gives back an equal event,
// and the parsed event encodes to the same bytes (and the same size as
// calculate_encoded_size()).
BOOST_AUTO_TEST_CASE(EventRoundTrip) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto spec{tc.draw("event", event_specs())};
        const auto built{build(spec)};
        const auto context{make_context(spec)};
        std::optional<ev::event> parsed;
        try {
          const ev::event_view view{context, cspan(built.encoded)};
          parsed.emplace(view);
        } catch (const std::exception &e) {
          pbt::fail(std::string{"generated event cannot be parsed: "} +
                    e.what() + " (bytes " + hex(built.encoded) + ")");
        }
        pbt::require(*parsed == built.created,
                     "parsed event differs from the created one");
        pbt::require_equal(hex(encode(*parsed)), hex(built.encoded),
                           "re-encoded event differs from the original bytes");
        pbt::require_equal(built.created.calculate_encoded_size(),
                           built.encoded.size(),
                           "calculate_encoded_size() != encoded size");
      },
      {.test_cases = 10000U});
}

// The common header describes the event: event_size is the total size,
// next_event_position is offset + size (0 for artificial ROTATE /
// FORMAT_DESCRIPTION events, as in MySQL), the server id is the requested
// one, and a footer is present exactly when requested (FORMAT_DESCRIPTION
// always has one).
BOOST_AUTO_TEST_CASE(CommonHeaderIsConsistent) {
  pbt::check([](hegel::TestCase &tc) {
    const auto spec{tc.draw("event", event_specs())};
    const auto built{build(spec)};
    const auto context{make_context(spec)};
    const ev::event_view view{context, cspan(built.encoded)};
    const auto header{view.get_common_header_view()};
    pbt::require_equal(std::size_t{header.get_event_size_raw()},
                       built.encoded.size(), "event_size field");
    const bool artificial_zero_position{
        (spec.event_kind == kind::rotate ||
         spec.event_kind == kind::format_description) &&
        (spec.flags & 0x020U) != 0U};
    const auto expected_position{
        artificial_zero_position
            ? std::uint32_t{0U}
            : static_cast<std::uint32_t>(spec.offset + built.encoded.size())};
    pbt::require_equal(header.get_next_event_position_raw(), expected_position,
                       "next_event_position (offset + size, 0 if artificial)");
    pbt::require_equal(header.get_server_id_raw(), spec.server_id,
                       "server_id field");
    pbt::require_equal(view.has_footer(), has_footer(spec), "footer presence");
    if (view.has_footer()) {
      pbt::require_equal(view.get_footer_view().get_crc_raw(),
                         view.calculate_crc(), "footer CRC32");
    }
  });
}

// ===========================================================================
// REJECT direction
// ===========================================================================

// With checksum verification on, flipping any single bit of an event that
// carries a CRC32 footer is detected: the event_view constructor throws.
// (The checksum_algorithm byte of FORMAT_DESCRIPTION is excluded: turning
// it off legitimately disables the check, as in MySQL.)
BOOST_AUTO_TEST_CASE(ChecksumDetectsSingleBitCorruption) {
  pbt::check(
      [](hegel::TestCase &tc) {
        auto spec{tc.draw("event", event_specs())};
        spec.include_checksum = true;
        // a FORMAT_DESCRIPTION event with checksum_algorithm=off is not
        // verified (by design, as in MySQL)
        spec.fde_checksum = true;
        auto data{build(spec).encoded};
        const auto position{tc.draw(
            "byte",
            gs::integers<std::size_t>({.max_value = data.size() - 1U}))};
        const auto bit{
            tc.draw("bit", gs::integers<unsigned>({.max_value = 7U}))};
        if (spec.event_kind == kind::format_description) {
          tc.assume(position != data.size() - 5U); // checksum_algorithm byte
        }
        data[position] ^= static_cast<std::uint8_t>(1U << bit);
        const auto context{make_context(spec)};
        try {
          const ev::event_view view{context, cspan(data)};
          [[maybe_unused]] const ev::event parsed{view};
        } catch (const std::exception &) {
          return;
        }
        pbt::fail("corrupted event accepted");
      },
      {.test_cases = 10000U});
}

// An event whose byte length does not match its event_size field
// (truncated, or with extra bytes) is rejected with an exception, never
// read out of bounds.
BOOST_AUTO_TEST_CASE(RejectsTruncatedOrExtendedEvents) {
  pbt::check([](hegel::TestCase &tc) {
    const auto spec{tc.draw("event", event_specs())};
    auto data{build(spec).encoded};
    const auto extend{tc.draw("extend", gs::booleans())};
    if (extend) {
      const auto extra{
          tc.draw("extra", gs::binary({.min_size = 1U, .max_size = 16U}))};
      data.insert(data.end(), extra.begin(), extra.end());
    } else {
      data.resize(tc.draw("length", gs::integers<std::size_t>(
                                        {.max_value = data.size() - 1U})));
    }
    const auto context{make_context(spec)};
    pbt::require_throws<std::exception>(
        [&] {
          const ev::event_view view{context, cspan(data)};
          [[maybe_unused]] const ev::event parsed{view};
        },
        "event with a wrong length must be rejected");
  });
}

namespace {

// Mutates the post header / body of a generated event, keeps the size and
// recomputes the CRC, so that the bytes reach the field parsers. Whatever
// the parser accepts must survive a semantic round trip: encoding the
// parsed event and parsing it again gives an equal event, and that
// encoding is a fixed point. (Byte identity is not required: like MySQL,
// PBS accepts non-minimal packed / variable-length integers and re-encodes
// them minimally.)
void check_mutation_roundtrip(hegel::TestCase &tc, const event_spec &spec) {
  auto data{build(spec).encoded};
  const std::size_t header_size{ev::default_common_header_length};
  const std::size_t footer_size{has_footer(spec) ? 4U : 0U};
  tc.assume(data.size() > header_size + footer_size);
  const auto edits{
      tc.draw("edits", gs::vectors(gs::tuples(gs::integers<std::size_t>(),
                                              gs::integers<std::uint8_t>()),
                                   {.min_size = 1U, .max_size = 4U}))};
  const auto payload_size{data.size() - header_size - footer_size};
  for (const auto &[position, value] : edits) {
    data[header_size + position % payload_size] = value;
  }
  if (footer_size != 0U) {
    fix_crc(data);
  }
  tc.note("mutated bytes = " + hex(data));
  const auto context{make_context(spec)};
  std::optional<ev::event> parsed;
  try {
    const ev::event_view view{context, cspan(data)};
    parsed.emplace(view);
  } catch (const std::exception &) {
    return; // rejected: fine
  }
  bytes reencoded;
  try {
    reencoded = encode(*parsed);
  } catch (const std::exception &e) {
    pbt::fail(std::string{"accepted event cannot be re-encoded: "} + e.what());
  }
  tc.note("re-encoded bytes = " + hex(reencoded));
  // A non-minimal integer encoding re-encodes shorter / longer; encode_to()
  // keeps the parsed common header (event_size), so such an event is not
  // re-parseable by construction. PBS only re-encodes upstream events in
  // the rewriter, which recomputes the sizes.
  if (reencoded.size() != data.size()) {
    return;
  }
  std::optional<ev::event> reparsed;
  try {
    const ev::event_view view{context, cspan(reencoded)};
    reparsed.emplace(view);
  } catch (const std::exception &e) {
    pbt::fail(std::string{"re-encoded event cannot be parsed: "} + e.what());
  }
  pbt::require(*reparsed == *parsed,
               "parse(encode(parse(bytes))) != parse(bytes)");
  pbt::require_equal(hex(encode(*reparsed)), hex(reencoded),
                     "re-encoding is not a fixed point");
}

} // namespace

// One property per event type, so that each defect has its own test id.
BOOST_AUTO_TEST_CASE(RotateMutationsRoundTrip) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto spec{tc.draw("event", event_specs({kind::rotate}))};
        check_mutation_roundtrip(tc, spec);
      },
      {.test_cases = 5000U});
}
BOOST_AUTO_TEST_CASE(FormatDescriptionMutationsRoundTrip) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto spec{
            tc.draw("event", event_specs({kind::format_description}))};
        check_mutation_roundtrip(tc, spec);
      },
      {.test_cases = 5000U});
}
BOOST_AUTO_TEST_CASE(PreviousGtidsMutationsRoundTrip) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto spec{
            tc.draw("event", event_specs({kind::previous_gtids_log}))};
        check_mutation_roundtrip(tc, spec);
      },
      {.test_cases = 5000U});
}
BOOST_AUTO_TEST_CASE(GtidLogMutationsRoundTrip) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto spec{tc.draw(
            "event", event_specs({kind::gtid_log, kind::anonymous_gtid_log}))};
        check_mutation_roundtrip(tc, spec);
      },
      {.test_cases = 5000U});
}
BOOST_AUTO_TEST_CASE(GtidTaggedLogMutationsRoundTrip) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto spec{tc.draw("event", event_specs({kind::gtid_tagged_log}))};
        check_mutation_roundtrip(tc, spec);
      },
      {.test_cases = 5000U});
}

// create_event<FORMAT_DESCRIPTION>() documents that FORMAT_DESCRIPTION
// events always get a footer (event.hpp: "if (code ==
// code_type::format_description) include_checksum = true"), so asking for
// one without a checksum must give the same event as asking with one.
BOOST_AUTO_TEST_CASE(FormatDescriptionIgnoresChecksumFlag) {
  pbt::check([](hegel::TestCase &tc) {
    auto spec{tc.draw("event", event_specs({kind::format_description}))};
    spec.include_checksum = true;
    const auto with_flag{build(spec)};
    spec.include_checksum = false;
    std::optional<built_event> without_flag;
    try {
      without_flag.emplace(build(spec));
    } catch (const std::exception &e) {
      pbt::fail(std::string{"create_event<format_description>(include_checksum"
                            "=false) failed: "} +
                e.what());
    }
    pbt::require_equal(hex(without_flag->encoded), hex(with_flag.encoded),
                       "FORMAT_DESCRIPTION without the checksum flag differs");
  });
}

// The server_version field of a FORMAT_DESCRIPTION event comes from the
// upstream server (or from a stored binlog). A value that is not a valid
// "major.minor.patch" version must make parsing fail with an exception;
// it must never terminate the process (std::terminate from a noexcept
// function).
BOOST_AUTO_TEST_CASE(FormatDescriptionWithOddServerVersionNeverAborts) {
  pbt::check([](hegel::TestCase &tc) {
    auto spec{tc.draw("event", event_specs({kind::format_description}))};
    auto data{build(spec).encoded};
    const auto version{tc.draw(
        "server_version",
        gs::one_of<std::string>(
            {gs::sampled_from<std::string>(
                 {"8.0.300", "8.0", "8", "x.y.z", "8.0.43a", "10.11.6-MariaDB",
                  "5.5.5-10.6.12-MariaDB", "", "8..0", "-8.0.1", "256.0.0"}),
             gs::from_regex("[0-9a-z.-]{0,12}", true)}))};
    // server_version: 50 bytes after the 2-byte binlog_version
    constexpr std::size_t version_offset{ev::default_common_header_length + 2U};
    constexpr std::size_t version_size{50U};
    for (std::size_t i{0U}; i < version_size; ++i) {
      data[version_offset + i] = i < version.size()
                                     ? static_cast<std::uint8_t>(version[i])
                                     : std::uint8_t{0U};
    }
    fix_crc(data);
    tc.note("server_version = '" + version + "'");
    const auto context{make_context(spec)};
    try {
      const ev::event_view view{context, cspan(data)};
      [[maybe_unused]] const ev::event parsed{view};
    } catch (const std::exception &) {
      // rejected with an exception: fine
    }
  });
}

namespace {

std::size_t varlen_size_of(std::uint64_t value) {
  const auto bits{static_cast<std::size_t>(std::bit_width(value))};
  const std::size_t needed{(bits + 6U) / 7U};
  return needed <= 1U ? 1U : (needed <= 8U ? needed : 9U);
}

bytes varlen_bytes(std::uint64_t value) {
  const auto size{varlen_size_of(value)};
  bytes result;
  if (size == 9U) {
    result.push_back(0xFFU);
    for (std::size_t i{0U}; i < 8U; ++i) {
      result.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
    }
    return result;
  }
  const std::uint64_t encoded{(value << size) | ((1ULL << (size - 1U)) - 1ULL)};
  for (std::size_t i{0U}; i < size; ++i) {
    result.push_back(static_cast<std::uint8_t>(encoded >> (8U * i)));
  }
  return result;
}

void put_u32(bytes &data, std::size_t offset, std::uint32_t value) {
  for (std::size_t i{0U}; i < 4U; ++i) {
    data[offset + i] = static_cast<std::uint8_t>(value >> (8U * i));
  }
}

std::uint32_t get_u32(const bytes &data, std::size_t offset) {
  std::uint32_t result{0U};
  for (std::size_t i{0U}; i < 4U; ++i) {
    result |= std::uint32_t{data[offset + i]} << (8U * i);
  }
  return result;
}

} // namespace

// GTID_TAGGED_LOG bodies use MySQL's serialization framework
// (libs/mysql/serialization/readme.md): "serializable_field_size ... is
// used ... to skip unknown fields in the packet in case it is encoded by
// encoder of version newer than the version of decoder", and only field
// ids up to last_non_ignorable_field_id (0 for this event) must be known.
// A body from a newer server with extra trailing fields must therefore be
// accepted, with the known fields unchanged.
BOOST_AUTO_TEST_CASE(GtidTaggedLogSkipsUnknownTrailingFields) {
  pbt::check([](hegel::TestCase &tc) {
    const auto spec{tc.draw("event", event_specs({kind::gtid_tagged_log}))};
    const auto built{build(spec)};
    const auto unknown_id{tc.draw(
        "unknown_field_id",
        gs::integers<std::uint8_t>({.min_value = 12U, .max_value = 120U}))};
    const auto payload{tc.draw("unknown_field_data",
                               gs::binary({.min_size = 1U, .max_size = 12U}))};

    const std::size_t header_size{ev::default_common_header_length};
    const std::size_t footer_size{spec.include_checksum ? 4U : 0U};
    const auto &original{built.encoded};
    const bytes body(
        std::next(original.begin(), static_cast<long>(header_size)),
        std::prev(original.end(), static_cast<long>(footer_size)));
    // body: <version:1> <serializable_field_size:varlen> <rest...>
    const auto size_field_length{
        static_cast<std::size_t>(std::countr_one(body.at(1U))) + 1U};
    bytes rest(
        std::next(body.begin(), static_cast<long>(1U + size_field_length)),
        body.end());
    const auto extra{varlen_bytes(unknown_id)};
    rest.insert(rest.end(), extra.begin(), extra.end());
    rest.insert(rest.end(), payload.begin(), payload.end());
    // serializable_field_size covers the whole body (as PBS encodes it)
    std::size_t new_size{1U + rest.size() + 1U};
    while (1U + varlen_size_of(new_size) + rest.size() != new_size) {
      new_size = 1U + varlen_size_of(new_size) + rest.size();
    }
    bytes new_body{body.front()};
    const auto size_bytes{varlen_bytes(new_size)};
    new_body.insert(new_body.end(), size_bytes.begin(), size_bytes.end());
    new_body.insert(new_body.end(), rest.begin(), rest.end());

    bytes data(original.begin(),
               std::next(original.begin(), static_cast<long>(header_size)));
    data.insert(data.end(), new_body.begin(), new_body.end());
    data.resize(data.size() + footer_size);
    const auto delta{static_cast<std::uint32_t>(data.size() - original.size())};
    put_u32(data, 9U, get_u32(data, 9U) + delta);   // event_size
    put_u32(data, 13U, get_u32(data, 13U) + delta); // next_event_position
    if (footer_size != 0U) {
      fix_crc(data);
    }
    tc.note("event with unknown field = " + hex(data));

    const auto context{make_context(spec)};
    try {
      const ev::event_view view{context, cspan(data)};
      const ev::event parsed{view};
      pbt::require(parsed.get_body<code_type::gtid_tagged_log>() ==
                       built.created.get_body<code_type::gtid_tagged_log>(),
                   "known fields changed by an unknown trailing field");
    } catch (const pbt::property_violation &) {
      throw;
    } catch (const std::exception &e) {
      pbt::fail(
          std::string{"event with an unknown ignorable field rejected: "} +
          e.what());
    }
  });
}
