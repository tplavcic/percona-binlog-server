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

// Property-based tests for the binary integer codecs in
// src/util/byte_span_inserters.hpp and src/util/byte_span_extractors.hpp:
//  - fixed-length little-endian integers (full and partial widths),
//  - MySQL "packed" integers (mysys/pack.cc, net_store_length /
//    net_field_length_ll),
//  - MySQL variable-length integers
//    (libs/mysql/serialization/variable_length_integers.h).
// These codecs decode every binlog event and GTID set, so they are run
// against independent reference implementations written from the MySQL
// format descriptions below, for every integer type they are instantiated
// with.
//
// Reference formats:
//  - fixed:  value as N bytes, least significant byte first.
//  - packed: v < 251 -> [v]; v < 2^16 -> [252, 2 bytes]; v < 2^24 -> [253,
//            3 bytes]; otherwise [254, 8 bytes]. On decoding 251 means NULL
//            (decoded as UINT64_MAX); 255 is rejected by PBS
//            (packed_int_forbidden_marker).
//  - varlen: the number of bytes n is 1 + the number of trailing one bits
//            of the first byte. For n <= 8 the n bytes, read as a
//            little-endian number x, hold the value as x >> n (n bytes hold
//            7*n value bits); for n == 9 the first byte is 0xFF and the
//            value is the next 8 bytes. The encoder uses the smallest n.
//            Signed values are zigzag-encoded: v >= 0 -> 2v, v < 0 -> -2v-1.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#define BOOST_TEST_MODULE ByteSpanCodecPropertyTests
// this include is needed as it provides the 'main()' function
// NOLINTNEXTLINE(misc-include-cleaner)
#include <boost/test/unit_test.hpp>

#include <boost/core/demangle.hpp>

#include "util/byte_span_extractors.hpp"
#include "util/byte_span_fwd.hpp"
#include "util/byte_span_inserters.hpp"

#include "common/pbt_helpers.hpp"

