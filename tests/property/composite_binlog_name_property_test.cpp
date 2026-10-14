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

// Property-based tests for binsrv::events::composite_binlog_name - the
// "<base>.<6-digit sequence>" binlog file names. They are parsed from
// upstream ROTATE events (rotate_body_impl.cpp), from the binlog.index file
// (storage_core.cpp, one name per line), from purge_binlogs / replica
// requests, and generated from rewrite.base_file_name.
//
// Oracles:
//  - MySQL names binlog files "<base>.<number>" with the number printed as
//    "%06lu", i.e. at least 6 digits, up to MAX_LOG_UNIQUE_FN_EXT
//    (0x7FFFFFFF, sql/binlog.cc find_uniq_filename()): binlog.999999 is
//    followed by binlog.1000000;
//  - the validation rules in composite_binlog_name.cpp (non-empty base
//    without '/', sequence number 1..999999);
//  - the storage layout: names are stored one per line in binlog.index and
//    used as file / object names, so a name must not contain line breaks or
//    NUL.

#include <cstdint>
#include <stdexcept>
#include <string>

#define BOOST_TEST_MODULE CompositeBinlogNamePropertyTests
// this include is needed as it provides the 'main()' function
// NOLINTNEXTLINE(misc-include-cleaner)
#include <boost/test/unit_test.hpp>

#include "binsrv/events/composite_binlog_name.hpp"

#include "common/pbt_helpers.hpp"

namespace {

namespace gs = hegel::generators;
using binsrv::events::composite_binlog_name;

constexpr std::uint32_t max_sequence_number{999999U};
// MySQL: MAX_LOG_UNIQUE_FN_EXT
constexpr std::uint32_t mysql_max_sequence_number{0x7FFFFFFFU};

// base names the way MySQL / users spell them: log_bin basename characters
gs::Generator<std::string> base_names() {
  return gs::one_of<std::string>(
      {gs::sampled_from<std::string>(
           {"binlog", "mysql-bin", "bin.log", "a", "relay-bin", "my_host-bin"}),
       gs::from_regex("[A-Za-z0-9_.-]{1,40}", true)});
}

gs::Generator<std::uint32_t> sequence_numbers() {
  return gs::one_of<std::uint32_t>(
      {gs::sampled_from<std::uint32_t>(
           {1U, 2U, 9U, 10U, 99999U, 100000U, 999998U, max_sequence_number}),
       gs::integers<std::uint32_t>(
           {.min_value = 1U, .max_value = max_sequence_number})});
}

std::string mysql_name(const std::string &base, std::uint32_t number) {
  std::string digits{std::to_string(number)};
  if (digits.size() < 6U) {
    digits.insert(0U, 6U - digits.size(), '0');
  }
  return base + "." + digits;
}

} // namespace

// ===========================================================================
// ACCEPT direction
// ===========================================================================

// str() is "<base>.<sequence number as %06u>" and parse(str()) gives the
// same name back.
BOOST_AUTO_TEST_CASE(StrMatchesMySQLFormatAndParsesBack) {
  pbt::check([](hegel::TestCase &tc) {
    const auto base{tc.draw("base", base_names())};
    const auto number{tc.draw("number", sequence_numbers())};
    const composite_binlog_name name{base, number};
    pbt::require_equal(name.str(), mysql_name(base, number),
                       "str() differs from MySQL's '<base>.%06u' format");
    const auto parsed{composite_binlog_name::parse(name.str())};
    pbt::require(parsed == name, "parse(str()) != name");
    pbt::require_equal(parsed.get_base_name(), base, "parsed base name");
    pbt::require_equal(parsed.get_sequence_number(), number,
                       "parsed sequence number");
  });
}

// next() keeps the base name and increments the sequence number.
BOOST_AUTO_TEST_CASE(NextIncrementsSequenceNumber) {
  pbt::check([](hegel::TestCase &tc) {
    const auto base{tc.draw("base", base_names())};
    const auto number{
        tc.draw("number",
                gs::integers<std::uint32_t>(
                    {.min_value = 1U, .max_value = max_sequence_number - 1U}))};
    const composite_binlog_name name{base, number};
    const auto next{name.next()};
    pbt::require_equal(next.get_base_name(), base, "next() base name");
    pbt::require_equal(next.get_sequence_number(), number + 1U,
                       "next() sequence number");
    pbt::require_equal(composite_binlog_name::parse(next.str()).str(),
                       next.str(), "next() roundtrip");
  });
}

// Every name MySQL itself can produce for a binlog file must be accepted:
// after binlog.999999 MySQL continues with binlog.1000000 (the number is
// printed with "%06lu", so it simply gets longer). PBS parses upstream
// ROTATE event names with parse(), so rejecting them stops replication.
BOOST_AUTO_TEST_CASE(ParseAcceptsEveryMySQLBinlogName) {
  pbt::check([](hegel::TestCase &tc) {
    const auto base{tc.draw("base", base_names())};
    const auto number{tc.draw(
        "number", gs::one_of<std::uint32_t>(
                      {sequence_numbers(),
                       gs::integers<std::uint32_t>(
                           {.min_value = max_sequence_number + 1U,
                            .max_value = mysql_max_sequence_number})}))};
    const auto text{mysql_name(base, number)};
    tc.note("name = '" + text + "'");
    try {
      const auto parsed{composite_binlog_name::parse(text)};
      pbt::require_equal(parsed.get_base_name(), base, "parsed base name");
      pbt::require_equal(parsed.get_sequence_number(), number,
                         "parsed sequence number");
    } catch (const std::invalid_argument &e) {
      pbt::fail(std::string{"valid MySQL binlog name rejected: "} + e.what());
    }
  });
}

