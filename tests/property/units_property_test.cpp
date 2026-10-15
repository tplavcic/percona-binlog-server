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

// Property-based tests for the configuration value types size_unit
// (storage.checkpoint_size, replication.rewrite.file_size) and time_unit
// (storage.checkpoint_interval), src/binsrv/size_unit.cpp / time_unit.cpp.
//
// README: "an integer followed by an optional suffix 'K' / 'M' / 'G' /
// 'T' / 'P', e.g. /\d+[KMGTP]?/" (powers of 1024) and "an integer followed
// by an optional suffix 's' / 'm' / 'h' / 'd'". Properties:
//  - every documented spelling parses to the right number of bytes /
//    seconds and prints back to an equivalent spelling;
//  - values that do not fit into 64 bits are rejected with
//    std::out_of_range, everything else that is not documented with
//    std::invalid_argument.

#define BOOST_TEST_MODULE UnitsPropertyTests

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "binsrv/size_unit.hpp"
#include "binsrv/time_unit.hpp"

#include "common/pbt_helpers.hpp"

namespace {

namespace gs = hegel::generators;

struct suffix {
  char symbol;
  std::uint64_t factor;
};

const std::vector<suffix> &size_suffixes() {
  static const std::vector<suffix> suffixes{
      {'\0', 1ULL},       {'K', 1ULL << 10U}, {'M', 1ULL << 20U},
      {'G', 1ULL << 30U}, {'T', 1ULL << 40U}, {'P', 1ULL << 50U}};
  return suffixes;
}

const std::vector<suffix> &time_suffixes() {
  static const std::vector<suffix> suffixes{
      {'\0', 1ULL}, {'s', 1ULL}, {'m', 60ULL}, {'h', 3600ULL}, {'d', 86400ULL}};
  return suffixes;
}

gs::Generator<std::uint64_t> numbers() {
  return gs::one_of<std::uint64_t>(
      {gs::integers<std::uint64_t>({.max_value = 5000U}),
       gs::integers<std::uint64_t>(),
       gs::sampled_from<std::uint64_t>(
           {0U, 1U, (1ULL << 14U) - 1U, (1ULL << 14U), (1ULL << 54U) - 1U,
            1ULL << 54U, 213503982334601ULL, 213503982334602ULL,
            std::numeric_limits<std::uint64_t>::max()})});
}

std::string spelling(std::uint64_t number, const suffix &unit) {
  auto result{std::to_string(number)};
  if (unit.symbol != '\0') {
    result += unit.symbol;
  }
  return result;
}

template <typename Unit>
void check_documented_spelling(hegel::TestCase &tc,
                               const std::vector<suffix> &suffixes) {
  const auto number{tc.draw("number", numbers())};
  const auto unit{tc.draw("suffix", gs::sampled_from(suffixes))};
  const auto text{spelling(number, unit)};
  tc.note("text = '" + text + "'");
  const bool fits{number <=
                  std::numeric_limits<std::uint64_t>::max() / unit.factor};
  if (!fits) {
    pbt::require_throws<std::out_of_range>(
        [&] { std::ignore = Unit{text}; },
        "value that does not fit into 64 bits must be rejected");
    return;
  }
  const Unit parsed{text};
  pbt::require_equal(parsed.get_value(), number * unit.factor, "get_value()");
  const Unit reparsed{parsed.to_string()};
  pbt::require_equal(reparsed.get_value(), parsed.get_value(),
                     "to_string() parses back to the same value");
}

gs::Generator<std::string> undocumented_spellings(std::string valid_suffixes) {
  return gs::compose([valid_suffixes](const hegel::TestCase &tc) {
    const auto number{std::to_string(
        tc.draw(gs::integers<std::uint64_t>({.max_value = 100000U})))};
    switch (tc.draw(gs::integers<int>({.min_value = 0, .max_value = 6}))) {
    case 0:
      return std::string{};
    case 1: // sign / whitespace
      return tc.draw(gs::sampled_from<std::string>({"+", "-", " ", "\t"})) +
             number;
    case 2: // trailing whitespace / garbage after a valid suffix
      return number + valid_suffixes.substr(0U, 1U) +
             tc.draw(gs::sampled_from<std::string>({" ", "B", "i", "x", "0"}));
    case 3: // an undocumented single-character suffix
      return number + tc.draw(gs::from_regex("[^0-9]", true).filter(
                          [valid_suffixes](const std::string &value) {
                            return valid_suffixes.find(value) ==
                                   std::string::npos;
                          }));
    case 4: // only a suffix
      return valid_suffixes.substr(0U, 1U);
    case 5: // a fraction
      return number + "." + number;
    default: // a multi-character suffix
      return number + tc.draw(gs::sampled_from<std::string>(
                          {"KB", "MiB", "sec", "ms", "min", "hours"}));
    }
  });
}

} // namespace

// "<n>" and "<n>K/M/G/T/P" give n * 1024^k bytes; to_string() round-trips;
// values above 2^64-1 are rejected with std::out_of_range.
BOOST_AUTO_TEST_CASE(SizeUnitParsesDocumentedSpellings) {
  pbt::check([](hegel::TestCase &tc) {
    check_documented_spelling<binsrv::size_unit>(tc, size_suffixes());
  });
}

// "<n>" and "<n>s/m/h/d" give n * 1 / 60 / 3600 / 86400 seconds;
// to_string() round-trips; values above 2^64-1 seconds are rejected.
BOOST_AUTO_TEST_CASE(TimeUnitParsesDocumentedSpellings) {
  pbt::check([](hegel::TestCase &tc) {
    check_documented_spelling<binsrv::time_unit>(tc, time_suffixes());
  });
}

// Anything else (empty, signs, whitespace, fractions, other or longer
// suffixes, a suffix without a number) is rejected with
// std::invalid_argument.
BOOST_AUTO_TEST_CASE(SizeUnitRejectsUndocumentedSpellings) {
  pbt::check([](hegel::TestCase &tc) {
    const auto text{tc.draw("text", undocumented_spellings("KMGTP"))};
    pbt::require_throws<std::invalid_argument>(
        [&] { std::ignore = binsrv::size_unit{text}; },
        "undocumented size spelling must be rejected");
  });
}

BOOST_AUTO_TEST_CASE(TimeUnitRejectsUndocumentedSpellings) {
  pbt::check([](hegel::TestCase &tc) {
    const auto text{tc.draw("text", undocumented_spellings("smhd"))};
    pbt::require_throws<std::invalid_argument>(
        [&] { std::ignore = binsrv::time_unit{text}; },
        "undocumented time spelling must be rejected");
  });
}