namespace {

namespace gs = hegel::generators;

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

util::byte_span as_span(std::vector<std::byte> &buffer) { return buffer; }

util::const_byte_span as_const_span(const bytes &data) {
  return {// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
          reinterpret_cast<const std::byte *>(std::data(data)),
          std::size(data)};
}

bytes to_bytes(std::span<const std::byte> data) {
  bytes result(std::size(data));
  for (std::size_t i{0U}; i < std::size(data); ++i) {
    result[i] = std::to_integer<std::uint8_t>(data[i]);
  }
  return result;
}

template <typename T> std::string type_name() {
  return boost::core::demangle(typeid(T).name());
}

// generator for every value of T, biased towards the boundaries the codecs
// care about (powers of two and the packed-int markers)
template <typename T> gs::Generator<T> values() {
  using limits = std::numeric_limits<T>;
  std::vector<T> interesting{0, 1, limits::max(), limits::min()};
  for (unsigned bit{1U}; bit < static_cast<unsigned>(limits::digits); ++bit) {
    const std::uint64_t power{1ULL << bit};
    interesting.push_back(static_cast<T>(power));
    interesting.push_back(static_cast<T>(power - 1U));
  }
  if constexpr (sizeof(T) >= 2U) {
    for (const std::uint64_t marker :
         {250ULL, 251ULL, 252ULL, 253ULL, 254ULL, 255ULL, 256ULL}) {
      interesting.push_back(static_cast<T>(marker));
    }
  }
  if constexpr (std::is_signed_v<T>) {
    interesting.push_back(static_cast<T>(-1));
    interesting.push_back(static_cast<T>(limits::min() + 1));
  }
  return gs::one_of<T>({gs::sampled_from(interesting), gs::integers<T>()});
}

// --------------------------------------------------------------------------
// reference implementations
// --------------------------------------------------------------------------
bytes ref_fixed(std::uint64_t value, std::size_t width) {
  bytes result;
  for (std::size_t i{0U}; i < width; ++i) {
    result.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
  }
  return result;
}

std::uint64_t ref_read_le(const bytes &data, std::size_t offset,
                          std::size_t width) {
  std::uint64_t result{0ULL};
  for (std::size_t i{0U}; i < width; ++i) {
    result |= std::uint64_t{data[offset + i]} << (8U * i);
  }
  return result;
}

bytes ref_packed(std::uint64_t value) {
  if (value < 251ULL) {
    return {static_cast<std::uint8_t>(value)};
  }
  bytes result;
  std::size_t width{8U};
  if (value < (1ULL << 16U)) {
    result.push_back(252U);
    width = 2U;
  } else if (value < (1ULL << 24U)) {
    result.push_back(253U);
    width = 3U;
  } else {
    result.push_back(254U);
  }
  const auto tail{ref_fixed(value, width)};
  result.insert(std::end(result), std::begin(tail), std::end(tail));
  return result;
}

struct decoded {
  std::uint64_t value{};
  std::size_t consumed{};
};

std::optional<decoded> ref_unpack(const bytes &data) {
  if (data.empty()) {
    return std::nullopt;
  }
  const auto first{data.front()};
  std::size_t width{0U};
  switch (first) {
  case 251U:
    return decoded{std::numeric_limits<std::uint64_t>::max(), 1U};
  case 252U:
    width = 2U;
    break;
  case 253U:
    width = 3U;
    break;
  case 254U:
    width = 8U;
    break;
  case 255U:
    return std::nullopt;
  default:
    return decoded{first, 1U};
  }
  if (data.size() < 1U + width) {
    return std::nullopt;
  }
  return decoded{ref_read_le(data, 1U, width), 1U + width};
}

std::size_t ref_varlen_size(std::uint64_t value) {
  const auto bits{static_cast<std::size_t>(std::bit_width(value))};
  const std::size_t needed{(bits + 6U) / 7U}; // n bytes hold 7*n bits
  if (needed <= 1U) {
    return 1U;
  }
  return needed <= 8U ? needed : 9U;
}

bytes ref_varlen(std::uint64_t value) {
  const auto size{ref_varlen_size(value)};
  if (size == 9U) {
    bytes result{0xFFU};
    const auto tail{ref_fixed(value, 8U)};
    result.insert(std::end(result), std::begin(tail), std::end(tail));
    return result;
  }
  // value < 2^(7*size), so value << size fits into 8*size <= 64 bits
  const std::uint64_t marker{(1ULL << (size - 1U)) - 1ULL};
  return ref_fixed((value << size) | marker, size);
}

std::optional<decoded> ref_unvarlen(const bytes &data) {
  if (data.empty()) {
    return std::nullopt;
  }
  const auto size{static_cast<std::size_t>(std::countr_one(data.front())) + 1U};
  if (data.size() < size) {
    return std::nullopt;
  }
  if (size == 9U) {
    return decoded{ref_read_le(data, 1U, 8U), 9U};
  }
  return decoded{ref_read_le(data, 0U, size) >> size, size};
}

template <typename T> std::uint64_t zigzag(T value) {
  using unsigned_t = std::make_unsigned_t<T>;
  if (value >= 0) {
    return std::uint64_t{static_cast<unsigned_t>(value)} << 1U;
  }
  // -(value + 1) is representable for every negative value
  const auto magnitude{static_cast<unsigned_t>(-(value + 1))};
  return (std::uint64_t{magnitude} << 1U) | 1ULL;
}

template <typename T> std::optional<T> unzigzag(std::uint64_t value) {
  using unsigned_t = std::make_unsigned_t<T>;
  if (value > std::numeric_limits<unsigned_t>::max()) {
    return std::nullopt;
  }
  const auto half{static_cast<unsigned_t>(value >> 1U)};
  if ((value & 1ULL) == 0ULL) {
    return static_cast<T>(half);
  }
  return static_cast<T>(-static_cast<T>(half) - 1);
}

// --------------------------------------------------------------------------
// fixed-length integers
// --------------------------------------------------------------------------

// insert_fixed_int_to_byte_span(value, n) writes exactly the n least
// significant bytes of value, least significant first, and advances the
// span by n; extract_fixed_int_from_byte_span(n) reads them back as
// value mod 2^(8n).
template <typename T> void check_fixed_unsigned_roundtrip() {
  pbt::check([](hegel::TestCase &tc) {
    const auto value{tc.draw("value", values<T>())};
    const auto width{tc.draw(
        "width",
        gs::integers<std::size_t>({.min_value = 1U, .max_value = sizeof(T)}))};
    const auto slack{tc.draw("slack", gs::integers<std::size_t>(
                                          {.min_value = 0U, .max_value = 3U}))};
    std::vector<std::byte> buffer(width + slack, std::byte{0xAA});
    auto out{as_span(buffer)};
    pbt::require(util::insert_fixed_int_to_byte_span_checked(out, value, width),
                 "checked insert into a large enough buffer failed");
    pbt::require_equal(std::size(out), slack, "remainder after insert");
    const auto written{to_bytes(std::span{buffer}.first(width))};
    pbt::require_equal(hex(written), hex(ref_fixed(value, width)),
                       "fixed int bytes (little endian)");
    pbt::require_equal(hex(to_bytes(std::span{buffer}.subspan(width))),
                       hex(bytes(slack, 0xAAU)),
                       "bytes after the inserted value were modified");

    util::const_byte_span in{buffer};
    T extracted{};
    pbt::require(
        util::extract_fixed_int_from_byte_span_checked(in, extracted, width),
        "checked extract from a large enough buffer failed");
    pbt::require_equal(std::size(in), slack, "remainder after extract");
    const auto mask{width == sizeof(std::uint64_t)
                        ? std::numeric_limits<std::uint64_t>::max()
                        : (1ULL << (8U * width)) - 1ULL};
    pbt::require_equal(std::uint64_t{extracted}, std::uint64_t{value} & mask,
                       "extracted value");
  });
}

BOOST_AUTO_TEST_CASE(FixedIntRoundTripU8) {
  check_fixed_unsigned_roundtrip<std::uint8_t>();
}
BOOST_AUTO_TEST_CASE(FixedIntRoundTripU16) {
  check_fixed_unsigned_roundtrip<std::uint16_t>();
}
BOOST_AUTO_TEST_CASE(FixedIntRoundTripU32) {
  check_fixed_unsigned_roundtrip<std::uint32_t>();
}
BOOST_AUTO_TEST_CASE(FixedIntRoundTripU64) {
  check_fixed_unsigned_roundtrip<std::uint64_t>();
}

// Signed fixed ints use two's complement over the full width; partial
// widths are rejected by the checked functions (documented limitation).
template <typename T> void check_fixed_signed_roundtrip() {
  pbt::check([](hegel::TestCase &tc) {
    using unsigned_t = std::make_unsigned_t<T>;
    const auto value{tc.draw("value", values<T>())};
    std::vector<std::byte> buffer(sizeof(T));
    auto out{as_span(buffer)};
    pbt::require(util::insert_fixed_int_to_byte_span_checked(out, value),
                 "checked signed insert failed");
    pbt::require_equal(
        hex(to_bytes(buffer)),
        hex(ref_fixed(static_cast<unsigned_t>(value), sizeof(T))),
        "signed fixed int bytes (two's complement, little endian)");
    util::const_byte_span in{buffer};
    T extracted{};
    pbt::require(util::extract_fixed_int_from_byte_span_checked(in, extracted),
                 "checked signed extract failed");
    pbt::require_equal(std::int64_t{extracted}, std::int64_t{value},
                       "signed roundtrip");

    if constexpr (sizeof(T) > 1U) {
      const auto partial{
          tc.draw("partial_width",
                  gs::integers<std::size_t>(
                      {.min_value = 1U, .max_value = sizeof(T) - 1U}))};
      auto out2{as_span(buffer)};
      pbt::require(
          !util::insert_fixed_int_to_byte_span_checked(out2, value, partial),
          "signed insert with a partial width must be rejected");
      pbt::require_equal(std::size(out2), sizeof(T),
                         "rejected insert must not advance the span");
      util::const_byte_span in2{buffer};
      pbt::require(!util::extract_fixed_int_from_byte_span_checked(
                       in2, extracted, partial),
                   "signed extract with a partial width must be rejected");
    }
  });
}

BOOST_AUTO_TEST_CASE(FixedIntRoundTripI8) {
  check_fixed_signed_roundtrip<std::int8_t>();
}
BOOST_AUTO_TEST_CASE(FixedIntRoundTripI16) {
  check_fixed_signed_roundtrip<std::int16_t>();
}
BOOST_AUTO_TEST_CASE(FixedIntRoundTripI32) {
  check_fixed_signed_roundtrip<std::int32_t>();
}
BOOST_AUTO_TEST_CASE(FixedIntRoundTripI64) {
  check_fixed_signed_roundtrip<std::int64_t>();
}

// Checked insert / extract on a buffer shorter than the requested width
// return false and leave both the span and the output value untouched.
BOOST_AUTO_TEST_CASE(FixedIntCheckedRejectsShortBuffer) {
  pbt::check([](hegel::TestCase &tc) {
    const auto value{tc.draw("value", values<std::uint64_t>())};
    const auto width{tc.draw("width", gs::integers<std::size_t>(
                                          {.min_value = 1U, .max_value = 8U}))};
    const auto size{tc.draw(
        "buffer_size",
        gs::integers<std::size_t>({.min_value = 0U, .max_value = width - 1U}))};
    std::vector<std::byte> buffer(size, std::byte{0x5A});
    auto out{as_span(buffer)};
    pbt::require(
        !util::insert_fixed_int_to_byte_span_checked(out, value, width),
        "insert into a too short buffer must fail");
    pbt::require_equal(std::size(out), size, "span advanced on failure");
    pbt::require_equal(hex(to_bytes(buffer)), hex(bytes(size, 0x5AU)),
                       "buffer modified on failure");
    util::const_byte_span in{buffer};
    std::uint64_t extracted{0x1234ULL};
    pbt::require(
        !util::extract_fixed_int_from_byte_span_checked(in, extracted, width),
        "extract from a too short buffer must fail");
    pbt::require_equal(std::size(in), size, "span advanced on failure");
    pbt::require_equal(extracted, std::uint64_t{0x1234ULL},
                       "output value modified on failure");
  });
}

// The width of a fixed int must be 1..sizeof(T) (asserted by the unchecked
// functions). The checked functions are the API for untrusted widths, so
// they must return false for an invalid width instead of reading / writing
// past the integer or advancing the span by more than they copy.
template <typename T>
void check_fixed_checked_rejects_width(hegel::TestCase &tc, std::size_t width) {
  const auto value{tc.draw("value", values<T>())};
  // the buffer is large enough, so only the width can make it fail
  std::vector<std::byte> buffer(32U, std::byte{0x5A});
  auto out{as_span(buffer)};
  pbt::require(!util::insert_fixed_int_to_byte_span_checked(out, value, width),
               "checked insert with an invalid width must return false");
  pbt::require_equal(std::size(out), buffer.size(), "span advanced on failure");
  util::const_byte_span in{buffer};
  T extracted{};
  pbt::require(
      !util::extract_fixed_int_from_byte_span_checked(in, extracted, width),
      "checked extract with an invalid width must return false");
  pbt::require_equal(std::size(in), buffer.size(), "span advanced on failure");
}

void check_fixed_checked_rejects_width_any_type(hegel::TestCase &tc,
                                                bool oversized) {
  const auto type_size{
      tc.draw("type_size", gs::sampled_from<std::size_t>({1U, 2U, 4U, 8U}))};
  const auto width{
      oversized ? tc.draw("width",
                          gs::integers<std::size_t>(
                              {.min_value = type_size + 1U, .max_value = 16U}))
                : std::size_t{0U}};
  switch (type_size) {
  case 1U:
    check_fixed_checked_rejects_width<std::uint8_t>(tc, width);
    break;
  case 2U:
    check_fixed_checked_rejects_width<std::uint16_t>(tc, width);
    break;
  case 4U:
    check_fixed_checked_rejects_width<std::uint32_t>(tc, width);
    break;
  default:
    check_fixed_checked_rejects_width<std::uint64_t>(tc, width);
    break;
  }
}

BOOST_AUTO_TEST_CASE(FixedIntCheckedRejectsZeroWidth) {
  pbt::check([](hegel::TestCase &tc) {
    check_fixed_checked_rejects_width_any_type(tc, false);
  });
}

BOOST_AUTO_TEST_CASE(FixedIntCheckedRejectsOversizedWidth) {
  pbt::check([](hegel::TestCase &tc) {
    check_fixed_checked_rejects_width_any_type(tc, true);
  });
}

// --------------------------------------------------------------------------
// packed integers
// --------------------------------------------------------------------------

// Encoding is byte-for-byte MySQL's net_store_length(), its size is
// calculate_packed_int_size(), and decoding gives the value back.
BOOST_AUTO_TEST_CASE(PackedIntRoundTripMatchesMySQL) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto value{tc.draw("value", values<std::uint64_t>())};
        std::vector<std::byte> buffer(16U);
        auto out{as_span(buffer)};
        pbt::require(util::insert_packed_int_to_byte_span_checked(out, value),
                     "packed insert failed");
        const auto written_size{std::size(buffer) - std::size(out)};
        const auto written{to_bytes(std::span{buffer}.first(written_size))};
        pbt::require_equal(hex(written), hex(ref_packed(value)),
                           "packed encoding differs from mysys/pack.cc");
        pbt::require_equal(util::calculate_packed_int_size(value), written_size,
                           "calculate_packed_int_size() != bytes written");

