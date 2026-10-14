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

// Property-based tests for binsrv::gtids::gtid_set (and the uuid / tag / gtid
// components it is built from).
//
// Oracles:
//  - an independent reference model (common/gtid_model.hpp) for the set
//    semantics, the canonical text form and the MySQL binary encoding;
//  - MySQL server behaviour for the same inputs (sql/rpl_gtid_set.cc,
//    libs/mysql/gtid/tag.cpp, the GTID-concepts chapter of the manual);
//  - the BNF grammar documented in src/binsrv/gtids/gtid_set.cpp.
//
// Each BOOST_AUTO_TEST_CASE is one property and can be run individually:
//   ./gtid_set_property_test --run_test=TextRoundTrip

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define BOOST_TEST_MODULE GtidSetPropertyTests
// this include is needed as it provides the 'main()' function
// NOLINTNEXTLINE(misc-include-cleaner)
#include <boost/test/unit_test.hpp>

#include "binsrv/gtids/common_types.hpp"
#include "binsrv/gtids/gtid.hpp"
#include "binsrv/gtids/gtid_set.hpp"
#include "binsrv/gtids/tag.hpp"
#include "binsrv/gtids/uuid.hpp"

#include "util/byte_span_fwd.hpp"

#include "common/gtid_model.hpp"
#include "common/pbt_helpers.hpp"

namespace {

namespace gs = hegel::generators;
namespace model = pbt::gtid_model;
namespace gtids = binsrv::gtids;

// ---------------------------------------------------------------------------
// conversions between the model and the system under test
// ---------------------------------------------------------------------------
gtids::uuid to_sut_uuid(const model::raw_uuid &raw) {
  gtids::uuid_storage storage{};
  std::ranges::transform(raw, std::begin(storage),
                         [](std::uint8_t value) { return std::byte{value}; });
  return gtids::uuid{storage};
}

gtids::tag to_sut_tag(const std::string &name) { return gtids::tag{name}; }

// Builds the SUT set through add_interval() only. Other entry points are
// compared against this one by dedicated properties.
gtids::gtid_set to_sut(const std::vector<model::element> &spec) {
  gtids::gtid_set result;
  for (const auto &item : spec) {
    result.add_interval(to_sut_uuid(item.key.uuid), to_sut_tag(item.key.tag),
                        item.range.lower, item.range.upper);
  }
  return result;
}

std::vector<std::uint8_t> encode(const gtids::gtid_set &gtids_value) {
  // generous slack so that an under-estimated size cannot write out of
  // bounds; the exact size is verified by a dedicated property
  static constexpr std::size_t slack{256U};
  const auto declared_size{gtids_value.calculate_encoded_size()};
  std::vector<std::byte> buffer(declared_size + slack);
  util::byte_span destination{buffer};
  gtids_value.encode_to(destination);
  const auto written{std::size(buffer) - std::size(destination)};
  std::vector<std::uint8_t> result(written);
  std::ranges::transform(
      std::span{buffer}.first(written), std::begin(result),
      [](std::byte value) { return std::to_integer<std::uint8_t>(value); });
  return result;
}

gtids::gtid_set decode(const std::vector<std::uint8_t> &bytes) {
  const util::const_byte_span span{
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<const std::byte *>(std::data(bytes)), std::size(bytes)};
  return gtids::gtid_set{span};
}

std::string hex_dump(const std::vector<std::uint8_t> &bytes) {
  static constexpr std::string_view digits{"0123456789abcdef"};
  std::string result;
  for (const auto value : bytes) {
    result += digits[value >> 4U];
    result += digits[value & 0x0FU];
  }
  return result;
}

// Invariants every gtid_set value must satisfy no matter how it was built
// (text, binary, API): an "empty" set prints as "", and the printed text
// parses back into an equal set.
void require_self_consistent(const gtids::gtid_set &value,
                             std::string_view context) {
  const auto text{value.str()};
  pbt::require_equal(value.is_empty(), text.empty(),
                     std::string{context} +
                         ": is_empty() must agree with str() being empty "
                         "(str()='" +
                         text + "')");
  gtids::gtid_set reparsed;
  try {
    reparsed = gtids::gtid_set{text};
  } catch (const std::exception &e) {
    pbt::fail(std::string{context} + ": str() output '" + text +
              "' cannot be parsed back: " + e.what());
  }
  pbt::require_equal(reparsed, value,
                     std::string{context} + ": gtid_set(str()) roundtrip");
}

// ---------------------------------------------------------------------------
// operation-sequence model test
// ---------------------------------------------------------------------------
enum class op_kind : std::uint8_t {
  add_gtid,
  add_interval,
  subtract_gtid,
  subtract_interval,
  add_set,
  subtract_set,
  clear
};

struct operation {
  op_kind kind{};
  model::element target{};
  std::vector<model::element> other{};
};

std::ostream &operator<<(std::ostream &output, const operation &obj) {
  static constexpr std::array names{
      "add_gtid", "add_interval", "subtract_gtid", "subtract_interval",
      "add_set",  "subtract_set", "clear"};
  output << names[static_cast<std::size_t>(obj.kind)] << " " << obj.target;
  if (!obj.other.empty()) {
    output << " other=[";
    for (const auto &item : obj.other) {
      output << item << " ";
    }
    output << "]";
  }
  return output;
}

gs::Generator<operation> operations() {
  return gs::builds<operation>(
      gs::sampled_from<op_kind>({op_kind::add_gtid, op_kind::add_interval,
                                 op_kind::subtract_gtid,
                                 op_kind::subtract_interval, op_kind::add_set,
                                 op_kind::subtract_set, op_kind::clear}),
      model::elements(), model::element_lists(4U));
}

} // namespace

