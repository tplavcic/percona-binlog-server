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

// Property-based tests for the binlog_server command line operations
// (layer C of extra/qa/PBT_PLAN.md): the real binary is run on a storage
// generated in-process (through binsrv::storage, as the collector writes
// it), and its JSON output is compared with the README ("Operation modes"):
//  - list: every binlog in creation order with size, URI, timestamps and
//    GTIDs; an empty storage gives "success" with an empty result;
//  - search_by_timestamp: the binlogs that have at least one event with a
//    timestamp <= the given one; errors for an empty storage / a timestamp
//    older than every event;
//  - search_by_gtid_set: the minimal set of binlogs covering the GTID set;
//    an error when it cannot be covered or the storage is position-based;
//  - purge_binlogs: removes the binlogs up to the given one and reports
//    them; list shows the rest;
//  - malformed arguments give "status": "error" (and a failure exit code),
//    never a crash.
//
// The binary is the binlog_server built next to this test
// (PBS_BINLOG_SERVER_PATH); PBT_BINLOG_SERVER overrides it.

#define BOOST_TEST_MODULE CliPropertyTests

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include "binsrv/main_config.hpp"
#include "binsrv/null_logger.hpp"
#include "binsrv/storage.hpp"
#include "binsrv/storage_core.hpp"

#include "binsrv/gtids/gtid.hpp"
#include "binsrv/gtids/gtid_set.hpp"
#include "binsrv/gtids/tag.hpp"
#include "binsrv/gtids/uuid.hpp"

#include "binsrv/events/composite_binlog_name.hpp"

#include "util/byte_span_fwd.hpp"
#include "util/ctime_timestamp.hpp"

#include "common/binlog_server_process.hpp"
#include "common/pbt_helpers.hpp"

namespace {

namespace gs = hegel::generators;
namespace fs = std::filesystem;
namespace json = boost::json;
namespace gtids = binsrv::gtids;

// ---------------------------------------------------------------------------
// running the binary
// ---------------------------------------------------------------------------
using pbt::process::run;
using pbt::process::run_result;
using pbt::process::temporary_directory;

// the JSON response; the process must not crash
json::object response_of(hegel::TestCase &tc, const run_result &result) {
  tc.note("output: " + result.output);
  pbt::require(!result.signaled, "binlog_server crashed with signal " +
                                     std::to_string(result.signal_number));
  json::value parsed;
  try {
    parsed = json::parse(result.output);
  } catch (const std::exception &e) {
    pbt::fail(std::string{"output is not one JSON document: "} + e.what());
  }
  pbt::require(parsed.is_object(), "output is a JSON object");
  const auto &object{parsed.as_object()};
  pbt::require(object.contains("status") && object.at("status").is_string(),
               "response has a \"status\"");
  const auto status{object.at("status").as_string()};
  pbt::require_equal(result.exit_code == 0, status == "success",
                     "exit code 0 exactly for \"status\": \"success\"");
  return object;
}

std::string response_message(const json::object &response) {
  const auto *message{response.if_contains("message")};
  return message != nullptr && message->is_string()
             ? std::string{message->as_string()}
             : std::string{};
}

void require_error(const json::object &response, std::string_view message) {
  pbt::require_equal(
      std::string{response.at("status").as_string()}, std::string{"error"},
      message.empty()
          ? std::string{"malformed argument accepted (status must be error)"}
          : "status must be error with message '" + std::string{message} +
                "'");
  if (!message.empty()) {
    pbt::require_equal(response_message(response), std::string{message},
                       "error message");
  }
}

std::vector<std::string> result_names(const json::object &response) {
  pbt::require_equal(std::string{response.at("status").as_string()},
                     std::string{"success"},
                     "status must be success (message: '" +
                         response_message(response) + "')");
  std::vector<std::string> names;
  for (const auto &item : response.at("result").as_array()) {
    names.emplace_back(item.as_object().at("name").as_string());
  }
  return names;
}

// ---------------------------------------------------------------------------
// generated storage
// ---------------------------------------------------------------------------
struct stored_transaction {
  gtids::gtid gtid;
  std::time_t timestamp{};
  std::size_t size{};
};

struct stored_file {
  std::string name;
  std::vector<stored_transaction> transactions;
  [[nodiscard]] std::uint64_t size() const {
    std::uint64_t result{4U};
    for (const auto &trx : transactions) {
      result += trx.size;
    }
    return result;
  }
  [[nodiscard]] gtids::gtid_set added() const {
    gtids::gtid_set result;
    for (const auto &trx : transactions) {
      if (!trx.gtid.is_empty()) {
        result += trx.gtid;
      }
    }
    return result;
  }
};

struct generated_storage {
  temporary_directory directory{"pbs-pbt-cli"};
  bool gtid_mode{};
  gtids::gtid_set purged;
  std::vector<stored_file> files;
  std::string config_path;