        util::const_byte_span in{as_const_span(written)};
        std::uint64_t extracted{};
        pbt::require(
            util::extract_packed_int_from_byte_span_checked(in, extracted),
            "packed extract failed");
        pbt::require(in.empty(), "packed extract did not consume everything");
        pbt::require_equal(extracted, value, "packed roundtrip");
      },
      {.test_cases = 10000U});
}

// The encoder never emits the NULL marker 251 or the forbidden marker 255
// as the first byte.
BOOST_AUTO_TEST_CASE(PackedIntEncoderNeverEmitsReservedMarkers) {
  pbt::check([](hegel::TestCase &tc) {
    const auto value{tc.draw("value", values<std::uint64_t>())};
    std::vector<std::byte> buffer(16U);
    auto out{as_span(buffer)};
    pbt::require(util::insert_packed_int_to_byte_span_checked(out, value),
                 "packed insert failed");
    const auto first{std::to_integer<std::uint8_t>(buffer.front())};
    pbt::require(first != 251U && first != 255U,
                 "packed encoding starts with a reserved marker");
  });
}

// Decoding arbitrary bytes agrees with the reference decoder (value,
// consumed bytes, failure), never reads past the input, and leaves the span
// and value untouched on failure.
BOOST_AUTO_TEST_CASE(PackedIntDecodeAgreesWithReference) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto data{tc.draw(
            "data", gs::one_of<bytes>(
                        {gs::binary({.max_size = 10U}),
                         gs::tuples(gs::sampled_from<std::uint8_t>(
                                        {250U, 251U, 252U, 253U, 254U, 255U}),
                                    gs::binary({.max_size = 9U}))
                             .map([](auto pair) {
                               bytes result{std::get<0>(pair)};
                               const auto &tail{std::get<1>(pair)};
                               result.insert(std::end(result), std::begin(tail),
                                             std::end(tail));
                               return result;
                             })}))};
        util::const_byte_span in{as_const_span(data)};
        std::uint64_t value{0xDEADBEEFULL};
        const bool ok{
            util::extract_packed_int_from_byte_span_checked(in, value)};
        const auto expected{ref_unpack(data)};
        pbt::require_equal(ok, expected.has_value(),
                           "packed decode success differs from reference");
        if (ok) {
          pbt::require_equal(value, expected->value, "packed decoded value");
          pbt::require_equal(data.size() - std::size(in), expected->consumed,
                             "packed decode consumed bytes");
        } else {
          pbt::require_equal(std::size(in), data.size(),
                             "failed packed decode advanced the span");
          pbt::require_equal(value, std::uint64_t{0xDEADBEEFULL},
                             "failed packed decode modified the value");
        }
      },
      {.test_cases = 10000U});
}