// ===========================================================================
// ACCEPT direction: valid inputs must give the right value
// ===========================================================================

// Any sequence of add / subtract / clear operations leaves the gtid_set in
// the same state as the reference model: same canonical text (MySQL format:
// UUIDs ordered, intervals merged and ascending), same is_empty(),
// contains_tags() and contains() answers at every interval boundary.
BOOST_AUTO_TEST_CASE(ModelAgreementAfterOperationSequence) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto ops{
            tc.draw("ops", gs::vectors(operations(), {.max_size = 25U}))};
        gtids::gtid_set sut;
        model::gtid_set expected;
        for (const auto &op : ops) {
          const auto uuid{to_sut_uuid(op.target.key.uuid)};
          const auto tag{to_sut_tag(op.target.key.tag)};
          const auto lower{op.target.range.lower};
          const auto upper{op.target.range.upper};
          switch (op.kind) {
          case op_kind::add_gtid:
            sut.add(gtids::gtid{uuid, tag, lower});
            expected.add(op.target.key, lower, lower);
            break;
          case op_kind::add_interval:
            sut.add_interval(uuid, tag, lower, upper);
            expected.add(op.target.key, lower, upper);
            break;
          case op_kind::subtract_gtid:
            sut.subtract(gtids::gtid{uuid, tag, lower});
            expected.subtract(op.target.key, lower, lower);
            break;
          case op_kind::subtract_interval:
            sut.subtract_interval(uuid, tag, lower, upper);
            expected.subtract(op.target.key, lower, upper);
            break;
          case op_kind::add_set:
            sut.add(to_sut(op.other));
            expected.add(model::to_model(op.other));
            break;
          case op_kind::subtract_set:
            sut.subtract(to_sut(op.other));
            expected.subtract(model::to_model(op.other));
            break;
          case op_kind::clear:
            sut.clear();
            expected = model::gtid_set{};
            break;
          }
        }

        pbt::require_equal(sut.str(), expected.str(),
                           "str() differs from the reference model");
        pbt::require_equal(sut.is_empty(), expected.is_empty(),
                           "is_empty() differs from the reference model");
        pbt::require_equal(sut.contains_tags(), expected.contains_tags(),
                           "contains_tags() differs from the reference model");
        for (const auto &[key, intervals] : expected.data()) {
          for (const auto &[lo, hi] : intervals.intervals()) {
            for (const std::uint64_t probe :
                 std::array<std::uint64_t, 4U>{lo - 1U, lo, hi, hi + 1U}) {
              if (probe < model::min_gno || probe > model::max_gno) {
                continue;
              }
              const gtids::gtid probe_gtid{to_sut_uuid(key.uuid),
                                           to_sut_tag(key.tag), probe};
              pbt::require_equal(sut.contains(probe_gtid),
                                 expected.contains(key, probe),
                                 "contains(" + probe_gtid.str() +
                                     ") differs from the reference model");
            }
          }
        }
      },
      {.test_cases = 20000U});
}

// gtid_set(s.str()) == s, and str() is stable across the roundtrip.
BOOST_AUTO_TEST_CASE(TextRoundTrip) {
  pbt::check([](hegel::TestCase &tc) {
    const auto spec{tc.draw("spec", model::element_lists())};
    const auto original{to_sut(spec)};
    const auto text{original.str()};
    const gtids::gtid_set reparsed{text};
    pbt::require_equal(reparsed, original, "gtid_set(str()) != original");
    pbt::require_equal(reparsed.str(), text, "str() is not stable");
  });
}

// Every spelling the documented grammar allows for the same set parses to
// that set: upper/lower case hex digits, any interval order, overlapping and
// duplicated intervals, "n-n" for a single gno, the same UUID repeated in
// several comma-separated chunks, any whitespace after a comma.
BOOST_AUTO_TEST_CASE(ParseAcceptsEquivalentSpellings) {
  pbt::check([](hegel::TestCase &tc) {
    const auto spec{
        tc.draw("spec", gs::vectors(model::elements(),
                                    {.min_size = 1U, .max_size = 10U}))};
    const auto upper_hex{
        tc.draw("upper_hex", gs::vectors(gs::booleans(),
                                         {.min_size = 10U, .max_size = 10U}))};
    const auto separators{tc.draw(
        "separators", gs::vectors(gs::sampled_from({", ", ",", ",  ", ",\t",
                                                    ",\n", ", \r\n"}),
                                  {.min_size = 10U, .max_size = 10U}))};
    const auto use_range_form{tc.draw(
        "use_range_form",
        gs::vectors(gs::booleans(), {.min_size = 10U, .max_size = 10U}))};

    // one "uuid[:tag]:interval" chunk per element, in generated order
    // (i.e. not sorted, possibly overlapping, possibly repeating UUIDs)
    std::string text;
    for (std::size_t i{0U}; i < spec.size(); ++i) {
      const auto &item{spec[i]};
      if (i != 0U) {
        text += separators[i];
      }
      text += model::uuid_to_string(item.key.uuid, upper_hex[i]);
      if (!item.key.tag.empty()) {
        text += ':';
        text += item.key.tag;
      }
      text += ':';
      text += std::to_string(item.range.lower);
      if (item.range.lower != item.range.upper || use_range_form[i]) {
        text += '-';
        text += std::to_string(item.range.upper);
      }
    }
    tc.note("text = '" + text + "'");

    gtids::gtid_set parsed;
    try {
      parsed = gtids::gtid_set{text};
    } catch (const std::exception &e) {
      pbt::fail("valid GTID set text rejected: " + std::string{e.what()});
    }
    pbt::require_equal(parsed.str(), model::to_model(spec).str(),
                       "parsed set differs from the reference model");
  });
}

