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

// Property-based tests against a real MySQL server (layer D of
// extra/qa/PBT_PLAN.md). Every test case resets the server's binary logs,
// runs a random workload (row changes, multi-statement and rolled back
// transactions, large rows, DDL, FLUSH BINARY LOGS, small max_binlog_size,
// compressed transactions, tagged GTIDs on 8.3+), runs the real
// binlog_server binary ('fetch' / 'pull', killed and restarted) with a
// random configuration and compares the storage with the server:
//  - the stored binlogs are byte-identical to the server's binlog files
//    (the server's open binlog has LOG_EVENT_BINLOG_IN_USE_F set in its
//    FORMAT_DESCRIPTION, which the dump thread clears - as in the MTR test
//    binlog_streaming.binsrv);
//  - 'list' reports the server's binlogs, sizes and (GTID mode) per-file
//    previous / added GTIDs (from SHOW BINLOG EVENTS and gtid_executed);
//  - 'search_by_gtid_set' returns the binlogs holding the GTIDs;
//  - rewrite mode keeps every transaction, in order, in well-formed files.
//
// The server: PBT_MYSQL_HOST / _PORT / _USER / _PASSWORD (set by
// run_property_tests.py, which starts a private server with
// extra/qa/mysqld.sh). It must run with log_bin, gtid_mode=ON and
// binlog_checksum=CRC32, and its binlog files must be readable by this
// process. Its binary logs and GTIDs are reset - never point these tests
// at a server whose data matters.

#define BOOST_TEST_MODULE MySqlPropertyTests

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <unistd.h>

#include <mysql/mysql.h>

#include <zlib.h>

#include <boost/test/unit_test.hpp>

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include "binsrv/gtids/gtid_set.hpp"

#include "common/binlog_server_process.hpp"
#include "common/pbt_helpers.hpp"

namespace {

namespace gs = hegel::generators;
namespace fs = std::filesystem;
namespace json = boost::json;
namespace gtids = binsrv::gtids;

using pbt::process::background_run;
using pbt::process::run;
using pbt::process::run_result;
using pbt::process::temporary_directory;

// ===========================================================================
// MySQL connection
// ===========================================================================
struct server_settings {
  std::string host;
  unsigned port{};
  std::string user;
  std::string password;
};

server_settings settings() {
  const auto host{pbt::detail::env("PBT_MYSQL_HOST")};
  const auto port{pbt::detail::env("PBT_MYSQL_PORT")};
  const auto user{pbt::detail::env("PBT_MYSQL_USER")};
  if (!host || !port || !user) {
    pbt::infrastructure_error(
        "MySQL server not configured: set PBT_MYSQL_HOST / _PORT / _USER / "
        "_PASSWORD (eval \"$(extra/qa/mysqld.sh env)\" after "
        "extra/qa/mysqld.sh start; run_property_tests.py does this)");
  }
  return {*host, static_cast<unsigned>(std::stoul(*port)), *user,
          pbt::detail::env("PBT_MYSQL_PASSWORD").value_or("")};
}

using row = std::vector<std::optional<std::string>>;

class sql_connection {
public:
  sql_connection() : handle_{::mysql_init(nullptr)} {
    if (handle_ == nullptr) {
      throw std::runtime_error{"mysql_init failed"};
    }
    const auto server{settings()};
    const unsigned timeout{10U};
    ::mysql_options(handle_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    if (::mysql_real_connect(handle_, server.host.c_str(), server.user.c_str(),
                             server.password.c_str(), nullptr, server.port,
                             nullptr, 0) == nullptr) {
      const std::string message{::mysql_error(handle_)};
      ::mysql_close(handle_);
      handle_ = nullptr;
      pbt::infrastructure_error("cannot connect to MySQL at " + server.host +
                                ":" + std::to_string(server.port) + ": " +
                                message);
    }
  }
  sql_connection(const sql_connection &) = delete;
  sql_connection &operator=(const sql_connection &) = delete;
  sql_connection(sql_connection &&) = delete;
  sql_connection &operator=(sql_connection &&) = delete;
  ~sql_connection() {
    if (handle_ != nullptr) {
      ::mysql_close(handle_);
    }
  }

  std::vector<row> query(std::string_view statement) {
    if (::mysql_real_query(handle_, statement.data(),
                           statement.size()) != 0) {
      throw std::runtime_error{"SQL error: " + std::string{statement} + ": " +
                               ::mysql_error(handle_)};
    }
    std::vector<row> rows;
    MYSQL_RES *result{::mysql_store_result(handle_)};
    if (result == nullptr) {
      if (::mysql_field_count(handle_) != 0U) {
        throw std::runtime_error{"SQL error (result): " +
                                 std::string{statement} + ": " +
                                 ::mysql_error(handle_)};
      }
      return rows;
    }
    const auto fields{::mysql_num_fields(result)};
    while (MYSQL_ROW values = ::mysql_fetch_row(result)) {
      const auto *lengths{::mysql_fetch_lengths(result)};
      row current;
      for (unsigned i{0U}; i < fields; ++i) {
        if (values[i] == nullptr) {
          current.emplace_back(std::nullopt);
        } else {
          current.emplace_back(std::string{values[i], lengths[i]});
        }
      }
      rows.push_back(std::move(current));
    }
    ::mysql_free_result(result);
    return rows;
  }

  void exec(std::string_view statement) { std::ignore = query(statement); }

  std::string scalar(std::string_view statement) {
    const auto rows{query(statement)};
    if (rows.empty() || rows.front().empty() || !rows.front().front()) {
      return {};
    }
    return *rows.front().front();
  }

private:
  MYSQL *handle_{nullptr};
};

struct server_info {
  int major{};
  int minor{};
  std::string version;
  fs::path binlog_dir;