// Packed insert into a buffer shorter than the encoding fails without
// writing.
BOOST_AUTO_TEST_CASE(PackedIntInsertRejectsShortBuffer) {
  pbt::check([](hegel::TestCase &tc) {
    const auto value{tc.draw("value", values<std::uint64_t>())};
    const auto needed{ref_packed(value).size()};
    const auto size{tc.draw("buffer_size",
                            gs::integers<std::size_t>(
                                {.min_value = 0U, .max_value = needed - 1U}))};
    std::vector<std::byte> buffer(size, std::byte{0x5A});
    auto out{as_span(buffer)};
    pbt::require(!util::insert_packed_int_to_byte_span_checked(out, value),
                 "packed insert into a too short buffer must fail");
    pbt::require_equal(std::size(out), size, "span advanced on failure");
    pbt::require_equal(hex(to_bytes(buffer)), hex(bytes(size, 0x5AU)),
                       "buffer modified on failure");
  });
}

// --------------------------------------------------------------------------
// variable-length integers
// --------------------------------------------------------------------------

// For every unsigned type: the encoding is byte-for-byte the MySQL one,
// calculate_varlen_int_size() is its length, and decoding into the same
// type gives the value back.
template <typename T> void check_varlen_unsigned_roundtrip() {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto value{tc.draw("value", values<T>())};
        std::vector<std::byte> buffer(16U);
        auto out{as_span(buffer)};
        pbt::require(util::insert_varlen_int_to_byte_span_checked(out, value),
                     "varlen insert failed");
        const auto written_size{std::size(buffer) - std::size(out)};
        const auto written{to_bytes(std::span{buffer}.first(written_size))};
        pbt::require_equal(hex(written), hex(ref_varlen(value)),
                           "varlen encoding differs from MySQL "
                           "variable_length_integers.h");
        pbt::require_equal(util::calculate_varlen_int_size(value), written_size,
                           "calculate_varlen_int_size() != bytes written");
        util::const_byte_span in{as_const_span(written)};
        T extracted{};
        pbt::require(
            util::extract_varlen_int_from_byte_span_checked(in, extracted),
            "varlen extract failed");
        pbt::require(in.empty(), "varlen extract did not consume everything");
        pbt::require_equal(std::uint64_t{extracted}, std::uint64_t{value},
                           "varlen roundtrip");
      },
      {.test_cases = 10000U});
}