// decode(encode(s)) == s
BOOST_AUTO_TEST_CASE(BinaryRoundTrip) {
  pbt::check([](hegel::TestCase &tc) {
    const auto spec{tc.draw("spec", model::element_lists())};
    const auto original{to_sut(spec)};
    const auto bytes{encode(original)};
    tc.note("encoded = " + hex_dump(bytes));
    gtids::gtid_set decoded;
    try {
      decoded = decode(bytes);
    } catch (const std::exception &e) {
      pbt::fail("own encoding cannot be decoded: " + std::string{e.what()});
    }
    pbt::require_equal(decoded, original, "decode(encode(s)) != s");
  });
}

// calculate_encoded_size() is exactly the number of bytes encode_to()
// writes (callers allocate buffers with it).
BOOST_AUTO_TEST_CASE(EncodedSizeMatchesBytesWritten) {
  pbt::check([](hegel::TestCase &tc) {
    const auto spec{tc.draw("spec", model::element_lists())};
    const auto value{to_sut(spec)};
    pbt::require_equal(encode(value).size(), value.calculate_encoded_size(),
                       "calculate_encoded_size() != bytes written");
  });
}

// The binary form is byte-for-byte the one MySQL 8.4 produces
// (Gtid_set::encode()): it is written into binlog events and must be
// readable by mysqld / mysqlbinlog.
BOOST_AUTO_TEST_CASE(BinaryEncodingMatchesMySQLLayout) {
  pbt::check([](hegel::TestCase &tc) {
    const auto spec{tc.draw("spec", model::element_lists())};
    const auto actual{encode(to_sut(spec))};
    const auto expected{model::to_model(spec).encode()};
    pbt::require_equal(hex_dump(actual), hex_dump(expected),
                       "binary encoding differs from the MySQL layout");
  });
}

// All ways of building a single-GTID set agree: gtid_set(gtid), add(gtid),
// add(uuid, tag, gno), add_interval(uuid, tag, gno, gno), and parsing
// "uuid[:tag]:gno"; and gtid::str() is exactly that text.
BOOST_AUTO_TEST_CASE(SingleGtidEntryPointsAgree) {
  pbt::check([](hegel::TestCase &tc) {
    const auto key{tc.draw("tsid", model::tsids())};
    const auto gno{tc.draw("gno", model::gnos())};
    const auto uuid{to_sut_uuid(key.uuid)};
    const auto tag{to_sut_tag(key.tag)};
    const gtids::gtid single{uuid, tag, gno};

    const gtids::gtid_set from_ctor{single};
    gtids::gtid_set from_add_gtid;
    from_add_gtid.add(single);
    gtids::gtid_set from_add_components;
    from_add_components.add(uuid, tag, gno);
    gtids::gtid_set from_add_interval;
    from_add_interval.add_interval(uuid, tag, gno, gno);
    const gtids::gtid_set from_text{single.str()};

    model::gtid_set expected;
    expected.add(key, gno, gno);
    pbt::require_equal(single.str(), expected.str(),
                       "gtid::str() differs from MySQL text form");
    pbt::require_equal(from_ctor.str(), expected.str(), "gtid_set(gtid)");
    pbt::require_equal(from_add_gtid, from_ctor, "add(gtid)");
    pbt::require_equal(from_add_components, from_ctor, "add(uuid,tag,gno)");
    pbt::require_equal(from_add_interval, from_ctor,
                       "add_interval(uuid,tag,gno,gno)");
    pbt::require_equal(from_text, from_ctor, "gtid_set(gtid.str())");
  });
}

// Union is commutative, associative and idempotent with the empty set as
// identity, and agrees with the model.
BOOST_AUTO_TEST_CASE(UnionLaws) {
  pbt::check([](hegel::TestCase &tc) {
    const auto spec_a{tc.draw("a", model::element_lists(6U))};
    const auto spec_b{tc.draw("b", model::element_lists(6U))};
    const auto spec_c{tc.draw("c", model::element_lists(6U))};
    const auto a{to_sut(spec_a)};
    const auto b{to_sut(spec_b)};
    const auto c{to_sut(spec_c)};
    pbt::require_equal(a + b, b + a, "a + b == b + a");
    pbt::require_equal((a + b) + c, a + (b + c), "(a + b) + c == a + (b + c)");
    pbt::require_equal(a + a, a, "a + a == a");
    pbt::require_equal(a + gtids::gtid_set{}, a, "a + {} == a");
    auto expected{model::to_model(spec_a)};
    expected.add(model::to_model(spec_b));
    pbt::require_equal((a + b).str(), expected.str(), "a + b vs model");
  });
}

// Difference laws: a - a == {}, a - {} == a, (a - b) does not intersect b,
// (a + b) - b == a - b, and the result agrees with the model.
BOOST_AUTO_TEST_CASE(SubtractionLaws) {
  pbt::check([](hegel::TestCase &tc) {
    const auto spec_a{tc.draw("a", model::element_lists(8U))};
    const auto spec_b{tc.draw("b", model::element_lists(8U))};
    const auto a{to_sut(spec_a)};
    const auto b{to_sut(spec_b)};
    pbt::require((a - a).is_empty(), "(a - a).is_empty()");
    pbt::require_equal(a - a, gtids::gtid_set{}, "a - a == {}");
    pbt::require_equal(a - gtids::gtid_set{}, a, "a - {} == a");
    pbt::require(!intersects(a - b, b), "!intersects(a - b, b)");
    pbt::require_equal((a + b) - b, a - b, "(a + b) - b == a - b");
    auto expected{model::to_model(spec_a)};
    expected.subtract(model::to_model(spec_b));
    pbt::require_equal((a - b).str(), expected.str(), "a - b vs model");
  });
}

