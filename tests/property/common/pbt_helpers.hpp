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

#ifndef TESTS_PROPERTY_COMMON_PBT_HELPERS_HPP
#define TESTS_PROPERTY_COMMON_PBT_HELPERS_HPP

// Glue between Boost.Test (the project's unit test framework) and Hegel
// (the property-based testing engine).
//
// Every property is a regular BOOST_AUTO_TEST_CASE whose body calls
// pbt::check(). This keeps the property tests runnable exactly like the
// existing unit tests (individually via --run_test=<name>, via ctest, with
// Boost.Test loggers such as JUNIT), while Hegel drives input generation and
// shrinking.
//
// Run-time knobs (environment variables, all optional):
//   PBT_TEST_CASES      - override the number of generated cases for every
//                         property (default: the per-property value)
//   PBT_CASES_SCALE     - multiply the per-property default by this factor
//                         (e.g. 0.1 for a smoke run, 10 for a nightly run);
//                         also scales the input size of non-generated probe
//                         tests (see pbt::scaled())
//   PBT_SEED            - fixed seed (makes the run reproducible)
//   PBT_DERANDOMIZE     - "1" to use Hegel's deterministic mode
//   PBT_DATABASE        - directory of Hegel's example database (failing
//                         examples are replayed first in the next run);
//                         "off" disables it
//   PBT_REPORT_MULTIPLE - "1" to keep searching after the first failure and
//                         report all distinct failures of a property
//   PBT_VERBOSITY       - quiet | normal | verbose | debug
//   PBT_REPRODUCE       - a failure blob printed in a failure report
//                         ("rerun with: ... {"<blob>"}"); replays exactly that
//                         counterexample. Use together with --run_test=<name>.

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <optional>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

#include <boost/core/demangle.hpp>
#include <boost/test/unit_test.hpp>

#include <hegel/hegel.h>