  [[nodiscard]] bool at_least(int ma, int mi) const {
    return major > ma || (major == ma && minor >= mi);
  }
  [[nodiscard]] bool has_tagged_gtids() const { return at_least(8, 3); }
  [[nodiscard]] std::string reset_statement() const {
    return at_least(8, 2) ? "RESET BINARY LOGS AND GTIDS" : "RESET MASTER";
  }
};

server_info inspect(sql_connection &sql) {
  server_info info;
  info.version = sql.scalar("SELECT @@GLOBAL.version");
  std::istringstream parts{info.version};
  char dot{};
  parts >> info.major >> dot >> info.minor;
  if (sql.scalar("SELECT @@GLOBAL.log_bin") != "1" ||
      sql.scalar("SELECT @@GLOBAL.gtid_mode") != "ON" ||
      sql.scalar("SELECT @@GLOBAL.binlog_checksum") != "CRC32") {
    pbt::infrastructure_error("the MySQL server must run with log_bin, "
                              "gtid_mode=ON and binlog_checksum=CRC32 "
                              "(extra/qa/mysqld.sh)");
  }
  info.binlog_dir =
      fs::path{sql.scalar("SELECT @@GLOBAL.log_bin_basename")}.parent_path();
  std::error_code error;
  if (!fs::is_directory(info.binlog_dir, error)) {
    pbt::infrastructure_error("the server's binlog directory " +
                              info.binlog_dir.string() +
                              " is not readable by the test (the server "
                              "must run on this host)");
  }
  return info;
}

// a fresh binary log: no dump threads, the test schema, binlog.000001 only
void reset_server(sql_connection &sql, const server_info &info,
                  std::uint64_t max_binlog_size) {
  for (const auto &dump : sql.query(
           "SELECT id FROM information_schema.processlist "
           "WHERE command LIKE 'Binlog Dump%'")) {
    try {
      sql.exec("KILL " + dump.at(0).value_or("0"));
    } catch (const std::exception &) {
      // already gone
    }
  }
  sql.exec("DROP DATABASE IF EXISTS pbt");
  sql.exec("CREATE DATABASE pbt");
  sql.exec("CREATE TABLE pbt.t1 (id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, "
           "v INT NOT NULL, b LONGBLOB) ENGINE=InnoDB");
  sql.exec("SET GLOBAL max_binlog_size = " + std::to_string(max_binlog_size));
  sql.exec(info.reset_statement());
}

// ===========================================================================
// workload
// ===========================================================================
enum class op_kind : std::uint8_t {
  insert_rows,
  transaction,
  rollback,
  large_row,
  ddl,
  flush,
  compressed,
  tagged,
  update_rows,
  delete_rows
};

struct operation {
  op_kind kind{};
  int count{};
  std::size_t size{};
  std::string tag;
};

std::string describe(const operation &op) {
  switch (op.kind) {
  case op_kind::insert_rows:
    return "insert " + std::to_string(op.count) + " row(s)";
  case op_kind::transaction:
    return "transaction of " + std::to_string(op.count) + " statement(s)";
  case op_kind::rollback:
    return "rolled back transaction";
  case op_kind::large_row:
    return "insert a " + std::to_string(op.size) + " byte row";
  case op_kind::ddl:
    return op.count == 0 ? "CREATE TABLE" : "DROP TABLE";
  case op_kind::flush:
    return "FLUSH BINARY LOGS";
  case op_kind::compressed:
    return "compressed transaction of " + std::to_string(op.count) + " row(s)";
  case op_kind::tagged:
    return "transaction tagged '" + op.tag + "'";
  case op_kind::update_rows:
    return "update rows with id % 3 = " + std::to_string(op.count);
  case op_kind::delete_rows:
    return "delete " + std::to_string(op.count) + " row(s)";
  }
  return "?";
}

std::vector<operation> draw_workload(hegel::TestCase &tc,
                                     const server_info &info,
                                     std::size_t max_operations) {
  std::vector<op_kind> kinds{
      op_kind::insert_rows, op_kind::transaction, op_kind::rollback,
      op_kind::large_row,   op_kind::ddl,         op_kind::flush,
      op_kind::compressed,  op_kind::update_rows, op_kind::delete_rows};
  if (info.has_tagged_gtids()) {
    kinds.push_back(op_kind::tagged);
  }
  const auto count{tc.draw(
      "operations", gs::integers<std::size_t>({.max_value = max_operations}))};
  std::vector<operation> result;
  for (std::size_t i{0U}; i < count; ++i) {
    operation op;
    op.kind = tc.draw("kind", gs::sampled_from(kinds));
    switch (op.kind) {
    case op_kind::insert_rows:
    case op_kind::compressed:
      op.count = tc.draw("rows", gs::integers<int>({.min_value = 1,
                                                    .max_value = 5}));
      break;
    case op_kind::transaction:
      op.count = tc.draw("statements", gs::integers<int>({.min_value = 1,
                                                          .max_value = 6}));
      break;
    case op_kind::large_row:
      op.size = tc.draw("row_size", gs::sampled_from<std::size_t>(
                                        {1000U, 5000U, 70000U, 300000U}));
      break;
    case op_kind::ddl:
    case op_kind::update_rows:
      op.count = tc.draw("variant", gs::integers<int>({.min_value = 0,
                                                       .max_value = 2}));
      break;
    case op_kind::delete_rows:
      op.count = tc.draw("rows", gs::integers<int>({.min_value = 1,
                                                    .max_value = 3}));
      break;
    case op_kind::tagged:
      // MySQL lower-cases tags (PBT-004 is about PBS's own parser)
      op.tag = tc.draw("tag", gs::sampled_from<std::string>(
                                  {"alpha", "b_2", "Upper", "t"}));
      break;
    default:
      break;
    }
    result.push_back(std::move(op));
  }
  return result;
}

void apply(sql_connection &sql, const operation &op) {
  const auto insert_values{[](int rows) {
    std::string statement{"INSERT INTO pbt.t1 (v, b) VALUES "};
    for (int i{0}; i < rows; ++i) {
      statement += (i == 0 ? "" : ", ") + std::string{"("} +
                   std::to_string(i) + ", REPEAT('r', " +
                   std::to_string(10 * (i + 1)) + "))";
    }
    return statement;
  }};
  switch (op.kind) {
  case op_kind::insert_rows:
    sql.exec(insert_values(op.count));
    break;
  case op_kind::transaction:
    sql.exec("BEGIN");
    for (int i{0}; i < op.count; ++i) {
      switch (i % 3) {
      case 0:
        sql.exec(insert_values(1 + (i % 2)));
        break;
      case 1:
        sql.exec("UPDATE pbt.t1 SET v = v + 1 ORDER BY id DESC LIMIT 2");
        break;
      default:
        sql.exec("DELETE FROM pbt.t1 ORDER BY id LIMIT 1");
        break;
      }
    }
    sql.exec("COMMIT");
    break;
  case op_kind::rollback:
    sql.exec("BEGIN");
    sql.exec(insert_values(2));
    sql.exec("ROLLBACK");
    break;
  case op_kind::large_row:
    sql.exec("INSERT INTO pbt.t1 (v, b) VALUES (7, REPEAT('L', " +
             std::to_string(op.size) + "))");
    break;
  case op_kind::ddl:
    if (op.count == 0) {
      sql.exec("CREATE TABLE IF NOT EXISTS pbt.t2 (id INT PRIMARY KEY, "
               "c VARCHAR(32)) ENGINE=InnoDB");
    } else {
      sql.exec("DROP TABLE IF EXISTS pbt.t2");
    }
    break;
  case op_kind::flush:
    sql.exec("FLUSH BINARY LOGS");
    break;
  case op_kind::compressed:
    sql.exec("SET SESSION binlog_transaction_compression = ON");
    sql.exec(insert_values(op.count));
    sql.exec("SET SESSION binlog_transaction_compression = OFF");
    break;
  case op_kind::tagged:
    sql.exec("SET SESSION gtid_next = 'AUTOMATIC:" + op.tag + "'");
    sql.exec(insert_values(1));
    sql.exec("SET SESSION gtid_next = 'AUTOMATIC'");
    break;
  case op_kind::update_rows:
    sql.exec("UPDATE pbt.t1 SET v = v * 2 + 1 WHERE id % 3 = " +
             std::to_string(op.count));
    break;
  case op_kind::delete_rows:
    sql.exec("DELETE FROM pbt.t1 ORDER BY id DESC LIMIT " +
             std::to_string(op.count));
    break;
  }
}

void apply_all(hegel::TestCase &tc, sql_connection &sql,
               const std::vector<operation> &ops) {
  for (const auto &op : ops) {
    tc.note("  " + describe(op));
    apply(sql, op);
  }
}

// ===========================================================================
// binlog_server configuration
// ===========================================================================
struct pbs_options {
  bool gtid_mode{};
  bool verify_checksum{true};
  std::optional<std::string> checkpoint_size;
  std::optional<std::string> checkpoint_interval;
  std::optional<std::string> rewrite_file_size;
};

std::string describe(const pbs_options &options) {
  std::string result{options.gtid_mode ? "gtid" : "position"};
  result += options.verify_checksum ? ", verify_checksum" : ", no checksum check";
  if (options.checkpoint_size) {
    result += ", checkpoint_size " + *options.checkpoint_size;
  }
  if (options.checkpoint_interval) {
    result += ", checkpoint_interval " + *options.checkpoint_interval;
  }
  if (options.rewrite_file_size) {
    result += ", rewrite file_size " + *options.rewrite_file_size;
  }
  return result;
}

pbs_options draw_options(hegel::TestCase &tc, std::optional<bool> gtid_mode = {}) {
  pbs_options options;
  options.gtid_mode =
      gtid_mode ? *gtid_mode : tc.draw("gtid_mode", gs::booleans());
  options.verify_checksum = tc.draw("verify_checksum", gs::booleans());
  if (tc.draw("has_checkpoint_size", gs::booleans())) {
    options.checkpoint_size = tc.draw(
        "checkpoint_size", gs::sampled_from<std::string>({"1", "1K", "64K"}));
  }
  if (tc.draw("has_checkpoint_interval", gs::booleans())) {
    options.checkpoint_interval = "1s";
  }
  return options;
}

struct pbs_storage {
  temporary_directory directory{"pbs-pbt-mysql"};
  pbs_options options;
  std::string config_path;