// intersects() is symmetric and true exactly when the model intersection is
// non-empty.
BOOST_AUTO_TEST_CASE(IntersectsAgreesWithModel) {
  pbt::check([](hegel::TestCase &tc) {
    const auto spec_a{tc.draw("a", model::element_lists(6U))};
    const auto spec_b{tc.draw("b", model::element_lists(6U))};
    const auto a{to_sut(spec_a)};
    const auto b{to_sut(spec_b)};
    // a ∩ b == a - (a - b)
    auto difference{model::to_model(spec_a)};
    difference.subtract(model::to_model(spec_b));
    auto intersection{model::to_model(spec_a)};
    intersection.subtract(difference);
    pbt::require_equal(intersects(a, b), !intersection.is_empty(),
                       "intersects(a, b) vs model");
    pbt::require_equal(intersects(a, b), intersects(b, a),
                       "intersects is symmetric");
  });
}

// operator>> (used when GTID sets are read from configuration / command
// line) agrees with the string constructor on canonical text.
BOOST_AUTO_TEST_CASE(StreamExtractionAgreesWithStringConstructor) {
  pbt::check([](hegel::TestCase &tc) {
    const auto spec{tc.draw("spec", model::element_lists())};
    const auto original{to_sut(spec)};
    std::istringstream input{original.str()};
    gtids::gtid_set extracted;
    input >> extracted;
    pbt::require(!input.fail(), "operator>> failed on canonical text '" +
                                    original.str() + "'");
    pbt::require_equal(extracted, original, "operator>> result");
  });
}

// MySQL normalises GTID tags to lower case (libs/mysql/gtid/tag.cpp,
// Tag::replace() applies tolower; the grammar accepts [a-zA-Z_]...), so
// "uuid:ABC:1" and "uuid:abc:1" denote the same GTID. Upper-case spellings
// arrive from users (search_by_gtid_set argument, configuration).
BOOST_AUTO_TEST_CASE(TagsAreCaseInsensitiveLikeMySQL) {
  pbt::check([](hegel::TestCase &tc) {
    const auto spec{
        tc.draw("spec", gs::vectors(model::elements(),
                                    {.min_size = 1U, .max_size = 6U}))};
    const auto canonical{model::to_model(spec)};
    tc.assume(canonical.contains_tags());
    std::string upper_text{canonical.str()};
    // upper-case only the tag letters: tags follow a ':' and start with a
    // letter or '_' (gnos start with a digit, the uuid is not affected)
    bool in_tag{false};
    for (std::size_t i{0U}; i < upper_text.size(); ++i) {
      auto &ch{upper_text[i]};
      if (ch == ':' || ch == ',') {
        in_tag = (ch == ':') && i + 1U < upper_text.size() &&
                 (std::isalpha(
                      static_cast<unsigned char>(upper_text[i + 1U])) != 0 ||
                  upper_text[i + 1U] == '_');
        continue;
      }
      if (in_tag) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      }
    }
    tc.note("upper-case spelling = '" + upper_text + "'");
    gtids::gtid_set parsed;
    try {
      parsed = gtids::gtid_set{upper_text};
    } catch (const std::exception &e) {
      pbt::fail("upper-case tag spelling rejected: " + std::string{e.what()});
    }
    pbt::require_equal(parsed.str(), canonical.str(),
                       "upper-case tag spelling must denote the same set");
  });
}

// ===========================================================================
// REJECT direction: invalid inputs must be refused, never silently accepted
// ===========================================================================

// Text that is a valid GTID set followed by characters that cannot continue
// the grammar must be rejected (MySQL: Gtid_set::add_gtid_text ->
// parse_error), not silently truncated to the valid prefix.
BOOST_AUTO_TEST_CASE(ParseRejectsTrailingGarbage) {
  pbt::check([](hegel::TestCase &tc) {
    const auto spec{
        tc.draw("spec", gs::vectors(model::elements(),
                                    {.min_size = 1U, .max_size = 6U}))};
    const auto valid_text{model::to_model(spec).str()};
    // after a gno only '-', ':', ',', digits or end-of-text may follow;
    // optional whitespace before the garbage is also exercised
    const auto lead{tc.draw("lead", gs::sampled_from({"", " ", "\t"}))};
    const auto first{tc.draw(
        "first", gs::sampled_from({"x", "Z", "_", ";", "#", ")", "!", "/", ".",
                                   "+", "*", "\"", "|", "abc"}))};
    const auto rest{
        tc.draw("rest", gs::text({.max_size = 8U, .codec = "ascii"}))};
    const auto text{valid_text + lead + first + rest};
    tc.note("text = '" + text + "'");
    pbt::require_throws<std::invalid_argument>(
        [&] { [[maybe_unused]] const gtids::gtid_set parsed{text}; },
        "GTID set text with trailing garbage must be rejected");
  });
}