BOOST_AUTO_TEST_CASE(VarlenUnsignedRoundTripU8) {
  check_varlen_unsigned_roundtrip<std::uint8_t>();
}
BOOST_AUTO_TEST_CASE(VarlenUnsignedRoundTripU16) {
  check_varlen_unsigned_roundtrip<std::uint16_t>();
}
BOOST_AUTO_TEST_CASE(VarlenUnsignedRoundTripU32) {
  check_varlen_unsigned_roundtrip<std::uint32_t>();
}
BOOST_AUTO_TEST_CASE(VarlenUnsignedRoundTripU64) {
  check_varlen_unsigned_roundtrip<std::uint64_t>();
}

// Signed values use the zigzag mapping and the same byte layout.
template <typename T> void check_varlen_signed_roundtrip() {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto value{tc.draw("value", values<T>())};
        std::vector<std::byte> buffer(16U);
        auto out{as_span(buffer)};
        pbt::require(util::insert_varlen_int_to_byte_span_checked(out, value),
                     "signed varlen insert failed");
        const auto written_size{std::size(buffer) - std::size(out)};
        const auto written{to_bytes(std::span{buffer}.first(written_size))};
        pbt::require_equal(hex(written), hex(ref_varlen(zigzag(value))),
                           "signed varlen encoding differs from MySQL "
                           "(zigzag + varlen)");
        pbt::require_equal(util::calculate_varlen_int_size(value), written_size,
                           "calculate_varlen_int_size() != bytes written");
        util::const_byte_span in{as_const_span(written)};
        T extracted{};
        pbt::require(
            util::extract_varlen_int_from_byte_span_checked(in, extracted),
            "signed varlen extract failed");
        pbt::require(in.empty(), "varlen extract did not consume everything");
        pbt::require_equal(std::int64_t{extracted}, std::int64_t{value},
                           "signed varlen roundtrip");
      },
      {.test_cases = 10000U});
}