  [[nodiscard]] fs::path vault() const { return directory.path() / "vault"; }
  [[nodiscard]] gtids::gtid_set all_gtids() const {
    auto result{purged};
    for (const auto &file : files) {
      result += file.added();
    }
    return result;
  }
};

gtids::uuid uuid_of(std::uint8_t value) {
  gtids::uuid_storage storage{};
  for (auto &item : storage) {
    item = std::byte{value};
  }
  return gtids::uuid{storage};
}

std::string iso(std::time_t value) {
  std::tm parts{};
  ::gmtime_r(&value, &parts);
  std::array<char, 32> text{};
  const auto length{
      std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%S", &parts)};
  return {text.data(), length};
}

void write_config(generated_storage &storage) {
  const auto config_path{storage.directory.path() / "config.json"};
  std::ofstream output{config_path};
  output << R"({
  "logger": {"level": "error", "file": ")"
         << (storage.directory.path() / "binsrv.log").generic_string() << R"("},
  "connection": {"host": "127.0.0.1", "port": 3306, "user": "u",
                 "password": "", "connect_timeout": 1, "read_timeout": 1,
                 "write_timeout": 1},
  "replication": {"server_id": 1, "idle_time": 1, "verify_checksum": true,
                  "mode": ")"
         << (storage.gtid_mode ? "gtid" : "position") << R"("},
  "replication_source": {"port": 3307, "read_timeout": 1, "write_timeout": 1,
                         "authentication": {"user": "u", "password": "p",
                                            "plugin": "caching_sha2_password"}},
  "storage": {"backend": "file", "uri": "file://)"
         << storage.vault().generic_string() << R"("}
}
)";
  storage.config_path = config_path.string();
}

// 0..4 binlogs (some without any transaction), GTIDs unique across the
// storage, event timestamps that never go back from one binlog to the next
// (the first event of a binlog is its earliest)
std::unique_ptr<generated_storage> generate(hegel::TestCase &tc,
                                            std::optional<bool> gtid_mode = {}) {
  auto result{std::make_unique<generated_storage>()};
  result->gtid_mode = gtid_mode.value_or(tc.draw("gtid_mode", gs::booleans()));
  fs::create_directories(result->vault());
  write_config(*result);
  const binsrv::main_config config{result->config_path};
  binsrv::storage storage{std::make_shared<binsrv::null_logger>(), config,
                          binsrv::storage_construction_mode_type::streaming};

  std::array<std::uint64_t, 2> next_gno{1U, 1U};
  if (result->gtid_mode && tc.draw("has_purged", gs::booleans())) {
    const auto purged_up_to{tc.draw(
        "purged_up_to", gs::integers<std::uint64_t>({.min_value = 1U,
                                                     .max_value = 5U}))};
    result->purged.add_interval(uuid_of(0x11), gtids::tag{}, 1U, purged_up_to);
    next_gno[0] = purged_up_to + 1U;
    storage.set_purged_gtids(result->purged);
  }
  const auto number_of_files{
      tc.draw("files", gs::integers<std::size_t>({.max_value = 4U}))};
  const auto first_number{tc.draw(
      "first_binlog_number",
      gs::integers<std::uint32_t>({.min_value = 1U, .max_value = 50U}))};
  std::time_t base{tc.draw(
      "first_timestamp",
      gs::integers<std::time_t>({.min_value = 1000000000, .max_value = 2000000000}))};
  for (std::size_t i{0U}; i < number_of_files; ++i) {
    const binsrv::events::composite_binlog_name name{
        "binlog", static_cast<std::uint32_t>(first_number + i)};
    stored_file file{name.str(), {}};
    std::ignore = storage.open_binlog(name);
    const auto transactions{tc.draw(
        "transactions", gs::integers<std::size_t>({.max_value = 4U}))};
    base += tc.draw("gap", gs::integers<std::time_t>({.min_value = 0, .max_value = 100}));
    for (std::size_t t{0U}; t < transactions; ++t) {
      stored_transaction trx;
      if (result->gtid_mode) {
        const auto source{
            tc.draw("source", gs::integers<std::size_t>({.max_value = 1U}))};
        trx.gtid = gtids::gtid{uuid_of(source == 0U ? 0x11 : 0x22),
                               gtids::tag{source == 0U ? "" : "t1"},
                               next_gno[source]++};
      }
      trx.timestamp =
          t == 0U ? base
                  : base + tc.draw("offset", gs::integers<std::time_t>(
                                                 {.min_value = 0, .max_value = 60}));
      trx.size = tc.draw("size", gs::integers<std::size_t>({.min_value = 1U,
                                                           .max_value = 40U}));
      const std::vector<std::byte> data(trx.size, std::byte{0x42});
      storage.write_event(util::const_byte_span{data}, true, trx.gtid,
                          util::ctime_timestamp{trx.timestamp}, t + 1U);
      file.transactions.push_back(trx);
    }
    storage.close_binlog();
    result->files.push_back(std::move(file));
  }
  std::string listing;
  for (const auto &file : result->files) {
    listing += " " + file.name + "(" + std::to_string(file.transactions.size()) +
               " trx, " + file.added().str() + ")";
  }
  tc.note("storage:" + listing);
  return result;
}