// gno values outside 1..2^63-1 must be rejected wherever they appear (single
// gno, interval start, interval end), including values that do not fit into
// 64 bits (MySQL: parse_gno() fails on overflow).
BOOST_AUTO_TEST_CASE(ParseRejectsOutOfRangeGno) {
  pbt::check([](hegel::TestCase &tc) {
    const auto key{tc.draw("tsid", model::tsids())};
    const auto bad{tc.draw(
        "bad_gno",
        gs::one_of<std::string>(
            {gs::just(std::string{"0"}),
             gs::integers<std::uint64_t>({.min_value = model::max_gno + 1ULL})
                 .map([](std::uint64_t v) { return std::to_string(v); }),
             // > 2^64: append digits to a large number
             gs::tuples(gs::integers<std::uint64_t>(
                            {.min_value = 1844674407370955162ULL}),
                        gs::integers<int>({.min_value = 0, .max_value = 9}))
                 .map([](auto pair) {
                   return std::to_string(std::get<0>(pair)) +
                          std::to_string(std::get<1>(pair));
                 })}))};
    const auto position{tc.draw(
        "position", gs::integers<int>({.min_value = 0, .max_value = 2}))};
    std::string prefix{model::uuid_to_string(key.uuid)};
    if (!key.tag.empty()) {
      prefix += ":" + key.tag;
    }
    std::string text;
    switch (position) {
    case 0:
      text = prefix + ":" + bad;
      break;
    case 1:
      text = prefix + ":" + bad + "-" + bad + "9";
      break;
    default:
      text = prefix + ":1-" + bad;
      break;
    }
    tc.note("text = '" + text + "'");
    pbt::require_throws<std::invalid_argument>(
        [&] { [[maybe_unused]] const gtids::gtid_set parsed{text}; },
        "GTID set text with an out-of-range gno must be rejected");
  });
}

// An interval whose end is smaller than its start is invalid.
BOOST_AUTO_TEST_CASE(ParseRejectsReversedInterval) {
  pbt::check([](hegel::TestCase &tc) {
    const auto key{tc.draw("tsid", model::tsids())};
    const auto range{tc.draw("range", model::intervals())};
    tc.assume(range.lower < range.upper);
    std::string text{model::uuid_to_string(key.uuid)};
    if (!key.tag.empty()) {
      text += ":" + key.tag;
    }
    text +=
        ":" + std::to_string(range.upper) + "-" + std::to_string(range.lower);
    pbt::require_throws<std::invalid_argument>(
        [&] { [[maybe_unused]] const gtids::gtid_set parsed{text}; },
        "reversed interval must be rejected");
  });
}

// Arbitrary text (including NUL, control characters and non-ASCII bytes)
// either throws std::invalid_argument or yields a self-consistent set; it
// never crashes or throws anything else.
BOOST_AUTO_TEST_CASE(ParseArbitraryTextIsRejectedOrConsistent) {
  pbt::check(
      [](hegel::TestCase &tc) {
        // mutate a valid text so that most inputs are "near-valid"
        const auto spec{tc.draw("spec", model::element_lists(4U))};
        auto text{model::to_model(spec).str()};
        const auto edits{tc.draw(
            "edits",
            gs::vectors(
                gs::tuples(gs::integers<std::size_t>({.max_value = 1000U}),
                           gs::binary({.min_size = 0U, .max_size = 3U}),
                           gs::integers<std::size_t>({.max_value = 3U})),
                {.max_size = 4U}))};
        for (const auto &[position, insert, erase] : edits) {
          const auto at{text.empty() ? 0U : position % (text.size() + 1U)};
          text.erase(at, erase);
          text.insert(at, std::string(std::begin(insert), std::end(insert)));
        }
        gtids::gtid_set parsed;
        try {
          parsed = gtids::gtid_set{text};
        } catch (const std::invalid_argument &) {
          return;
        } catch (const std::exception &e) {
          pbt::fail(std::string{"unexpected exception type: "} + e.what());
        }
        require_self_consistent(parsed, "parsed from '" + text + "'");
      },
      {.test_cases = 20000U});
}

// add_interval() must validate the upper bound like add() validates a single
// gno: an end above 2^63-1 is rejected with std::invalid_argument.
BOOST_AUTO_TEST_CASE(AddIntervalRejectsOutOfRangeUpperBound) {
  pbt::check([](hegel::TestCase &tc) {
    const auto key{tc.draw("tsid", model::tsids())};
    const auto lower{tc.draw("lower", model::gnos())};
    const auto upper{tc.draw(
        "upper",
        gs::integers<std::uint64_t>({.min_value = model::max_gno + 1ULL}))};
    const auto uuid{to_sut_uuid(key.uuid)};
    const auto tag{to_sut_tag(key.tag)};
    gtids::gtid_set value;
    pbt::require_throws<std::invalid_argument>(
        [&] { value.add(uuid, tag, upper); },
        "add() with gno > 2^63-1 must be rejected");
    pbt::require_throws<std::invalid_argument>(
        [&] { value.add_interval(uuid, tag, lower, upper); },
        "add_interval() with upper bound > 2^63-1 must be rejected");
    require_self_consistent(value, "after rejected add_interval()");
  });
}

// subtract_interval() must treat the upper bound like subtract() treats a
// single gno: an end above 2^63-1 is rejected with std::invalid_argument
// and the set is left unchanged.
BOOST_AUTO_TEST_CASE(SubtractIntervalRejectsOutOfRangeUpperBound) {
  pbt::check([](hegel::TestCase &tc) {
    const auto spec{tc.draw("spec", model::element_lists(6U))};
    const auto key{tc.draw("tsid", model::tsids())};
    const auto lower{tc.draw("lower", model::gnos())};
    const auto upper{tc.draw(
        "upper",
        gs::integers<std::uint64_t>({.min_value = model::max_gno + 1ULL}))};
    const auto original{to_sut(spec)};
    auto value{original};
    pbt::require_throws<std::invalid_argument>(
        [&] {
          value.subtract_interval(to_sut_uuid(key.uuid), to_sut_tag(key.tag),
                                  lower, upper);
        },
        "subtract_interval() with upper bound > 2^63-1 must be rejected");
    pbt::require_equal(value, original, "set changed by a rejected call");
  });
}