BOOST_AUTO_TEST_CASE(VarlenSignedRoundTripI8) {
  check_varlen_signed_roundtrip<std::int8_t>();
}
BOOST_AUTO_TEST_CASE(VarlenSignedRoundTripI16) {
  check_varlen_signed_roundtrip<std::int16_t>();
}
BOOST_AUTO_TEST_CASE(VarlenSignedRoundTripI32) {
  check_varlen_signed_roundtrip<std::int32_t>();
}
BOOST_AUTO_TEST_CASE(VarlenSignedRoundTripI64) {
  check_varlen_signed_roundtrip<std::int64_t>();
}

// Decoding arbitrary bytes into T agrees with the reference decoder: same
// success, value and consumed length; a value that does not fit into T is
// rejected (MySQL read_varlen_bytes returns 0); failures leave span and
// value untouched. Non-minimal encodings are accepted like in MySQL.
template <typename T> void check_varlen_decode_agrees_with_reference() {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto data{tc.draw(
            "data", gs::one_of<bytes>(
                        {gs::binary({.max_size = 10U}),
                         // encodings of 64-bit values, decoded into narrower T
                         values<std::uint64_t>().map(
                             [](std::uint64_t v) { return ref_varlen(v); }),
                         // non-minimal encodings: value padded to more bytes
                         gs::tuples(gs::integers<std::uint64_t>(
                                        {.max_value = (1ULL << 49U) - 1U}),
                                    gs::integers<std::size_t>(
                                        {.min_value = 1U, .max_value = 8U}))
                             .map([](auto pair) {
                               const auto [v, extra]{pair};
                               const auto minimal{ref_varlen_size(v)};
                               const auto size{
                                   std::min<std::size_t>(minimal + extra, 8U)};
                               return ref_fixed(
                                   (v << size) | ((1ULL << (size - 1U)) - 1ULL),
                                   size);
                             })}))};
        util::const_byte_span in{as_const_span(data)};
        T value{static_cast<T>(42)};
        const bool ok{
            util::extract_varlen_int_from_byte_span_checked(in, value)};

        std::optional<T> expected_value;
        std::size_t expected_consumed{0U};
        if (const auto raw{ref_unvarlen(data)}; raw) {
          expected_consumed = raw->consumed;
          if constexpr (std::is_signed_v<T>) {
            expected_value = unzigzag<T>(raw->value);
          } else if (raw->value <= std::numeric_limits<T>::max()) {
            expected_value = static_cast<T>(raw->value);
          }
        }
        tc.note("data = " + hex(data) + ", T = " + type_name<T>());
        pbt::require_equal(ok, expected_value.has_value(),
                           "varlen decode success differs from reference");
        if (ok) {
          if (value != *expected_value) {
            pbt::fail("varlen decoded value differs from reference: actual " +
                      std::to_string(value) + ", expected " +
                      std::to_string(*expected_value));
          }
          pbt::require_equal(data.size() - std::size(in), expected_consumed,
                             "varlen decode consumed bytes");
        } else {
          pbt::require_equal(std::size(in), data.size(),
                             "failed varlen decode advanced the span");
          pbt::require(value == static_cast<T>(42),
                       "failed varlen decode modified the value");
        }
      },
      {.test_cases = 10000U});
}