  [[nodiscard]] fs::path vault() const { return directory.path() / "vault"; }
  [[nodiscard]] fs::path log() const { return directory.path() / "binsrv.log"; }
};

std::uint16_t listener_port() {
  // replication_source listener of 'pull'; one per test process
  return static_cast<std::uint16_t>(20000U + static_cast<unsigned>(::getpid()) % 20000U);
}

std::unique_ptr<pbs_storage> make_storage(const pbs_options &options) {
  auto result{std::make_unique<pbs_storage>()};
  result->options = options;
  fs::create_directories(result->vault());
  const auto server{settings()};
  json::object config;
  config["logger"] = json::object{{"level", "info"},
                                  {"file", result->log().string()}};
  config["connection"] = json::object{{"host", server.host},
                                      {"port", server.port},
                                      {"user", server.user},
                                      {"password", server.password},
                                      {"connect_timeout", 10},
                                      {"read_timeout", 1},
                                      {"write_timeout", 10}};
  json::object replication{{"server_id", 4242},
                           {"idle_time", 1},
                           {"verify_checksum", options.verify_checksum},
                           {"mode", options.gtid_mode ? "gtid" : "position"}};
  if (options.rewrite_file_size) {
    replication["rewrite"] = json::object{
        {"base_file_name", "rw"}, {"file_size", *options.rewrite_file_size}};
  }
  config["replication"] = replication;
  config["replication_source"] = json::object{
      {"port", listener_port()},
      {"read_timeout", 5},
      {"write_timeout", 5},
      {"authentication", json::object{{"user", "replica"},
                                      {"password", "replica-password"},
                                      {"plugin", "caching_sha2_password"}}}};
  json::object storage{{"backend", "file"},
                       {"uri", "file://" + result->vault().generic_string()}};
  if (options.checkpoint_size) {
    storage["checkpoint_size"] = *options.checkpoint_size;
  }
  if (options.checkpoint_interval) {
    storage["checkpoint_interval"] = *options.checkpoint_interval;
  }
  config["storage"] = storage;
  result->config_path = (result->directory.path() / "config.json").string();
  std::ofstream{result->config_path} << json::serialize(config);
  return result;
}

std::string log_tail(const pbs_storage &storage, std::size_t lines = 25U) {
  std::ifstream input{storage.log()};
  std::vector<std::string> all;
  for (std::string line; std::getline(input, line);) {
    all.push_back(std::move(line));
  }
  std::string result;
  for (auto i{all.size() > lines ? all.size() - lines : 0U}; i < all.size();
       ++i) {
    result += "\n    | " + all[i];
  }
  return result;
}

void fetch(hegel::TestCase &tc, const pbs_storage &storage) {
  const auto result{run({"fetch", storage.config_path})};
  if (result.signaled || result.exit_code != 0) {
    tc.note("binlog_server log:" + log_tail(storage));
  }
  pbt::require(!result.signaled && result.exit_code == 0,
               "binlog_server fetch failed (" + result.describe() + ")");
}

json::object list(hegel::TestCase &tc, const pbs_storage &storage) {
  const auto result{run({"list", storage.config_path})};
  pbt::require(!result.signaled && result.exit_code == 0,
               "binlog_server list failed (" + result.describe() + "): " +
                   result.output);
  json::value parsed;
  try {
    parsed = json::parse(result.output);
  } catch (const std::exception &e) {
    tc.note("list output: " + result.output);
    pbt::fail(std::string{"list output is not JSON: "} + e.what());
  }
  return parsed.as_object();
}

// ===========================================================================
// binlog files
// ===========================================================================
std::string read_file(const fs::path &path) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    throw std::runtime_error{"cannot read " + path.string()};
  }
  std::ostringstream content;
  content << input.rdbuf();
  return std::move(content).str();
}