// ---------------------------------------------------------------------------
// binary decoding of malformed input
// ---------------------------------------------------------------------------
namespace {

enum class binary_mutation : std::uint8_t {
  zero_intervals,      // a TSID with an interval count of 0
  zero_lower_bound,    // interval start = 0
  bound_above_max_gno, // interval end > 2^63 (signed overflow in MySQL)
  unsorted_intervals,  // intervals not strictly increasing
  adjacent_intervals,  // [a,b) followed by [b,c): not normalized
  nil_uuid,            // UUID 00000000-0000-0000-0000-000000000000
  tag_too_long,        // varlen tag length 33..127
  invalid_tag_chars,   // tag containing characters outside [a-z0-9_]
  truncated,           // drop the last N bytes
  trailing_bytes       // append bytes
};

void put_u64(std::vector<std::uint8_t> &out, std::uint64_t value) {
  for (std::size_t i{0U}; i < 8U; ++i) {
    out.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
  }
}

// Builds a single-TSID encoding by hand so that every field can be set to a
// malformed value.
std::vector<std::uint8_t>
build_encoding(const model::raw_uuid &uuid, const std::string *tag,
               const std::vector<std::pair<std::uint64_t, std::uint64_t>>
                   &half_open_intervals,
               std::uint8_t tag_length_byte = 0U) {
  std::vector<std::uint8_t> out;
  if (tag != nullptr) {
    put_u64(out, (1ULL << 56U) | (1ULL << 8U) | 1ULL);
  } else {
    put_u64(out, 1ULL);
  }
  out.insert(std::end(out), std::begin(uuid), std::end(uuid));
  if (tag != nullptr) {
    out.push_back(tag_length_byte != 0U
                      ? tag_length_byte
                      : static_cast<std::uint8_t>(tag->size() << 1U));
    out.insert(std::end(out), std::begin(*tag), std::end(*tag));
  }
  put_u64(out, half_open_intervals.size());
  for (const auto &[start, end] : half_open_intervals) {
    put_u64(out, start);
    put_u64(out, end);
  }
  return out;
}

} // namespace

namespace {

// Builds one malformed encoding of the given class and checks that it is
// either rejected with std::invalid_argument or, for encodings MySQL's
// Gtid_set::add_gtid_encoding() accepts, decoded into a self-consistent set.
// MySQL refuses: start <= previous end (so start 0, unsorted, overlapping
// and adjacent intervals), end <= start, values >= 2^63 (signed), invalid
// tags and any length mismatch.
void check_malformed_binary(hegel::TestCase &tc, binary_mutation mutation) {
  const auto uuid{tc.draw("uuid", model::uuids())};
  const auto tag_name{tc.draw("tag", model::tags())};
  const auto a{tc.draw("a", gs::integers<std::uint64_t>(
                                {.min_value = 1ULL, .max_value = 1000ULL}))};
  const auto len{tc.draw("len", gs::integers<std::uint64_t>(
                                    {.min_value = 1ULL, .max_value = 50ULL}))};
  const std::string *tag{tag_name.empty() ? nullptr : &tag_name};

  bool mysql_rejects{true};
  std::vector<std::uint8_t> bytes;
  switch (mutation) {
  case binary_mutation::zero_intervals:
    bytes = build_encoding(uuid, tag, {});
    mysql_rejects = false;
    break;
  case binary_mutation::zero_lower_bound:
    bytes = build_encoding(uuid, tag, {{0ULL, len}});
    break;
  case binary_mutation::bound_above_max_gno:
    bytes = build_encoding(uuid, tag, {{a, model::max_gno + 1ULL + len}});
    break;
  case binary_mutation::unsorted_intervals:
    bytes = build_encoding(uuid, tag,
                           {{a + len + 5ULL, a + len + 10ULL}, {a, a + len}});
    break;
  case binary_mutation::adjacent_intervals:
    bytes = build_encoding(uuid, tag, {{a, a + len}, {a + len, a + 2 * len}});
    break;
  case binary_mutation::nil_uuid:
    bytes = build_encoding(model::raw_uuid{}, tag, {{a, a + len}});
    // MySQL does not validate the UUID while decoding, so only
    // self-consistency is required
    mysql_rejects = false;
    break;
  case binary_mutation::tag_too_long: {
    const std::string long_tag(32U + (len % 20U) + 1U, 'a');
    bytes = build_encoding(uuid, &long_tag, {{a, a + len}});
    break;
  }
  case binary_mutation::invalid_tag_chars: {
    const auto bad{tc.draw(
        "bad_tag",
        gs::sampled_from<std::string>({"1abc", "a-b", "a b", "a:b",
                                       std::string{"a\0b", 3U}, "\xC3\xA9"}))};
    bytes = build_encoding(uuid, &bad, {{a, a + len}});
    break;
  }
  case binary_mutation::truncated: {
    bytes = build_encoding(uuid, tag, {{a, a + len}});
    bytes.resize(bytes.size() -
                 static_cast<std::size_t>(1ULL + (len % (bytes.size() - 1U))));
    break;
  }
  case binary_mutation::trailing_bytes:
    bytes = build_encoding(uuid, tag, {{a, a + len}});
    bytes.insert(std::end(bytes), static_cast<std::size_t>(len % 7ULL) + 1U,
                 std::uint8_t{0U});
    break;
  }
  tc.note("bytes = " + hex_dump(bytes));

  gtids::gtid_set decoded;
  try {
    decoded = decode(bytes);
  } catch (const std::invalid_argument &) {
    return; // rejected with the documented exception type
  } catch (const std::exception &e) {
    pbt::fail(std::string{"malformed encoding rejected with an unexpected "
                          "exception type: "} +
              boost::core::demangle(typeid(e).name()) + " (" + e.what() + ")");
  }
  pbt::require(!mysql_rejects,
               "malformed encoding accepted (MySQL rejects it); decoded as '" +
                   decoded.str() + "'");
  require_self_consistent(decoded, "decoded from malformed binary");
}

constexpr std::uint64_t malformed_binary_cases{2000U};

} // namespace