// ===========================================================================
// REJECT direction
// ===========================================================================

// The constructor enforces its documented rules: non-empty base without
// '/', sequence number 1..999999.
BOOST_AUTO_TEST_CASE(ConstructorValidatesComponents) {
  pbt::check([](hegel::TestCase &tc) {
    const auto base{tc.draw(
        "base", gs::one_of<std::string>(
                    {base_names(), gs::just(std::string{}),
                     gs::tuples(base_names(), base_names()).map([](auto pair) {
                       return std::get<0>(pair) + "/" + std::get<1>(pair);
                     })}))};
    const auto number{
        tc.draw("number", gs::one_of<std::uint32_t>(
                              {sequence_numbers(), gs::just(std::uint32_t{0U}),
                               gs::integers<std::uint32_t>(
                                   {.min_value = max_sequence_number + 1U})}))};
    const bool valid{!base.empty() && base.find('/') == std::string::npos &&
                     number >= 1U && number <= max_sequence_number};
    if (valid) {
      try {
        [[maybe_unused]] const composite_binlog_name name{base, number};
      } catch (const std::exception &e) {
        pbt::fail(std::string{"valid components rejected: "} + e.what());
      }
    } else {
      pbt::require_throws<std::invalid_argument>(
          [&] {
            [[maybe_unused]] const composite_binlog_name name{base, number};
          },
          "invalid components must be rejected");
    }
  });
}

// next() of the last 6-digit sequence number does not silently produce an
// invalid name: it throws std::invalid_argument (consistent with the
// constructor).
BOOST_AUTO_TEST_CASE(NextAtLastSequenceNumberThrows) {
  pbt::check([](hegel::TestCase &tc) {
    const auto base{tc.draw("base", base_names())};
    const composite_binlog_name name{base, max_sequence_number};
    pbt::require_throws<std::invalid_argument>(
        [&] { [[maybe_unused]] const auto next{name.next()}; },
        "next() after the maximum sequence number must throw");
  });
}

// Malformed names are rejected with std::invalid_argument: missing or
// misplaced separator, non-digit characters (sign, space, hex) in the
// sequence part, sequence number 0, empty base.
BOOST_AUTO_TEST_CASE(ParseRejectsMalformedNames) {
  pbt::check([](hegel::TestCase &tc) {
    const auto base{tc.draw("base", base_names())};
    const auto number{tc.draw("number", sequence_numbers())};
    const auto valid{mysql_name(base, number)};
    const auto suffix_start{valid.size() - 6U};
    const auto kind{
        tc.draw("kind", gs::integers<int>({.min_value = 0, .max_value = 6}))};
    std::string text{valid};
    switch (kind) {
    case 0: // separator replaced
      text[suffix_start - 1U] =
          tc.draw("separator", gs::sampled_from<char>({'_', '-', ':', 'x'}));
      break;
    case 1: { // non-digit inside the 6-digit part
      const auto position{
          tc.draw("position",
                  gs::integers<std::size_t>({.min_value = suffix_start,
                                             .max_value = text.size() - 1U}))};
      text[position] = tc.draw(
          "char", gs::sampled_from<char>({'+', '-', ' ', 'a', 'F', '\n'}));
      break;
    }
    case 2: // sequence number zero
      text = base + ".000000";
      break;
    case 3: // empty base
      text = text.substr(suffix_start - 1U);
      break;
    case 4: // too short
      text = text.substr(
          0U, tc.draw("length", gs::integers<std::size_t>({.max_value = 7U})));
      break;
    case 5: // trailing garbage after the digits
      text += tc.draw("trailing", gs::sampled_from<std::string>(
                                      {"x", " ", "\n", ".json", "-"}));
      break;
    default: // leading sign in the 7-character tail
      text = base + ".+00001";
      break;
    }
    tc.note("name = '" + text + "'");
    pbt::require_throws<std::invalid_argument>(
        [&] {
          [[maybe_unused]] const auto parsed{
              composite_binlog_name::parse(text)};
        },
        "malformed binlog name must be rejected");
  });
}

// A base name is stored in binlog.index (one name per line, read back
// line by line) and used as a file / object name, so it must not contain
// line breaks or NUL: such a name corrupts the index (the next startup
// reads two different names) or names a different file.
BOOST_AUTO_TEST_CASE(ConstructorRejectsLineBreaksAndNul) {
  pbt::check([](hegel::TestCase &tc) {
    const auto prefix{tc.draw("prefix", base_names())};
    const auto bad{
        tc.draw("bad_char", gs::sampled_from<char>({'\n', '\r', '\0'}))};
    const auto suffix{tc.draw("suffix", gs::from_regex("[a-z]{0,5}", true))};
    const std::string base{prefix + std::string(1U, bad) + suffix};
    const auto number{tc.draw("number", sequence_numbers())};
    pbt::require_throws<std::invalid_argument>(
        [&] {
          [[maybe_unused]] const composite_binlog_name name{base, number};
        },
        "base name with a line break / NUL must be rejected");
  });
}