std::string read_file_if_exists(const fs::path &path) {
  std::error_code error;
  return fs::exists(path, error) ? read_file(path) : std::string{};
}

// the server's binlog file as the dump thread sends it: in-use flag of the
// FORMAT_DESCRIPTION (common header flags at offset 4 + 17) cleared
std::string server_file(const server_info &info, const std::string &name) {
  auto content{read_file(info.binlog_dir / name)};
  constexpr std::size_t flags_offset{4U + 17U};
  if (content.size() > flags_offset) {
    content[flags_offset] = static_cast<char>(
        static_cast<unsigned char>(content[flags_offset]) & 0xFEU);
  }
  return content;
}

struct server_binlog {
  std::string name;
  std::uint64_t size{};
};

std::vector<server_binlog> server_binlogs(sql_connection &sql) {
  std::vector<server_binlog> result;
  for (const auto &item : sql.query("SHOW BINARY LOGS")) {
    result.push_back({item.at(0).value_or(""), std::stoull(item.at(1).value_or("0"))});
  }
  return result;
}

std::string strip_spaces(std::string text) {
  std::erase_if(text, [](char c) { return c == ' ' || c == '\n' || c == '\r'; });
  return text;
}

gtids::gtid_set server_previous_gtids(sql_connection &sql,
                                      const std::string &name) {
  const auto rows{
      sql.query("SHOW BINLOG EVENTS IN '" + name + "' LIMIT 1, 1")};
  if (rows.empty() || rows.front().at(2).value_or("") != "Previous_gtids") {
    throw std::runtime_error{"no Previous_gtids event in " + name};
  }
  return gtids::gtid_set{strip_spaces(rows.front().at(5).value_or(""))};
}

gtids::gtid_set server_executed(sql_connection &sql) {
  return gtids::gtid_set{strip_spaces(sql.scalar("SELECT @@GLOBAL.gtid_executed"))};
}

std::uint32_t le32(const std::string &data, std::size_t offset) {
  std::uint32_t value{0U};
  for (std::size_t i{0U}; i < 4U; ++i) {
    value |= static_cast<std::uint32_t>(
                 static_cast<unsigned char>(data[offset + i]))
             << (8U * i);
  }
  return value;
}

std::string uuid_text(const std::string &data, std::size_t offset) {
  static constexpr std::string_view digits{"0123456789abcdef"};
  std::string result;
  for (std::size_t i{0U}; i < 16U; ++i) {
    if (i == 4U || i == 6U || i == 8U || i == 10U) {
      result += '-';
    }
    const auto byte{static_cast<unsigned char>(data[offset + i])};
    result += digits[byte >> 4U];
    result += digits[byte & 0x0FU];
  }
  return result;
}

constexpr std::uint8_t rotate_event{4U};
constexpr std::uint8_t format_description_event{15U};
constexpr std::uint8_t previous_gtids_event{35U};
constexpr std::uint8_t gtid_event{33U};
constexpr std::uint8_t anonymous_gtid_event{34U};
constexpr std::uint8_t gtid_tagged_event{42U};

struct walked_file {
  std::vector<std::uint8_t> types;
  std::vector<std::string> untagged_gtids;  // "uuid:gno", in file order
  std::size_t transactions{};               // GTID / tagged / anonymous events
  std::size_t last_event_size{};
};

// Walks the events of a binlog file with an independent reader: magic,
// event sizes, log_pos (= end of the event) and CRC32 footers. Violations
// are reported with 'what'.
walked_file walk(const std::string &data, const std::string &what) {
  pbt::require(data.size() >= 4U && data.compare(0U, 4U, "\xFE" "bin") == 0,
               what + ": binlog magic");
  walked_file result;
  std::size_t offset{4U};
  constexpr std::size_t header_size{19U};
  while (offset < data.size()) {
    pbt::require(data.size() - offset >= header_size,
                 what + ": truncated event header at offset " +
                     std::to_string(offset));
    const auto type{static_cast<std::uint8_t>(data[offset + 4U])};
    const auto size{le32(data, offset + 9U)};
    const auto log_pos{le32(data, offset + 13U)};
    pbt::require(size >= header_size + 4U && size <= data.size() - offset,
                 what + ": event size " + std::to_string(size) +
                     " at offset " + std::to_string(offset));
    pbt::require_equal(static_cast<std::size_t>(log_pos), offset + size,
                       what + ": log_pos of the event at offset " +
                           std::to_string(offset));
    const auto *bytes{reinterpret_cast<const Bytef *>(data.data() + offset)};
    const auto crc{static_cast<std::uint32_t>(
        ::crc32(0UL, bytes, size - 4U))};
    pbt::require_equal(crc, le32(data, offset + size - 4U),
                       what + ": CRC32 of the event at offset " +
                           std::to_string(offset) + " (type " +
                           std::to_string(type) + ")");
    result.types.push_back(type);
    result.last_event_size = size;
    if (type == gtid_event) {
      // post header: flags (1), uuid (16), gno (8)
      const auto body{offset + header_size};
      std::uint64_t gno{0U};
      for (std::size_t i{0U}; i < 8U; ++i) {
        gno |= static_cast<std::uint64_t>(
                   static_cast<unsigned char>(data[body + 17U + i]))
               << (8U * i);
      }
      result.untagged_gtids.push_back(uuid_text(data, body + 1U) + ":" +
                                      std::to_string(gno));
    }
    if (type == gtid_event || type == gtid_tagged_event ||
        type == anonymous_gtid_event) {
      ++result.transactions;
    }
    offset += size;
  }
  return result;
}