// One property per malformation class, so that every distinct defect has
// its own test id (see extra/qa/known_bugs.md).
BOOST_AUTO_TEST_CASE(DecodeZeroIntervalTsidIsConsistent) {
  pbt::check(
      [](hegel::TestCase &tc) {
        check_malformed_binary(tc, binary_mutation::zero_intervals);
      },
      {.test_cases = malformed_binary_cases});
}
BOOST_AUTO_TEST_CASE(DecodeRejectsZeroLowerBound) {
  pbt::check(
      [](hegel::TestCase &tc) {
        check_malformed_binary(tc, binary_mutation::zero_lower_bound);
      },
      {.test_cases = malformed_binary_cases});
}
BOOST_AUTO_TEST_CASE(DecodeRejectsBoundAboveMaxGno) {
  pbt::check(
      [](hegel::TestCase &tc) {
        check_malformed_binary(tc, binary_mutation::bound_above_max_gno);
      },
      {.test_cases = malformed_binary_cases});
}
BOOST_AUTO_TEST_CASE(DecodeRejectsUnsortedIntervals) {
  pbt::check(
      [](hegel::TestCase &tc) {
        check_malformed_binary(tc, binary_mutation::unsorted_intervals);
      },
      {.test_cases = malformed_binary_cases});
}
BOOST_AUTO_TEST_CASE(DecodeRejectsAdjacentIntervals) {
  pbt::check(
      [](hegel::TestCase &tc) {
        check_malformed_binary(tc, binary_mutation::adjacent_intervals);
      },
      {.test_cases = malformed_binary_cases});
}
BOOST_AUTO_TEST_CASE(DecodeNilUuidIsConsistent) {
  pbt::check(
      [](hegel::TestCase &tc) {
        check_malformed_binary(tc, binary_mutation::nil_uuid);
      },
      {.test_cases = malformed_binary_cases});
}
BOOST_AUTO_TEST_CASE(DecodeRejectsTooLongTag) {
  pbt::check(
      [](hegel::TestCase &tc) {
        check_malformed_binary(tc, binary_mutation::tag_too_long);
      },
      {.test_cases = malformed_binary_cases});
}
BOOST_AUTO_TEST_CASE(DecodeRejectsInvalidTagCharacters) {
  pbt::check(
      [](hegel::TestCase &tc) {
        check_malformed_binary(tc, binary_mutation::invalid_tag_chars);
      },
      {.test_cases = malformed_binary_cases});
}
BOOST_AUTO_TEST_CASE(DecodeRejectsTruncatedInput) {
  pbt::check(
      [](hegel::TestCase &tc) {
        check_malformed_binary(tc, binary_mutation::truncated);
      },
      {.test_cases = malformed_binary_cases});
}
BOOST_AUTO_TEST_CASE(DecodeRejectsTrailingBytes) {
  pbt::check(
      [](hegel::TestCase &tc) {
        check_malformed_binary(tc, binary_mutation::trailing_bytes);
      },
      {.test_cases = malformed_binary_cases});
}

// Arbitrary bytes are either rejected with std::invalid_argument or decode
// into a self-consistent set (no crash, hang or foreign exception type).
BOOST_AUTO_TEST_CASE(DecodeArbitraryBytesIsRejectedOrConsistent) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto spec{tc.draw("spec", model::element_lists(3U))};
        auto bytes{model::to_model(spec).encode()};
        const auto flips{tc.draw(
            "flips", gs::vectors(gs::tuples(gs::integers<std::size_t>(),
                                            gs::integers<std::uint8_t>()),
                                 {.max_size = 4U}))};
        for (const auto &[position, value] : flips) {
          if (!bytes.empty()) {
            bytes[position % bytes.size()] = value;
          }
        }
        tc.note("bytes = " + hex_dump(bytes));
        gtids::gtid_set decoded;
        try {
          decoded = decode(bytes);
        } catch (const std::invalid_argument &) {
          return;
        } catch (const std::exception &e) {
          pbt::fail(std::string{"unexpected exception type: "} +
                    boost::core::demangle(typeid(e).name()) + " (" + e.what() +
                    ")");
        }
        require_self_consistent(decoded, "decoded from mutated binary");
      },
      {.test_cases = 20000U});
}

// ===========================================================================
// components: uuid / tag
// ===========================================================================

// uuid(u.str()) == u, str() is the canonical lower-case 8-4-4-4-12 form and
// the raw bytes survive the roundtrip.
BOOST_AUTO_TEST_CASE(UuidTextRoundTrip) {
  pbt::check([](hegel::TestCase &tc) {
    const auto raw{tc.draw("uuid", model::uuids())};
    const auto value{to_sut_uuid(raw)};
    pbt::require_equal(value.str(), model::uuid_to_string(raw),
                       "uuid::str() canonical form");
    pbt::require_equal(gtids::uuid{value.str()}, value, "uuid(str()) == uuid");
    pbt::require_equal(gtids::uuid{model::uuid_to_string(raw, true)}, value,
                       "upper-case spelling parses to the same uuid");
  });
}