BOOST_AUTO_TEST_CASE(VarlenDecodeAgreesWithReferenceU8) {
  check_varlen_decode_agrees_with_reference<std::uint8_t>();
}
BOOST_AUTO_TEST_CASE(VarlenDecodeAgreesWithReferenceU16) {
  check_varlen_decode_agrees_with_reference<std::uint16_t>();
}
BOOST_AUTO_TEST_CASE(VarlenDecodeAgreesWithReferenceU32) {
  check_varlen_decode_agrees_with_reference<std::uint32_t>();
}
BOOST_AUTO_TEST_CASE(VarlenDecodeAgreesWithReferenceU64) {
  check_varlen_decode_agrees_with_reference<std::uint64_t>();
}
BOOST_AUTO_TEST_CASE(VarlenDecodeAgreesWithReferenceI8) {
  check_varlen_decode_agrees_with_reference<std::int8_t>();
}
BOOST_AUTO_TEST_CASE(VarlenDecodeAgreesWithReferenceI64) {
  check_varlen_decode_agrees_with_reference<std::int64_t>();
}

// Varlen insert into a buffer shorter than the encoding fails without
// writing.
BOOST_AUTO_TEST_CASE(VarlenInsertRejectsShortBuffer) {
  pbt::check([](hegel::TestCase &tc) {
    const auto value{tc.draw("value", values<std::uint64_t>())};
    const auto needed{ref_varlen(value).size()};
    const auto size{tc.draw("buffer_size",
                            gs::integers<std::size_t>(
                                {.min_value = 0U, .max_value = needed - 1U}))};
    std::vector<std::byte> buffer(size, std::byte{0x5A});
    auto out{as_span(buffer)};
    pbt::require(!util::insert_varlen_int_to_byte_span_checked(out, value),
                 "varlen insert into a too short buffer must fail");
    pbt::require_equal(std::size(out), size, "span advanced on failure");
    pbt::require_equal(hex(to_bytes(buffer)), hex(bytes(size, 0x5AU)),
                       "buffer modified on failure");
  });
}