// strict "YYYY-MM-DDTHH:MM:SS" with calendar / clock range checks
bool is_valid_iso_timestamp(std::string_view value) {
  if (value.size() != 19U || value[4] != '-' || value[7] != '-' ||
      value[10] != 'T' || value[13] != ':' || value[16] != ':') {
    return false;
  }
  const auto number{[value](std::size_t from, std::size_t length) {
    int result{0};
    for (std::size_t i{from}; i < from + length; ++i) {
      if (value[i] < '0' || value[i] > '9') {
        return -1;
      }
      result = result * 10 + (value[i] - '0');
    }
    return result;
  }};
  const auto year{number(0U, 4U)};
  const auto month{number(5U, 2U)};
  const auto day{number(8U, 2U)};
  const auto hour{number(11U, 2U)};
  const auto minute{number(14U, 2U)};
  const auto second{number(17U, 2U)};
  if (year < 1970 || month < 1 || month > 12 || day < 1 || hour < 0 ||
      hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
    return false;
  }
  constexpr std::array<int, 12> days{31, 28, 31, 30, 31, 30,
                                     31, 31, 30, 31, 30, 31};
  const bool leap{(year % 4 == 0 && year % 100 != 0) || year % 400 == 0};
  const auto last_day{days[static_cast<std::size_t>(month - 1)] +
                      (month == 2 && leap ? 1 : 0)};
  return day <= last_day;
}

std::vector<std::string> names_of(const std::vector<const stored_file *> &files) {
  std::vector<std::string> result;
  for (const auto *file : files) {
    result.push_back(file->name);
  }
  return result;
}

std::string describe(const std::vector<std::string> &names) {
  std::string result{"["};
  for (const auto &name : names) {
    result += (result.size() > 1U ? ", " : "") + name;
  }
  return result + "]";
}

} // namespace

// ===========================================================================
// list
// ===========================================================================

// "list" prints every binlog in creation order with its size, URI,
// min / max event timestamps and (GTID mode) previous / added GTIDs; an
// empty storage gives "success" with an empty "result".
BOOST_AUTO_TEST_CASE(ListReportsEveryBinlog) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto storage{generate(tc)};
        const auto response{
            response_of(tc, run({"list", storage->config_path}))};
        const auto names{result_names(response)};
        std::vector<std::string> expected;
        for (const auto &file : storage->files) {
          expected.push_back(file.name);
        }
        pbt::require_equal(describe(names), describe(expected),
                           "binlogs in creation order");
        gtids::gtid_set previous{storage->purged};
        const auto &items{response.at("result").as_array()};
        for (std::size_t i{0U}; i < items.size(); ++i) {
          const auto &item{items[i].as_object()};
          const auto &file{storage->files[i]};
          pbt::require_equal(item.at("size").to_number<std::uint64_t>(),
                             file.size(), "size");
          pbt::require_equal(
              std::string{item.at("uri").as_string()},
              "file://" + (storage->vault() / file.name).generic_string(),
              "uri");
          if (!file.transactions.empty()) {
            std::time_t min_ts{file.transactions.front().timestamp};
            std::time_t max_ts{min_ts};
            for (const auto &trx : file.transactions) {
              min_ts = std::min(min_ts, trx.timestamp);
              max_ts = std::max(max_ts, trx.timestamp);
            }
            pbt::require_equal(std::string{item.at("min_timestamp").as_string()},
                               iso(min_ts), "min_timestamp");
            pbt::require_equal(std::string{item.at("max_timestamp").as_string()},
                               iso(max_ts), "max_timestamp");
          }
          if (storage->gtid_mode) {
            pbt::require_equal(
                gtids::gtid_set{item.at("previous_gtids").as_string()},
                previous, "previous_gtids");
            pbt::require_equal(gtids::gtid_set{item.at("added_gtids").as_string()},
                               file.added(), "added_gtids");
          }
          previous += file.added();
        }
      },
      {.test_cases = 150U, .slow = true});
}