std::string first_difference(const std::string &expected,
                             const std::string &actual) {
  const auto common{std::min(expected.size(), actual.size())};
  std::size_t position{0U};
  while (position < common && expected[position] == actual[position]) {
    ++position;
  }
  return "first difference at offset " + std::to_string(position) +
         " (server " + std::to_string(expected.size()) + " bytes, stored " +
         std::to_string(actual.size()) + " bytes)";
}

// ===========================================================================
// oracles
// ===========================================================================

// PBT-020: after the FORMAT_DESCRIPTION / PREVIOUS_GTIDS of a new binlog
// the reader is not at a transaction boundary, so a fetch / pull that ends
// before the first transaction of the server's newest binlog stores that
// binlog as the 4-byte magic only. In position mode the next run stores
// the rest; in GTID mode the server may start the next dump at a newer
// binlog (auto-positioning), and the 4-byte binlog stays as it is.
// 'tolerated' accepts exactly that (checked strictly by
// FetchStoresBinlogWithoutTransactionsMySQL) so that the other properties
// are not dominated by it.
//
// PBT-033: in GTID mode a binlog that the storage has up to its last
// transaction never gets the server's final ROTATE event: when the server
// rotates, the next dump starts at the new binlog (auto-positioning).
// Checked strictly by GtidResumeStoresRotateEventMySQL.
struct tolerances {
  bool pbt_020{true};
  bool pbt_033{true};
};

// the storage holds exactly the server's binlogs, byte for byte, and 'list'
// reports them (GTID mode: per-file previous / added GTIDs)
void require_storage_matches_server(
    hegel::TestCase &tc, sql_connection &sql, const server_info &info,
    const pbs_storage &storage, tolerances tolerate = {}) {
  auto binlogs{server_binlogs(sql)};
  const auto response{list(tc, storage)};
  // GTID auto-positioning: the server starts the dump at the newest binlog
  // whose PREVIOUS_GTIDS is a subset of the client's GTID set, so binlogs
  // without any transaction before that point are legitimately not sent.
  // Keep those the storage does not have only if they hold no transaction.
  if (storage.options.gtid_mode) {
    std::vector<std::string> stored_list;
    for (const auto &item : response.at("result").as_array()) {
      stored_list.emplace_back(item.at("name").as_string());
    }
    std::erase_if(binlogs, [&](const server_binlog &binlog) {
      if (std::ranges::find(stored_list, binlog.name) != stored_list.end()) {
        return false;
      }
      if (walk(server_file(info, binlog.name), "server " + binlog.name)
              .transactions != 0U) {
        return false;  // must be stored: reported by the comparison below
      }
      tc.note("(" + binlog.name + " has no transaction and was not sent "
              "by the server: GTID auto-positioning)");
      return true;
    });
  }
  pbt::require_equal(std::string{response.at("status").as_string()},
                     std::string{"success"}, "list status");
  const auto &stored{response.at("result").as_array()};
  // stored binlogs (by name) that PBT-020 left as the 4-byte magic
  std::vector<std::string> preamble_only;
  if (tolerate.pbt_020) {
    for (std::size_t i{0U}; i < stored.size(); ++i) {
      const auto &item{stored[i]};
      const std::string name{item.at("name").as_string()};
      // position mode resumes in that binlog: only the newest can be left
      // so; GTID mode can leave it behind (see above)
      if (!storage.options.gtid_mode && i + 1U != stored.size()) {
        continue;
      }
      const auto server_binlog_it{std::ranges::find(
          binlogs, name, [](const server_binlog &binlog) { return binlog.name; })};
      if (item.at("size").to_number<std::uint64_t>() == 4U &&
          server_binlog_it != binlogs.end() &&
          walk(server_file(info, name), "server " + name).transactions == 0U) {
        preamble_only.push_back(name);
        server_binlog_it->size = 4U;
        tc.note("(PBT-020 tolerated: " + name +
                " stored without its preamble)");
      }
    }
  }
  // stored binlogs (by name) that PBT-033 left without the final ROTATE
  std::vector<std::string> without_rotate;
  if (tolerate.pbt_033 && storage.options.gtid_mode) {
    for (std::size_t i{0U}; i + 1U < stored.size(); ++i) {
      const std::string name{stored[i].at("name").as_string()};
      const auto server_binlog_it{std::ranges::find(
          binlogs, name, [](const server_binlog &binlog) { return binlog.name; })};
      if (server_binlog_it == binlogs.end()) {
        continue;
      }
      const auto expected{server_file(info, name)};
      const auto actual{read_file(storage.vault() / name)};
      const auto events{walk(expected, "server " + name)};
      if (actual.size() < expected.size() && !events.types.empty() &&
          events.types.back() == rotate_event &&
          expected.size() - actual.size() == events.last_event_size &&
          expected.compare(0U, actual.size(), actual) == 0) {
        without_rotate.push_back(name);
        server_binlog_it->size = actual.size();
        tc.note("(PBT-033 tolerated: " + name +
                " stored without the server's final ROTATE)");
      }
    }
  }
  std::string server_names;
  std::string stored_names;
  for (const auto &binlog : binlogs) {
    server_names += binlog.name + "(" + std::to_string(binlog.size) + ") ";
  }
  for (const auto &item : stored) {
    stored_names += std::string{item.at("name").as_string()} + "(" +
                    std::to_string(item.at("size").to_number<std::uint64_t>()) +
                    ") ";
  }
  pbt::require_equal(stored_names, server_names,
                     "list: the server's binlogs with their sizes");

  std::vector<gtids::gtid_set> previous;
  if (storage.options.gtid_mode) {
    for (const auto &binlog : binlogs) {
      previous.push_back(server_previous_gtids(sql, binlog.name));
    }
    previous.push_back(server_executed(sql));
  }
  for (std::size_t i{0U}; i < binlogs.size(); ++i) {
    const auto &name{binlogs[i].name};
    auto expected{std::ranges::find(preamble_only, name) !=
                          preamble_only.end()
                      ? std::string{"\xFE" "bin"}
                      : server_file(info, name)};
    if (std::ranges::find(without_rotate, name) != without_rotate.end()) {
      expected.resize(binlogs[i].size);
    }
    const auto actual{read_file(storage.vault() / name)};
    pbt::require(actual == expected, "stored " + name +
                                         " differs from the server's file: " +
                                         first_difference(expected, actual));
    if (storage.options.gtid_mode) {
      const auto &item{stored[i].as_object()};
      pbt::require_equal(
          gtids::gtid_set{std::string_view{item.at("previous_gtids").as_string()}},
          previous[i], "list: previous_gtids of " + name);
      pbt::require_equal(
          gtids::gtid_set{std::string_view{item.at("added_gtids").as_string()}},
          previous[i + 1U] - previous[i], "list: added_gtids of " + name);
    }
  }
}