// gtid(uuid, tag, gno) accepts exactly a non-nil uuid and gno in
// 1..2^63-1 (MySQL manual: "It is not possible for a transaction to have 0
// as a sequence number", upper limit 2^63-1).
BOOST_AUTO_TEST_CASE(GtidValidatesComponents) {
  pbt::check([](hegel::TestCase &tc) {
    const auto nil_uuid{tc.draw("nil_uuid", gs::booleans())};
    const auto raw{nil_uuid ? model::raw_uuid{}
                            : tc.draw("uuid", model::uuids())};
    const auto tag_name{tc.draw("tag", model::tags())};
    const auto gno{
        tc.draw("gno", gs::one_of<std::uint64_t>(
                           {gs::just(std::uint64_t{0U}), model::gnos(),
                            gs::integers<std::uint64_t>(
                                {.min_value = model::max_gno + 1ULL})}))};
    const auto uuid{to_sut_uuid(raw)};
    const auto tag{to_sut_tag(tag_name)};
    const bool valid{!nil_uuid && gno >= model::min_gno &&
                     gno <= model::max_gno};
    if (valid) {
      const gtids::gtid value{uuid, tag, gno};
      pbt::require_equal(value.get_gno(), gno, "gtid::get_gno()");
      pbt::require(!value.is_empty(), "valid gtid must not be empty");
    } else {
      pbt::require_throws<std::invalid_argument>(
          [&] { [[maybe_unused]] const gtids::gtid value{uuid, tag, gno}; },
          "invalid gtid components must be rejected");
      gtids::gtid_set set_value;
      pbt::require_throws<std::invalid_argument>(
          [&] { set_value.add(uuid, tag, gno); },
          "gtid_set::add() with invalid components must be rejected");
    }
  });
}

// tag(name) accepts exactly [a-zA-Z_][a-zA-Z0-9_]{0,31} (or the empty
// string for "untagged") and rejects everything else with
// std::invalid_argument.
BOOST_AUTO_TEST_CASE(TagValidationMatchesGrammar) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto name{tc.draw(
            "name", gs::one_of<std::string>(
                        {gs::from_regex("[a-zA-Z_][a-zA-Z0-9_]{0,40}", true),
                         gs::text({.max_size = 40U}),
                         gs::binary({.max_size = 40U}).map([](auto bytes) {
                           return std::string(std::begin(bytes),
                                              std::end(bytes));
                         })}))};
        const auto is_valid{[&name] {
          if (name.empty()) {
            return true;
          }
          if (name.size() > model::tag_max_length) {
            return false;
          }
          const auto is_alpha{[](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
          }};
          const auto is_digit{[](char ch) { return ch >= '0' && ch <= '9'; }};
          if (!is_alpha(name.front()) && name.front() != '_') {
            return false;
          }
          return std::ranges::all_of(name, [&](char ch) {
            return is_alpha(ch) || is_digit(ch) || ch == '_';
          });
        }()};
        if (is_valid) {
          gtids::tag value;
          try {
            value = gtids::tag{name};
          } catch (const std::exception &e) {
            pbt::fail("valid tag '" + name + "' rejected: " + e.what());
          }
          pbt::require_equal(value.get_name(), std::string_view{name},
                             "tag name roundtrip");
        } else {
          pbt::require_throws<std::invalid_argument>(
              [&] { [[maybe_unused]] const gtids::tag value{name}; },
              "invalid tag '" + name + "' must be rejected");
        }
      },
      {.test_cases = 20000U});
}

// ===========================================================================
// scale probe (not generated): very large sets must work in reasonable time
// ===========================================================================
BOOST_AUTO_TEST_CASE(ScaleProbeLargeSet) {
  // 200000 intervals by default; follows PBT_CASES_SCALE (--cases-scale)
  const std::uint64_t interval_count{pbt::scaled(200000ULL)};
  BOOST_TEST_MESSAGE("scale probe: " << interval_count << " intervals");
  const auto uuid{to_sut_uuid(
      model::raw_uuid{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
                      0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01})};
  gtids::gtid_set value;
  model::gtid_set expected;
  const model::tsid key{model::raw_uuid{0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                                        0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc,
                                        0xdd, 0xee, 0xff, 0x01},
                        ""};
  for (std::uint64_t i{0ULL}; i < interval_count; ++i) {
    value.add(uuid, gtids::tag{}, 2ULL * i + 1ULL);
    expected.add(key, 2ULL * i + 1ULL, 2ULL * i + 1ULL);
  }
  const auto text{value.str()};
  BOOST_CHECK_EQUAL(text, expected.str());
  BOOST_CHECK(gtids::gtid_set{text} == value);
  BOOST_CHECK(decode(encode(value)) == value);
  BOOST_CHECK_EQUAL(encode(value).size(), value.calculate_encoded_size());
  auto copy{value};
  copy.add_interval(uuid, gtids::tag{}, 1ULL, 2ULL * interval_count);
  BOOST_CHECK_EQUAL(copy.str(), model::uuid_to_string(key.uuid) + ":1-" +
                                    std::to_string(2ULL * interval_count));
  BOOST_CHECK((copy - value).str().size() > 0U);
  BOOST_CHECK((value - value).is_empty());
}