// ===========================================================================
// search_by_timestamp
// ===========================================================================

// README: "the list of binlog files ... that have at least one event whose
// timestamp is less or equal to the provided <iso_timestamp>"; "Binlog
// storage is empty" for an empty storage, "Timestamp is too old" when no
// binlog qualifies. Timestamps are drawn around every event timestamp.
BOOST_AUTO_TEST_CASE(SearchByTimestampFollowsReadme) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto storage{generate(tc)};
        std::vector<std::time_t> interesting{1000000000};
        for (const auto &file : storage->files) {
          for (const auto &trx : file.transactions) {
            interesting.push_back(trx.timestamp);
          }
        }
        const auto anchor{tc.draw("anchor", gs::sampled_from(interesting))};
        const auto query{anchor + tc.draw("delta", gs::sampled_from<std::time_t>(
                                                       {-1, 0, 0, 1, 1000}))};
        tc.note("query = " + iso(query));
        const auto response{response_of(
            tc, run({"search_by_timestamp", storage->config_path, iso(query)}))};
        if (storage->files.empty()) {
          require_error(response, "Binlog storage is empty");
          return;
        }
        std::vector<const stored_file *> expected;
        for (const auto &file : storage->files) {
          if (std::ranges::any_of(file.transactions,
                                  [query](const stored_transaction &trx) {
                                    return trx.timestamp <= query;
                                  })) {
            expected.push_back(&file);
          }
        }
        if (expected.empty()) {
          require_error(response, "Timestamp is too old");
          return;
        }
        pbt::require_equal(describe(result_names(response)),
                           describe(names_of(expected)),
                           "binlogs with an event at or before the timestamp");
      },
      {.test_cases = 200U, .slow = true});
}

// ===========================================================================
// search_by_gtid_set
// ===========================================================================

// README: "the minimal set of binlog files ... required to cover the
// specified GTID set" (in storage order); "The specified GTID set cannot be
// covered" when a GTID is not in any binlog (also purged ones); errors for
// an empty storage and for a position-based storage.
BOOST_AUTO_TEST_CASE(SearchByGtidSetFollowsReadme) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto storage{generate(tc)};
        // a query made of stored, purged and never seen GTIDs
        gtids::gtid_set query;
        const auto stored{storage->all_gtids()};
        const auto picks{tc.draw("picks", gs::integers<std::size_t>(
                                              {.min_value = 1U, .max_value = 4U}))};
        for (std::size_t i{0U}; i < picks; ++i) {
          const auto source{
              tc.draw("query_source", gs::integers<std::size_t>({.max_value = 1U}))};
          const auto gno{tc.draw("query_gno", gs::integers<std::uint64_t>(
                                                  {.min_value = 1U,
                                                   .max_value = 25U}))};
          query += gtids::gtid{uuid_of(source == 0U ? 0x11 : 0x22),
                               gtids::tag{source == 0U ? "" : "t1"}, gno};
        }
        tc.note("query = " + query.str());
        const auto response{response_of(
            tc, run({"search_by_gtid_set", storage->config_path, query.str()}))};
        if (storage->files.empty()) {
          require_error(response, "Binlog storage is empty");
          return;
        }
        if (!storage->gtid_mode) {
          require_error(response,
                        "GTID set search is not supported in storages created "
                        "in position-based replication mode");
          return;
        }
        gtids::gtid_set in_binlogs;
        std::vector<const stored_file *> expected;
        for (const auto &file : storage->files) {
          const auto added{file.added()};
          in_binlogs += added;
          if (gtids::intersects(added, query)) {
            expected.push_back(&file);
          }
        }
        auto uncovered{query};
        uncovered.subtract(in_binlogs);
        if (!uncovered.is_empty()) {
          require_error(response, "The specified GTID set cannot be covered");
          return;
        }
        pbt::require_equal(describe(result_names(response)),
                           describe(names_of(expected)),
                           "the binlogs that hold a GTID of the query");
      },
      {.test_cases = 200U, .slow = true});
}