namespace pbt {

namespace gs = hegel::generators;

// The exception thrown by every failed property assertion. Its origin
// ("file:line") lets Hegel tell distinct bugs apart when
// PBT_REPORT_MULTIPLE=1, and its message is what the test report and the
// known-bugs matcher (extra/qa/known_bugs.md) see.
class property_violation : public std::logic_error,
                           public hegel::FailureOrigin {
public:
  property_violation(const std::string &message,
                     const std::source_location &location)
      : std::logic_error{message},
        origin_{std::string{location.file_name()} + ":" +
                std::to_string(location.line())} {}

  [[nodiscard]] std::string failure_origin() const override { return origin_; }

private:
  std::string origin_;
};

[[noreturn]] inline void
fail(std::string_view message,
     const std::source_location &location = std::source_location::current()) {
  throw property_violation{
      std::string{"PROPERTY VIOLATED: "} + std::string{message}, location};
}

inline void require(
    bool condition, std::string_view message,
    const std::source_location &location = std::source_location::current()) {
  if (!condition) {
    fail(message, location);
  }
}

template <typename T> std::string to_display_string(const T &value) {
  std::ostringstream oss;
  if constexpr (requires(std::ostream &stream, const T &obj) {
                  stream << obj;
                }) {
    oss << value;
  } else {
    oss << "<" << boost::core::demangle(typeid(T).name()) << ">";
  }
  return oss.str();
}

template <typename T, typename U>
void require_equal(
    const T &actual, const U &expected, std::string_view message,
    const std::source_location &location = std::source_location::current()) {
  if (!(actual == expected)) {
    fail(std::string{message} + "\n    actual:   '" +
             to_display_string(actual) + "'\n    expected: '" +
             to_display_string(expected) + "'",
         location);
  }
}

// Expects 'func' to throw exactly 'Expected' (or a type derived from it).
// Returning normally or throwing any other exception type is a violation.
template <typename Expected, typename Func>
  requires std::derived_from<Expected, std::exception>
void require_throws(
    Func &&func, std::string_view message,
    const std::source_location &location = std::source_location::current()) {
  try {
    std::forward<Func>(func)();
  } catch (const property_violation &) {
    // a nested property failure is never the expected exception
    throw;
  } catch (const std::exception &e) {
    if constexpr (std::is_same_v<Expected, std::exception>) {
      return;
    } else {
      if (dynamic_cast<const Expected *>(&e) != nullptr) {
        return;
      }
    }
    fail(std::string{message} + "\n    expected exception: " +
             boost::core::demangle(typeid(Expected).name()) +
             "\n    actual exception:   " +
             boost::core::demangle(typeid(e).name()) + " (" + e.what() + ")",
         location);
  } catch (...) {
    fail(std::string{message} + "\n    expected exception: " +
             boost::core::demangle(typeid(Expected).name()) +
             "\n    actual exception:   <non-std exception>",
         location);
  }
  fail(std::string{message} + "\n    expected exception: " +
           boost::core::demangle(typeid(Expected).name()) +
           "\n    actual: no exception",
       location);
}

// The test cannot run because something outside the code under test is
// missing (e.g. no S3 endpoint). run_property_tests.py reports the marker
// as an infrastructure error, not as a property failure.
[[noreturn]] inline void infrastructure_error(std::string_view message) {
  throw std::runtime_error{"PBT_INFRA: " + std::string{message}};
}

namespace detail {

inline std::optional<std::string> env(const char *name) {
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *value{std::getenv(name)};
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::string{value};
}

inline hegel::Settings make_settings(std::uint64_t default_test_cases) {
  hegel::Settings settings{};

  std::uint64_t test_cases{default_test_cases};
  if (const auto scale{env("PBT_CASES_SCALE")}; scale) {
    const double factor{std::stod(*scale)};
    test_cases = static_cast<std::uint64_t>(
        std::max(1.0, std::round(static_cast<double>(test_cases) * factor)));
  }
  if (const auto value{env("PBT_TEST_CASES")}; value) {
    test_cases = std::stoull(*value);
  }
  settings.test_cases = test_cases;

  if (const auto seed{env("PBT_SEED")}; seed) {
    settings.seed = std::stoull(*seed);
  }
  if (const auto value{env("PBT_DERANDOMIZE")}; value) {
    settings.derandomize = (*value == "1" || *value == "true");
  }
  if (const auto value{env("PBT_REPORT_MULTIPLE")}; value) {
    settings.report_multiple_failures = (*value == "1" || *value == "true");
  }
  if (const auto value{env("PBT_DATABASE")}; value) {
    settings.database = (*value == "off") ? hegel::Database::disabled()
                                          : hegel::Database::from_path(*value);
  }
  if (const auto value{env("PBT_VERBOSITY")}; value) {
    if (*value == "quiet") {
      settings.verbosity = hegel::Verbosity::Quiet;
    } else if (*value == "verbose") {
      settings.verbosity = hegel::Verbosity::Verbose;
    } else if (*value == "debug") {
      settings.verbosity = hegel::Verbosity::Debug;
    }
  }
  return settings;
}

} // namespace detail

// Scales a size used by a non-generated test (e.g. the scale probe) with
// PBT_CASES_SCALE, so that smoke runs stay fast and nightly runs probe
// larger inputs. Never returns less than 1.
inline std::uint64_t scaled(std::uint64_t base) {
  if (const auto scale{detail::env("PBT_CASES_SCALE")}; scale) {
    return static_cast<std::uint64_t>(std::max(
        1.0, std::round(static_cast<double>(base) * std::stod(*scale))));
  }
  return base;
}

struct check_options {
  // default number of generated test cases for this property
  std::uint64_t test_cases{5000U};
};

// Runs a Hegel property inside the current Boost.Test test case. The Hegel
// failure report (minimal counterexample + reproduction blob) goes to
// stderr; the exception that falsified the property is rethrown so that
// Boost.Test marks the test case as failed with that message.
inline void
check(const std::function<void(hegel::TestCase &)> &body,
      check_options options = {},
      const std::source_location &location = std::source_location::current()) {
  const auto &boost_tc{boost::unit_test::framework::current_test_case()};
  const std::string test_name{boost_tc.p_name.get()};
  auto settings{detail::make_settings(options.test_cases)};
  settings.database_key =
      std::string{
          boost::unit_test::framework::master_test_suite().p_name.get()} +
      "::" + test_name;
  std::vector<std::string> failure_blobs;
  if (const auto blob{detail::env("PBT_REPRODUCE")}; blob) {
    failure_blobs.push_back(*blob);
  }
  hegel::test(body,
              hegel::TestLocation{test_name, location.file_name(),
                                  static_cast<int>(location.line())},
              settings, failure_blobs);
}

} // namespace pbt

#endif // TESTS_PROPERTY_COMMON_PBT_HELPERS_HPP