struct server_session {
  sql_connection sql;
  server_info info{inspect(sql)};
};

void start_case(hegel::TestCase &tc, server_session &server) {
  const auto max_binlog_size{tc.draw(
      "max_binlog_size",
      gs::sampled_from<std::uint64_t>({4096U, 16384U, 1073741824U}))};
  tc.note("server " + server.info.version + ", max_binlog_size " +
          std::to_string(max_binlog_size));
  reset_server(server.sql, server.info, max_binlog_size);
}

void wait_ms(std::uint64_t milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds{milliseconds});
}

} // namespace

// ===========================================================================
// D1: fetch copies the server's binlogs
// ===========================================================================

// After any workload one 'fetch' leaves the storage byte-identical to the
// server's binlog files (position and GTID mode, any checkpointing,
// verify_checksum on and off), and 'list' describes them.
BOOST_AUTO_TEST_CASE(FetchMatchesServerBinlogsMySQL) {
  pbt::check(
      [](hegel::TestCase &tc) {
        server_session server;
        start_case(tc, server);
        const auto options{draw_options(tc)};
        tc.note("binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        tc.note("workload:");
        apply_all(tc, server.sql, draw_workload(tc, server.info, 12U));
        fetch(tc, *storage);
        require_storage_matches_server(tc, server.sql, server.info, *storage);
      },
      {.test_cases = 100U, .slow = true});
}

// The server's newest binlog may have no transaction yet (right after
// FLUSH BINARY LOGS, a rotation or a reset): fetch still stores it as the
// server has it, with its FORMAT_DESCRIPTION and PREVIOUS_GTIDS events.
BOOST_AUTO_TEST_CASE(FetchStoresBinlogWithoutTransactionsMySQL) {
  pbt::check(
      [](hegel::TestCase &tc) {
        server_session server;
        start_case(tc, server);
        const auto options{draw_options(tc)};
        tc.note("binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        tc.note("workload:");
        auto workload{draw_workload(tc, server.info, 4U)};
        workload.push_back(operation{.kind = op_kind::flush});
        apply_all(tc, server.sql, workload);
        fetch(tc, *storage);
        require_storage_matches_server(tc, server.sql, server.info, *storage,
                                       {.pbt_020 = false, .pbt_033 = true});
      },
      {.test_cases = 10U, .slow = true});
}

// ===========================================================================
// D2: repeated fetch resumes where the previous one stopped
// ===========================================================================

// Workload and 'fetch' alternate on the same storage (resume from the last
// stored position / GTID set, also in the middle of an open binlog and after
// rotations); after every fetch the storage matches the server.
BOOST_AUTO_TEST_CASE(RepeatedFetchResumesMySQL) {
  pbt::check(
      [](hegel::TestCase &tc) {
        server_session server;
        start_case(tc, server);
        const auto options{draw_options(tc)};
        tc.note("binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        const auto rounds{tc.draw(
            "rounds", gs::integers<std::size_t>({.min_value = 2U, .max_value = 4U}))};
        for (std::size_t round{0U}; round < rounds; ++round) {
          tc.note("round " + std::to_string(round + 1U) + ":");
          apply_all(tc, server.sql, draw_workload(tc, server.info, 5U));
          fetch(tc, *storage);
          require_storage_matches_server(tc, server.sql, server.info, *storage);
        }
      },
      {.test_cases = 60U, .slow = true});
}

// GTID mode: the storage has a binlog up to its last transaction, then the
// server rotates (FLUSH BINARY LOGS) and writes more; the next fetch must
// still store the server's ROTATE at the end of the old binlog, so that
// the stored binlogs stay copies of the server's.
BOOST_AUTO_TEST_CASE(GtidResumeStoresRotateEventMySQL) {
  pbt::check(
      [](hegel::TestCase &tc) {
        server_session server;
        start_case(tc, server);
        const auto options{draw_options(tc, true)};
        tc.note("binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        tc.note("round 1:");
        auto first{draw_workload(tc, server.info, 3U)};
        first.push_back(operation{.kind = op_kind::insert_rows, .count = 1});
        apply_all(tc, server.sql, first);
        fetch(tc, *storage);
        tc.note("round 2:");
        std::vector<operation> second{operation{.kind = op_kind::flush}};
        const auto more{draw_workload(tc, server.info, 3U)};
        second.insert(second.end(), more.begin(), more.end());
        second.push_back(operation{.kind = op_kind::insert_rows, .count = 1});
        apply_all(tc, server.sql, second);
        fetch(tc, *storage);
        require_storage_matches_server(tc, server.sql, server.info, *storage,
                                       {.pbt_020 = true, .pbt_033 = false});
      },
      {.test_cases = 10U, .slow = true});
}

// ===========================================================================
// D3: pull, killed and restarted
// ===========================================================================

// 'pull' runs while the workload runs and is killed (SIGKILL) or stopped
// (SIGTERM) and restarted at random points. It never exits on its own,
// stops within a few seconds on SIGTERM with exit code 0, and the storage
// the last 'pull' leaves matches the server (also after a 'fetch'): a kill
// never leaves a storage that cannot be resumed or that has lost /
// duplicated transactions.
BOOST_AUTO_TEST_CASE(KilledPullResumesMySQL) {
  pbt::check(
      [](hegel::TestCase &tc) {
        server_session server;
        start_case(tc, server);
        const auto options{draw_options(tc)};
        tc.note("binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        const std::vector<std::string> pull_arguments{"pull", storage->config_path};
        // binlog_server truncates its log when it starts: keep the logs
        // of earlier runs as binsrv.log.<n>
        std::size_t runs{0U};
        const auto launch{[&]() {
          std::error_code ignored;
          if (fs::exists(storage->log(), ignored)) {
            fs::rename(storage->log(),
                       storage->log().string() + "." + std::to_string(++runs),
                       ignored);
          }
          return std::make_unique<background_run>(pull_arguments);
        }};
        auto pull{launch()};
        const auto restart{[&]() { pull = launch(); }};

        const auto require_still_running{[&]() {
          if (const auto exited{pull->poll()}; exited) {
            tc.note("binlog_server log:" + log_tail(*storage));
            pbt::fail("binlog_server pull exited on its own (" +
                      exited->describe() + ")");
          }
        }};
        const auto stop_gracefully{[&]() {
          // a SIGTERM before the handlers are installed terminates any
          // program: wait until 'pull' has logged that it has them
          const auto deadline{std::chrono::steady_clock::now() +
                              std::chrono::seconds{20}};
          const auto handlers_installed{[&]() {
            return read_file_if_exists(storage->log())
                       .find("set custom handlers for SIGINT and SIGTERM") !=
                   std::string::npos;
          }};
          while (!handlers_installed()) {
            require_still_running();
            pbt::require(std::chrono::steady_clock::now() < deadline,
                         "pull did not install its signal handlers in 20 s");
            wait_ms(10U);
          }
          pull->send(SIGTERM);
          const auto [result, in_time]{pull->wait_for(std::chrono::seconds{20})};
          if (!in_time || result.signaled || result.exit_code != 0) {
            tc.note("binlog_server log:" + log_tail(*storage));
          }
          pbt::require(in_time, "pull did not stop within 20 s of SIGTERM");
          pbt::require(!result.signaled && result.exit_code == 0,
                       "pull stopped by SIGTERM with " + result.describe());
        }};

        const auto steps{tc.draw(
            "steps", gs::integers<std::size_t>({.min_value = 1U, .max_value = 8U}))};
        for (std::size_t step{0U}; step < steps; ++step) {
          const auto action{tc.draw(
              "action", gs::sampled_from<std::string>(
                            {"workload", "workload", "sleep", "kill", "stop"}))};
          if (action == "workload") {
            tc.note("workload:");
            apply_all(tc, server.sql, draw_workload(tc, server.info, 3U));
          } else if (action == "sleep") {
            const auto delay{tc.draw("sleep_ms", gs::sampled_from<std::uint64_t>(
                                                     {10U, 100U, 400U, 1500U}))};
            tc.note("sleep " + std::to_string(delay) + " ms");
            wait_ms(delay);
          } else {
            const auto delay{tc.draw(
                "delay_ms", gs::sampled_from<std::uint64_t>({0U, 5U, 50U, 300U}))};
            wait_ms(delay);
            require_still_running();
            if (action == "kill") {
              tc.note("kill -9 pull after " + std::to_string(delay) + " ms");
              pull->send(SIGKILL);
              std::ignore = pull->wait();
            } else {
              tc.note("SIGTERM pull after " + std::to_string(delay) + " ms");
              stop_gracefully();
            }
            restart();
          }
        }
        // pull receives events as they are written; read_timeout and
        // idle_time are 1 s, so 3 s is enough to catch up after a reconnect
        wait_ms(3000U);
        require_still_running();
        tc.note("SIGTERM pull, compare, then fetch and compare again");
        stop_gracefully();
        require_storage_matches_server(tc, server.sql, server.info, *storage);
        fetch(tc, *storage);
        require_storage_matches_server(tc, server.sql, server.info, *storage);
      },
      {.test_cases = 20U, .slow = true});
}

// ===========================================================================
// D4: search_by_gtid_set on real data
// ===========================================================================

// After a GTID-mode fetch, searching for some of the server's GTIDs returns
// exactly the binlogs (in order) whose GTID_LOG events hold them (the
// minimal covering set: every GTID is in exactly one binlog).
BOOST_AUTO_TEST_CASE(SearchByGtidSetMatchesServerMySQL) {
  pbt::check(
      [](hegel::TestCase &tc) {
        server_session server;
        start_case(tc, server);
        auto options{draw_options(tc, true)};
        const auto storage{make_storage(options)};
        tc.note("workload:");
        apply_all(tc, server.sql,
                  {operation{.kind = op_kind::insert_rows, .count = 1}});
        apply_all(tc, server.sql, draw_workload(tc, server.info, 10U));
        fetch(tc, *storage);

        std::vector<std::pair<std::string, std::string>> located;  // gtid, binlog
        for (const auto &binlog : server_binlogs(server.sql)) {
          const auto walked{
              walk(server_file(server.info, binlog.name), "server " + binlog.name)};
          for (const auto &gtid : walked.untagged_gtids) {
            located.emplace_back(gtid, binlog.name);
          }
        }
        pbt::require(!located.empty(), "the workload wrote GTID_LOG events");
        const auto picks{tc.draw(
            "picks", gs::integers<std::size_t>({.min_value = 1U, .max_value = 4U}))};
        gtids::gtid_set query;
        std::vector<std::string> expected;
        for (std::size_t i{0U}; i < picks; ++i) {
          const auto index{tc.draw(
              "gtid", gs::integers<std::size_t>({.max_value = located.size() - 1U}))};
          query += gtids::gtid_set{located[index].first};
          expected.push_back(located[index].second);
        }
        std::ranges::sort(expected);
        expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
        tc.note("search_by_gtid_set " + query.str());
        const auto result{run({"search_by_gtid_set", storage->config_path, query.str()})};
        tc.note("output: " + result.output);
        pbt::require(!result.signaled && result.exit_code == 0,
                     "search_by_gtid_set failed (" + result.describe() + ")");
        const auto response{json::parse(result.output).as_object()};
        std::string actual_names;
        for (const auto &item : response.at("result").as_array()) {
          actual_names += std::string{item.at("name").as_string()} + " ";
        }
        std::string expected_names;
        for (const auto &name : expected) {
          expected_names += name + " ";
        }
        pbt::require_equal(actual_names, expected_names,
                           "binlogs holding the GTIDs");
      },
      {.test_cases = 60U, .slow = true});
}

// ===========================================================================
// D5: rewrite mode
// ===========================================================================

// In rewrite mode the stored binlogs (rw.000001, rw.000002, ...) are
// well-formed (magic, FORMAT_DESCRIPTION, PREVIOUS_GTIDS, consistent
// log_pos, valid CRC32), every binlog but the last reaches file_size, and
// together they hold every transaction of the server in the server's
// order; 'list' reports their GTIDs.
namespace {

void check_rewrite_mode(hegel::TestCase &tc, bool verify_checksum) {
        server_session server;
        start_case(tc, server);
        auto options{draw_options(tc, true)};
        options.verify_checksum = verify_checksum;
        options.rewrite_file_size = tc.draw(
            "file_size", gs::sampled_from<std::string>({"1K", "4K", "64K"}));
        tc.note("binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        const auto rounds{tc.draw(
            "rounds", gs::integers<std::size_t>({.min_value = 1U, .max_value = 3U}))};
        for (std::size_t round{0U}; round < rounds; ++round) {
          tc.note("round " + std::to_string(round + 1U) + ":");
          apply_all(tc, server.sql, draw_workload(tc, server.info, 8U));
          fetch(tc, *storage);
        }

        std::vector<std::string> server_order;
        std::size_t server_transactions{0U};
        for (const auto &binlog : server_binlogs(server.sql)) {
          const auto walked{
              walk(server_file(server.info, binlog.name), "server " + binlog.name)};
          server_order.insert(server_order.end(), walked.untagged_gtids.begin(),
                              walked.untagged_gtids.end());
          server_transactions += walked.transactions;
        }

        const auto response{list(tc, *storage)};
        const auto &stored{response.at("result").as_array()};
        const auto file_size{[&]() -> std::uint64_t {
          const auto &text{*options.rewrite_file_size};
          return std::stoull(text) * (text.back() == 'K' ? 1024U : 1U);
        }()};
        std::vector<std::string> stored_order;
        std::size_t stored_transactions{0U};
        gtids::gtid_set stored_gtids;
        for (std::size_t i{0U}; i < stored.size(); ++i) {
          const auto &item{stored[i].as_object()};
          const std::string name{item.at("name").as_string()};
          auto expected_name{std::to_string(i + 1U)};
          expected_name.insert(0U, expected_name.size() < 6U
                                       ? 6U - expected_name.size()
                                       : 0U,
                               '0');
          expected_name.insert(0U, "rw.");
          pbt::require_equal(name, expected_name,
                             "rewritten binlog names are consecutive");
          const auto data{read_file(storage->vault() / name)};
          pbt::require_equal(std::uint64_t{data.size()},
                             item.at("size").to_number<std::uint64_t>(),
                             "list: size of " + name);
          const auto walked{walk(data, "stored " + name)};
          pbt::require(walked.types.size() >= 2U &&
                           walked.types[0] == format_description_event &&
                           walked.types[1] == previous_gtids_event,
                       name + " starts with FORMAT_DESCRIPTION, PREVIOUS_GTIDS");
          if (i + 1U < stored.size()) {
            pbt::require(data.size() >= file_size,
                         name + " (" + std::to_string(data.size()) +
                             " bytes) was closed before reaching file_size");
          }
          pbt::require_equal(
              gtids::gtid_set{std::string_view{item.at("previous_gtids").as_string()}},
              stored_gtids, "list: previous_gtids of " + name);
          const gtids::gtid_set added{
              std::string_view{item.at("added_gtids").as_string()}};
          stored_gtids += added;
          stored_order.insert(stored_order.end(), walked.untagged_gtids.begin(),
                              walked.untagged_gtids.end());
          stored_transactions += walked.transactions;
        }
        pbt::require_equal(stored_gtids, server_executed(server.sql),
                           "stored GTIDs = the server's gtid_executed");
        pbt::require_equal(stored_transactions, server_transactions,
                           "number of transactions");
        std::string server_text;
        std::string stored_text;
        for (const auto &gtid : server_order) {
          server_text += gtid + " ";
        }
        for (const auto &gtid : stored_order) {
          stored_text += gtid + " ";
        }
        pbt::require_equal(stored_text, server_text,
                           "transactions in the server's order");
}

} // namespace

BOOST_AUTO_TEST_CASE(RewriteModeKeepsTransactionsMySQL) {
  pbt::check([](hegel::TestCase &tc) { check_rewrite_mode(tc, true); },
             {.test_cases = 60U, .slow = true});
}

// The same with replication.verify_checksum = false (README: rewrite mode
// needs a source that writes CRC32 checksums, which this server does; the
// option only controls whether PBS verifies them).
BOOST_AUTO_TEST_CASE(RewriteModeWithoutChecksumVerificationMySQL) {
  pbt::check([](hegel::TestCase &tc) { check_rewrite_mode(tc, false); },
             {.test_cases = 10U, .slow = true});
}