// ===========================================================================
// purge_binlogs
// ===========================================================================

// README: purge_binlogs <binlog_name> removes the binlogs up to and
// including it and reports them (oldest first); the most recent binlog
// cannot be purged; afterwards list shows exactly the remaining binlogs.
BOOST_AUTO_TEST_CASE(PurgeBinlogsFollowsReadme) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto storage{generate(tc)};
        tc.assume(!storage->files.empty());
        const auto index{tc.draw(
            "target", gs::integers<std::size_t>(
                          {.max_value = storage->files.size() - 1U}))};
        const auto &target{storage->files[index].name};
        const auto response{response_of(
            tc, run({"purge_binlogs", storage->config_path, target}))};
        std::vector<std::string> remaining;
        if (index + 1U == storage->files.size()) {
          require_error(response,
                        "cannot purge: target is the current tail binlog "
                        "file; at least one binlog file must remain in the "
                        "storage to preserve the resume position");
          for (const auto &file : storage->files) {
            remaining.push_back(file.name);
          }
        } else {
          std::vector<std::string> purged;
          for (std::size_t i{0U}; i < storage->files.size(); ++i) {
            (i <= index ? purged : remaining).push_back(storage->files[i].name);
          }
          pbt::require_equal(describe(result_names(response)), describe(purged),
                             "purged binlogs, oldest first");
        }
        const auto listed{
            response_of(tc, run({"list", storage->config_path}))};
        pbt::require_equal(describe(result_names(listed)), describe(remaining),
                           "list after purge_binlogs");
      },
      {.test_cases = 150U, .slow = true});
}

// ===========================================================================
// malformed arguments
// ===========================================================================

// Malformed timestamps, GTID sets and binlog names are reported as
// {"status": "error", ...} with a failure exit code - never a crash, never
// a "success". (README: <timestamp> "a valid timestamp in ISO format",
// <gtid_set> "a valid gtid set", <binlog_name> "a valid binlog file name
// without path".)
BOOST_AUTO_TEST_CASE(MalformedArgumentsAreRejected) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto storage{generate(tc, true)};
        const auto operation{tc.draw(
            "operation", gs::sampled_from<std::string>(
                             {"search_by_timestamp", "search_by_gtid_set",
                              "purge_binlogs"}))};
        std::string argument;
        if (operation == "search_by_timestamp") {
          argument = tc.draw(
              "timestamp",
              gs::one_of<std::string>(
                  {gs::sampled_from<std::string>(
                       {"", "now", "2026-02-30T00:00:00", "2026-13-01T00:00:00",
                        "2026-02-10T25:00:00", "2026-02-10T14:60:00",
                        "2026-02-10", "14:30:00", "2026-02-10T14:30:00Z+",
                        "-1", "1770733800", "2026/02/10T14:30:00"}),
                   gs::from_regex("[0-9T:\\-]{1,25}", true).filter(
                       [](const std::string &value) {
                         // keep only strings that are not valid ISO
                         // timestamps (independent oracle, not PBS's parser)
                         return !is_valid_iso_timestamp(value);
                       })}));
        } else if (operation == "search_by_gtid_set") {
          argument = tc.draw(
              "gtid_set",
              gs::sampled_from<std::string>(
                  {"not-a-gtid", "11111111-1111-1111-1111-111111111111",
                   "11111111-1111-1111-1111-111111111111:",
                   "11111111-1111-1111-1111-111111111111:0",
                   "11111111-1111-1111-1111-111111111111:5-3",
                   "11111111-1111-1111-1111-11111111111:1",
                   "11111111-1111-1111-1111-111111111111:TAG!:1",
                   "11111111-1111-1111-1111-111111111111::1",
                   ":1-2", "11111111-1111-1111-1111-111111111111:1,"}));
        } else {
          argument = tc.draw(
              "binlog_name",
              gs::sampled_from<std::string>(
                  {"", "binlog", "binlog.", "binlog.1", "binlog.00001",
                   "binlog.0000001", ".000001", "dir/binlog.000001",
                   "binlog.000000", "binlog.abcdef", "binlog.-00001"}));
        }
        tc.note(operation + " '" + argument + "'");
        const auto response{response_of(
            tc, run({operation, storage->config_path, argument}))};
        require_error(response, "");
      },
      {.test_cases = 200U, .slow = true});
}