// --------------------------------------------------------------------------
// raw byte spans / arrays
// --------------------------------------------------------------------------

// insert_byte_span / extract_byte_span copy exactly the bytes, advance by
// their length, and the checked variants fail without side effects when the
// destination / source is too short.
BOOST_AUTO_TEST_CASE(ByteSpanCopyRoundTrip) {
  pbt::check([](hegel::TestCase &tc) {
    const auto payload{tc.draw("payload", gs::binary({.max_size = 64U}))};
    const auto size{
        tc.draw("buffer_size", gs::integers<std::size_t>({.max_value = 70U}))};
    std::vector<std::byte> buffer(size, std::byte{0x5A});
    auto out{as_span(buffer)};
    const auto source{as_const_span(payload)};
    const bool fits{payload.size() <= size};
    pbt::require_equal(util::insert_byte_span_to_byte_span_checked(out, source),
                       fits, "insert_byte_span success");
    if (!fits) {
      pbt::require_equal(std::size(out), size, "span advanced on failure");
      return;
    }
    pbt::require_equal(std::size(out), size - payload.size(),
                       "insert_byte_span remainder");
    std::vector<std::byte> target(payload.size());
    util::const_byte_span in{buffer};
    pbt::require(util::extract_byte_span_from_byte_span_checked(
                     in, std::span<std::byte>{target}),
                 "extract_byte_span failed");
    pbt::require_equal(hex(to_bytes(target)), hex(payload),
                       "byte span roundtrip");
    std::vector<std::byte> too_big(std::size(in) + 1U);
    const auto before{std::size(in)};
    pbt::require(!util::extract_byte_span_from_byte_span_checked(
                     in, std::span<std::byte>{too_big}),
                 "extract larger than the remainder must fail");
    pbt::require_equal(std::size(in), before, "span advanced on failure");
  });
}

} // namespace
