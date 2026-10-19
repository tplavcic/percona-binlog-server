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
//  - rewrite mode keeps every transaction, in order, in well-formed files;
//  - the replication source listener of 'pull' sends exactly the events
//    the server sends from the same position, and a MySQL replica of the
//    listener catches up with the source;
//  - server restarts / crashes during 'pull' and PURGE BINARY LOGS on the
//    server leave a storage that matches the server (or a fetch that fails
//    cleanly when the server no longer has what the storage needs).
//
// The server: PBT_MYSQL_HOST / _PORT / _USER / _PASSWORD (set by
// run_property_tests.py, which starts a private server with
// extra/qa/mysqld.sh). It must run with log_bin, gtid_mode=ON and
// binlog_checksum=CRC32, and its binlog files must be readable by this
// process. Its binary logs and GTIDs are reset - never point these tests
// at a server whose data matters. The replica properties use a second
// server (PBT_MYSQL_REPLICA_PORT, mysqld.sh --replica), the restart
// properties restart the server with PBT_MYSQLD_CTL (mysqld.sh).

#define BOOST_TEST_MODULE MySqlPropertyTests

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <poll.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mysql/mysql.h>

#include <zlib.h>

#include <openssl/evp.h>

#include <boost/test/unit_test.hpp>

#include <boost/json/array.hpp>
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
  sql_connection() : sql_connection(std::nullopt) {}
  // 'port': another server on the same host with the same account (the
  // replica instance)
  explicit sql_connection(std::optional<std::uint16_t> port)
      : handle_{::mysql_init(nullptr)} {
    if (handle_ == nullptr) {
      throw std::runtime_error{"mysql_init failed"};
    }
    auto server{settings()};
    if (port) {
      server.port = *port;
    }
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

  // the first row as column name -> value (empty if no row)
  std::vector<std::pair<std::string, std::string>>
  first_row_named(std::string_view statement) {
    if (::mysql_real_query(handle_, statement.data(), statement.size()) != 0) {
      throw std::runtime_error{"SQL error: " + std::string{statement} + ": " +
                               ::mysql_error(handle_)};
    }
    std::vector<std::pair<std::string, std::string>> result;
    MYSQL_RES *rows{::mysql_store_result(handle_)};
    if (rows == nullptr) {
      return result;
    }
    const auto fields{::mysql_num_fields(rows)};
    const MYSQL_FIELD *names{::mysql_fetch_fields(rows)};
    if (MYSQL_ROW values = ::mysql_fetch_row(rows)) {
      const auto *lengths{::mysql_fetch_lengths(rows)};
      for (unsigned i{0U}; i < fields; ++i) {
        result.emplace_back(names[i].name,
                            values[i] == nullptr
                                ? std::string{}
                                : std::string{values[i], lengths[i]});
      }
    }
    ::mysql_free_result(rows);
    return result;
  }

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
  std::string gtid_mode;  // @@GLOBAL.gtid_mode (set per case: start_case)

  [[nodiscard]] bool at_least(int ma, int mi) const {
    return major > ma || (major == ma && minor >= mi);
  }
  // tagged GTIDs: MySQL 8.3+ and GTID_MODE=ON (SET gtid_next =
  // 'AUTOMATIC:<tag>')
  [[nodiscard]] bool has_tagged_gtids() const {
    return at_least(8, 3) && gtid_mode == "ON";
  }
  [[nodiscard]] std::string binlog_status_statement() const {
    return at_least(8, 2) ? "SHOW BINARY LOG STATUS" : "SHOW MASTER STATUS";
  }
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
      sql.scalar("SELECT @@GLOBAL.enforce_gtid_consistency") != "ON" ||
      sql.scalar("SELECT @@GLOBAL.binlog_checksum") != "CRC32") {
    pbt::infrastructure_error("the MySQL server must run with log_bin, "
                              "enforce_gtid_consistency=ON and "
                              "binlog_checksum=CRC32 (extra/qa/mysqld.sh)");
  }
  info.gtid_mode = sql.scalar("SELECT @@GLOBAL.gtid_mode");
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

// @@GLOBAL.gtid_mode changes one step at a time (OFF <-> OFF_PERMISSIVE <->
// ON_PERMISSIVE <-> ON); the session must not be replicating
void set_gtid_mode(sql_connection &sql, server_info &info,
                   const std::string &target) {
  static const std::vector<std::string> steps{"OFF", "OFF_PERMISSIVE",
                                              "ON_PERMISSIVE", "ON"};
  const auto position{[](const std::string &mode) {
    return std::ranges::find(steps, mode) - steps.begin();
  }};
  auto current{position(sql.scalar("SELECT @@GLOBAL.gtid_mode"))};
  const auto wanted{position(target)};
  while (current != wanted) {
    current += current < wanted ? 1 : -1;
    sql.exec("SET GLOBAL gtid_mode = " + steps[static_cast<std::size_t>(current)]);
  }
  info.gtid_mode = target;
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
    pbt::note(tc, "  " + describe(op));
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
  bool s3{false};  // storage.backend s3 (SeaweedFS, PBT_S3_*) instead of file
  // S3 endpoint port PBS connects to instead of PBT_S3_ENDPOINT's (the
  // outage proxy); the oracle still reads through the filer
  std::optional<std::uint16_t> s3_port;
  // storage.encryption: KEK (keyring) and data cipher
  struct encryption_options {
    std::string kek_cipher;
    std::string kek_hex;
    std::string data_cipher;
  };
  std::optional<encryption_options> encryption;
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
  if (options.s3) {
    result += ", S3 storage";
  }
  if (options.encryption) {
    result += ", encrypted (KEK " + options.encryption->kek_cipher + ", data " +
              options.encryption->data_cipher + ")";
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
  // README "storage.encryption": the data cipher must be in CTR mode; the
  // KEK's mode one of ECB / CBC / CTR / GCM; an ECB / CBC KEK can only
  // encrypt file keys whose length is a multiple of 16 bytes. Exact
  // OpenSSL names (PBT-012 / PBT-013 are covered elsewhere).
  if (tc.draw("encrypted", gs::integers<int>({.min_value = 0, .max_value = 2})) == 0) {
    const auto kek_cipher{tc.draw(
        "kek_cipher", gs::sampled_from<std::string>(
                          {"AES-128-ECB", "AES-256-CBC", "AES-256-GCM",
                           "AES-192-CTR", "CAMELLIA-128-CBC"}))};
    const bool block_kek{kek_cipher.ends_with("ECB") || kek_cipher.ends_with("CBC")};
    std::vector<std::string> data_ciphers{"AES-128-CTR", "AES-256-CTR",
                                          "ARIA-128-CTR", "CAMELLIA-256-CTR"};
    if (!block_kek) {
      data_ciphers.emplace_back("AES-192-CTR");
    }
    const auto key_bytes{kek_cipher.find("-128-") != std::string::npos ? 16U
                         : kek_cipher.find("-192-") != std::string::npos ? 24U
                                                                         : 32U};
    std::string kek_hex;
    static constexpr std::string_view digits{"0123456789abcdef"};
    for (std::size_t i{0U}; i < key_bytes * 2U; ++i) {
      kek_hex += digits[(i * 7U + key_bytes) % 16U];
    }
    options.encryption = pbs_options::encryption_options{
        kek_cipher, kek_hex,
        tc.draw("data_cipher", gs::sampled_from(data_ciphers))};
  }
  return options;
}

// S3: the objects are read back independently of PBS, through the
// SeaweedFS filer's plain HTTP interface (PBT_S3_FILER, unsigned; the same
// one extra/qa/seaweedfs.sh uses for its readiness check):
// <filer>/buckets/<bucket>/<prefix>/<object>
struct s3_location {
  std::string uri;     // storage.uri (README: http://key:secret@host:port/bucket/path)
  std::string filer;   // <filer>/buckets/<bucket>/<prefix>
};

s3_location s3_location_for(const std::string &prefix,
                            std::optional<std::uint16_t> port = {}) {
  const auto endpoint{pbt::detail::env("PBT_S3_ENDPOINT")};
  const auto access_key{pbt::detail::env("PBT_S3_ACCESS_KEY")};
  const auto secret_key{pbt::detail::env("PBT_S3_SECRET_KEY")};
  const auto bucket{pbt::detail::env("PBT_S3_BUCKET")};
  const auto filer{pbt::detail::env("PBT_S3_FILER")};
  if (!endpoint || !access_key || !secret_key || !bucket || !filer) {
    pbt::infrastructure_error(
        "S3 endpoint not configured: set PBT_S3_ENDPOINT / _ACCESS_KEY / "
        "_SECRET_KEY / _BUCKET / _FILER (extra/qa/seaweedfs.sh env; "
        "run_property_tests.py starts SeaweedFS)");
  }
  const auto scheme_end{endpoint->find("://")};
  if (scheme_end == std::string::npos) {
    pbt::infrastructure_error("PBT_S3_ENDPOINT must be http[s]://host:port");
  }
  auto host_port{endpoint->substr(scheme_end + 3U)};
  if (port) {
    host_port = "127.0.0.1:" + std::to_string(*port);
  }
  return {endpoint->substr(0U, scheme_end + 3U) + *access_key + ":" +
              *secret_key + "@" + host_port + "/" + *bucket + "/" + prefix,
          *filer + "/buckets/" + *bucket + "/" + prefix};
}

struct pbs_storage {
  temporary_directory directory{"pbs-pbt-mysql"};
  pbs_options options;
  std::string config_path;
  std::optional<s3_location> s3;

  pbs_storage() = default;
  pbs_storage(const pbs_storage &) = delete;
  pbs_storage &operator=(const pbs_storage &) = delete;
  pbs_storage(pbs_storage &&) = delete;
  pbs_storage &operator=(pbs_storage &&) = delete;
  ~pbs_storage();

  [[nodiscard]] fs::path vault() const { return directory.path() / "vault"; }
  [[nodiscard]] fs::path log() const { return directory.path() / "binsrv.log"; }
  // the stored binlog file / object
  [[nodiscard]] std::string stored(const std::string &name) const;
  // its plaintext: decrypted independently of PBS (OpenSSL EVP, the KEK of
  // the test's keyring and the envelopes in <binlog>.json) when the storage
  // is encrypted, else the stored bytes
  [[nodiscard]] std::string plaintext(const std::string &name) const;
};

std::uint16_t listener_port() {
  // replication_source listener of 'pull'; one per test process
  return static_cast<std::uint16_t>(20000U + static_cast<unsigned>(::getpid()) % 20000U);
}

// one RSA key per test process for the listener (openssl genrsa)
const fs::path &listener_rsa_key() {
  static const fs::path key{[]() {
    const auto path{fs::temp_directory_path() /
                    ("pbs-pbt-rsa-" + std::to_string(::getpid()) + ".pem")};
    std::error_code ignored;
    if (!fs::exists(path, ignored)) {
      const std::string command{"openssl genrsa -out '" + path.string() +
                                "' 2048 >/dev/null 2>&1"};
      // NOLINTNEXTLINE(concurrency-mt-unsafe,cert-env33-c)
      if (std::system(command.c_str()) != 0) {
        pbt::infrastructure_error(
            "cannot generate the listener's RSA key (openssl genrsa)");
      }
    }
    return path;
  }()};
  return key;
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
  // README: server_id / server_uuid are what downstream replicas see (must
  // differ from theirs: the source is 1, the replica 2); without TLS the
  // listener's caching_sha2_password full authentication needs the RSA key
  config["replication_source"] = json::object{
      {"port", listener_port()},
      {"server_id", 142},
      {"server_uuid", "00000000-0000-0000-0000-00000000008e"},
      {"auth_rsa_private_key", listener_rsa_key().string()},
      {"read_timeout", 5},
      {"write_timeout", 5},
      {"authentication", json::object{{"user", "replica"},
                                      {"password", "replica-password"},
                                      {"plugin", "caching_sha2_password"}}}};
  json::object storage{{"backend", "file"},
                       {"uri", "file://" + result->vault().generic_string()}};
  if (options.s3) {
    result->s3 = s3_location_for(result->directory.path().filename().string(),
                                 options.s3_port);
    storage["backend"] = "s3";
    storage["uri"] = result->s3->uri;
    // partially uploaded binlogs: inside the test's directory ($TMPDIR)
    fs::create_directories(result->directory.path() / "s3-buffer");
    storage["fs_buffer_directory"] =
        (result->directory.path() / "s3-buffer").string();
  }
  if (options.checkpoint_size) {
    storage["checkpoint_size"] = *options.checkpoint_size;
  }
  if (options.checkpoint_interval) {
    storage["checkpoint_interval"] = *options.checkpoint_interval;
  }
  if (options.encryption) {
    const auto keyring_path{result->directory.path() / "keyring.json"};
    std::ofstream{keyring_path} << json::serialize(json::object{
        {"version", 1},
        {"keys", json::array{json::object{{"id", "pbt-kek"},
                                          {"cipher", options.encryption->kek_cipher},
                                          {"data_hex", options.encryption->kek_hex}}}}});
    config["keyring"] = json::object{{"uri", "file://" + keyring_path.string()}};
    storage["encryption"] = json::object{{"format", "generic"},
                                         {"kek_id", "pbt-kek"},
                                         {"cipher", options.encryption->data_cipher}};
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
    pbt::note(tc, "binlog_server log:" + log_tail(storage));
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
    pbt::note(tc, "list output: " + result.output);
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

// curl, as extra/qa/seaweedfs.sh uses it
int run_curl(const std::string &arguments) {
  const std::string command{"curl --silent --show-error --max-time 60 " +
                            arguments + " >/dev/null 2>&1"};
  // NOLINTNEXTLINE(concurrency-mt-unsafe,cert-env33-c)
  return std::system(command.c_str());
}

std::string pbs_storage::stored(const std::string &name) const {
  if (!s3) {
    return read_file(vault() / name);
  }
  const auto target{directory.path() / ("downloaded." + name)};
  std::error_code ignored;
  fs::remove(target, ignored);
  if (run_curl("--fail --output '" + target.string() + "' '" + s3->filer +
               "/" + name + "'") != 0) {
    throw std::runtime_error{"cannot read the S3 object " + name +
                             " through the SeaweedFS filer"};
  }
  auto content{read_file(target)};
  fs::remove(target, ignored);
  return content;
}

namespace {

std::string from_hex(std::string_view text) {
  const auto digit{[](char value) -> int {
    if (value >= '0' && value <= '9') {
      return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
      return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
      return value - 'A' + 10;
    }
    throw std::invalid_argument{"invalid hex digit"};
  }};
  std::string result;
  for (std::size_t i{0U}; i + 1U < text.size(); i += 2U) {
    result += static_cast<char>((digit(text[i]) << 4) | digit(text[i + 1U]));
  }
  return result;
}

std::string evp_decrypt(const std::string &cipher_name, const std::string &key,
                        const std::string &iv, const std::string &tag,
                        const std::string &input) {
  const EVP_CIPHER *cipher{EVP_get_cipherbyname(cipher_name.c_str())};
  pbt::require(cipher != nullptr, "oracle: unknown cipher " + cipher_name);
  const std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx{
      EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free};
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto *key_bytes{reinterpret_cast<const unsigned char *>(key.data())};
  const auto *iv_bytes{iv.empty() ? nullptr
                                  : reinterpret_cast<const unsigned char *>(iv.data())};
  pbt::require(EVP_DecryptInit_ex(ctx.get(), cipher, nullptr, key_bytes,
                                  iv_bytes) == 1,
               "oracle: EVP_DecryptInit_ex");
  EVP_CIPHER_CTX_set_padding(ctx.get(), 0);
  std::string output(input.size() + 32U, '\0');
  int length{0};
  pbt::require(EVP_DecryptUpdate(ctx.get(),
                                 reinterpret_cast<unsigned char *>(output.data()),
                                 &length,
                                 reinterpret_cast<const unsigned char *>(input.data()),
                                 static_cast<int>(input.size())) == 1,
               "oracle: EVP_DecryptUpdate");
  auto total{static_cast<std::size_t>(length)};
  if (!tag.empty()) {
    std::string tag_copy{tag};
    pbt::require(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_TAG,
                                     static_cast<int>(tag_copy.size()),
                                     tag_copy.data()) == 1,
                 "oracle: set tag");
  }
  pbt::require(EVP_DecryptFinal_ex(
                   ctx.get(),
                   reinterpret_cast<unsigned char *>(output.data()) + total,
                   &length) == 1,
               "oracle: EVP_DecryptFinal_ex (file key authentication)");
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
  total += static_cast<std::size_t>(length);
  output.resize(total);
  return output;
}

std::string optional_hex(const json::object &object, std::string_view key) {
  const auto *value{object.if_contains(key)};
  if (value == nullptr || value->is_null()) {
    return {};
  }
  return from_hex(value->as_string());
}

} // namespace

// README: file key = KEK-decrypt(file_key_envelope), data =
// <file_data_envelope.cipher>-decrypt(file key, iv) from offset 0
std::string pbs_storage::plaintext(const std::string &name) const {
  auto data{stored(name)};
  if (!options.encryption) {
    return data;
  }
  const auto metadata{json::parse(stored(name + ".json")).as_object()};
  const auto *encryption{metadata.if_contains("encryption")};
  pbt::require(encryption != nullptr && encryption->is_object(),
               "binlog " + name + " of an encrypted storage has no "
               "encryption envelopes in its metadata");
  const auto &key_envelope{encryption->as_object().at("file_key_envelope").as_object()};
  const auto &data_envelope{encryption->as_object().at("file_data_envelope").as_object()};
  pbt::require_equal(std::string{key_envelope.at("kek_id").as_string()},
                     std::string{"pbt-kek"}, "file_key_envelope.kek_id of " + name);
  pbt::require_equal(std::string{data_envelope.at("cipher").as_string()},
                     options.encryption->data_cipher,
                     "file_data_envelope.cipher of " + name);
  const auto file_key{evp_decrypt(options.encryption->kek_cipher,
                                  from_hex(options.encryption->kek_hex),
                                  optional_hex(key_envelope, "iv_hex"),
                                  optional_hex(key_envelope, "tag_hex"),
                                  from_hex(key_envelope.at("data_hex").as_string()))};
  return evp_decrypt(options.encryption->data_cipher, file_key,
                     from_hex(data_envelope.at("iv_hex").as_string()), {}, data);
}

pbs_storage::~pbs_storage() {
  if (s3) {
    // every case uses its own prefix: remove it, the bucket is shared
    std::ignore = run_curl("-X DELETE '" + s3->filer + "/?recursive=true'");
  }
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

constexpr std::uint8_t stop_event{3U};
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
  std::vector<std::size_t> offsets;  // start of every event
  std::uint32_t min_timestamp{};     // over every event (0: no event)
  std::uint32_t max_timestamp{};
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
    result.offsets.push_back(offset);
    const auto timestamp{le32(data, offset)};
    result.min_timestamp = result.types.size() == 1U
                               ? timestamp
                               : std::min(result.min_timestamp, timestamp);
    result.max_timestamp = std::max(result.max_timestamp, timestamp);
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
// transaction never gets the server's final ROTATE (rotation) or STOP
// (server shutdown) event: the next dump starts at the new binlog
// (auto-positioning).
// Checked strictly by GtidResumeStoresRotateEventMySQL.
struct tolerances {
  bool pbt_020{true};
  bool pbt_033{true};
};

// the storage holds exactly the server's binlogs, byte for byte, and 'list'
// reports them (GTID mode: per-file previous / added GTIDs). Stored binlogs
// named in 'ignore_stored' (purged on the server) are left out.
void require_storage_matches_server(
    hegel::TestCase &tc, sql_connection &sql, const server_info &info,
    const pbs_storage &storage, tolerances tolerate = {},
    const std::vector<std::string> &ignore_stored = {}) {
  auto binlogs{server_binlogs(sql)};
  const auto response{list(tc, storage)};
  pbt::require_equal(std::string{response.at("status").as_string()},
                     std::string{"success"}, "list status");
  json::array stored_items;
  for (const auto &item : response.at("result").as_array()) {
    if (std::ranges::find(ignore_stored,
                          std::string{item.at("name").as_string()}) ==
        ignore_stored.end()) {
      stored_items.push_back(item);
    }
  }
  // GTID auto-positioning: the server starts the dump at the newest binlog
  // whose PREVIOUS_GTIDS is a subset of the client's GTID set, so binlogs
  // without any transaction before that point are legitimately not sent.
  // Keep those the storage does not have only if they hold no transaction.
  if (storage.options.gtid_mode) {
    std::vector<std::string> stored_list;
    for (const auto &item : stored_items) {
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
      pbt::note(tc, "(" + binlog.name + " has no transaction and was not sent "
              "by the server: GTID auto-positioning)");
      return true;
    });
  }
  const auto &stored{stored_items};
  // stored binlogs (by name) that PBT-020 left as the 4-byte magic
  std::vector<std::string> preamble_only;
  if (tolerate.pbt_020) {
    for (std::size_t i{0U}; i < stored.size(); ++i) {
      const auto &item{stored[i]};
      const std::string name{item.at("name").as_string()};
      // position mode resumes in that binlog: only the newest can be left
      // so, or one the server left without ROTATE / STOP (killed: the dump
      // re-sends its preamble and moves on to the next binlog); GTID mode
      // can leave any behind (see above)
      if (!storage.options.gtid_mode && i + 1U != stored.size()) {
        const auto events{walk(server_file(info, name), "server " + name)};
        if (!events.types.empty() && (events.types.back() == rotate_event ||
                                      events.types.back() == stop_event)) {
          continue;
        }
      }
      const auto server_binlog_it{std::ranges::find(
          binlogs, name, [](const server_binlog &binlog) { return binlog.name; })};
      if (item.at("size").to_number<std::uint64_t>() == 4U &&
          server_binlog_it != binlogs.end() &&
          walk(server_file(info, name), "server " + name).transactions == 0U) {
        preamble_only.push_back(name);
        server_binlog_it->size = 4U;
        pbt::note(tc, "(PBT-020 tolerated: " + name +
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
      const auto actual{storage.plaintext(name)};
      const auto events{walk(expected, "server " + name)};
      if (actual.size() < expected.size() && !events.types.empty() &&
          (events.types.back() == rotate_event ||
           events.types.back() == stop_event) &&
          expected.size() - actual.size() == events.last_event_size &&
          expected.compare(0U, actual.size(), actual) == 0) {
        without_rotate.push_back(name);
        server_binlog_it->size = actual.size();
        pbt::note(tc, "(PBT-033 tolerated: " + name +
                " stored without the server's final ROTATE / STOP)");
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
    const auto actual{storage.plaintext(name)};
    pbt::require(actual == expected, "stored " + name +
                                         " differs from the server's file: " +
                                         first_difference(expected, actual));
    // list: the timestamps of the events the stored binlog holds
    {
      const auto events{walk(actual, "stored " + name)};
      const auto iso_of{[](std::uint32_t value) {
        const auto seconds{static_cast<std::time_t>(value)};
        std::tm parts{};
        ::gmtime_r(&seconds, &parts);
        std::array<char, 32> text{};
        const auto length{std::strftime(text.data(), text.size(),
                                        "%Y-%m-%dT%H:%M:%S", &parts)};
        return std::string{text.data(), length};
      }};
      const auto &item{stored[i].as_object()};
      pbt::require_equal(std::string{item.at("min_timestamp").as_string()},
                         iso_of(events.min_timestamp),
                         "list: min_timestamp of " + name +
                             " (earliest event in the stored file)");
      pbt::require_equal(std::string{item.at("max_timestamp").as_string()},
                         iso_of(events.max_timestamp),
                         "list: max_timestamp of " + name +
                             " (latest event in the stored file)");
    }
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

// every case sets the server's GTID mode it needs (ON unless it tests
// another one) before the reset
void start_case(hegel::TestCase &tc, server_session &server,
                const std::string &gtid_mode = "ON") {
  const auto max_binlog_size{tc.draw(
      "max_binlog_size",
      gs::sampled_from<std::uint64_t>({4096U, 16384U, 1073741824U}))};
  pbt::note(tc, "server " + server.info.version + ", max_binlog_size " +
          std::to_string(max_binlog_size) + ", gtid_mode " + gtid_mode);
  set_gtid_mode(server.sql, server.info, gtid_mode);
  reset_server(server.sql, server.info, max_binlog_size);
}

void wait_ms(std::uint64_t milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds{milliseconds});
}

bool port_accepts_connections(std::uint16_t port) {
  const int fd{::socket(AF_INET, SOCK_STREAM, 0)};
  if (fd < 0) {
    return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const bool connected{::connect(fd, reinterpret_cast<sockaddr *>(&address),
                                 sizeof address) == 0};
  ::close(fd);
  return connected;
}

// 'binlog_server pull' in the background, restartable. binlog_server
// truncates its log when it starts: the logs of earlier runs are kept as
// binsrv.log.<n>.
class pull_process {
public:
  pull_process(hegel::TestCase &tc, const pbs_storage &storage)
      : tc_{tc}, storage_{storage} {
    launch();
  }

  void launch() {
    std::error_code ignored;
    if (fs::exists(storage_.log(), ignored)) {
      fs::rename(storage_.log(),
                 storage_.log().string() + "." + std::to_string(++runs_),
                 ignored);
    }
    process_ = std::make_unique<background_run>(
        std::vector<std::string>{"pull", storage_.config_path});
  }

  void require_running() {
    if (const auto exited{process_->poll()}; exited) {
      pbt::note(tc_, "binlog_server log:" + log_tail(storage_));
      pbt::fail("binlog_server pull exited on its own (" + exited->describe() +
                ")");
    }
  }

  // waits up to 'timeout' for pull to exit on its own (nullopt: still
  // running)
  std::optional<run_result> wait_for_exit(std::chrono::seconds timeout) {
    const auto deadline{std::chrono::steady_clock::now() + timeout};
    while (std::chrono::steady_clock::now() < deadline) {
      if (const auto result{process_->poll()}; result) {
        return result;
      }
      wait_ms(100U);
    }
    return std::nullopt;
  }

  void kill() {
    process_->send(SIGKILL);
    std::ignore = process_->wait();
  }

  // SIGTERM: stops within 20 s with exit code 0. A SIGTERM before the
  // handlers are installed terminates any program, so wait for 'pull' to
  // log that it has them.
  void stop() {
    wait_for_log("set custom handlers for SIGINT and SIGTERM",
                 "pull did not install its signal handlers in 20 s");
    process_->send(SIGTERM);
    const auto [result, in_time]{process_->wait_for(std::chrono::seconds{20})};
    if (!in_time || result.signaled || result.exit_code != 0) {
      pbt::note(tc_, "binlog_server log:" + log_tail(storage_));
    }
    pbt::require(in_time, "pull did not stop within 20 s of SIGTERM");
    pbt::require(!result.signaled && result.exit_code == 0,
                 "pull stopped by SIGTERM with " + result.describe());
  }

  // the replication source listener accepts connections
  void wait_until_listening() {
    const auto deadline{std::chrono::steady_clock::now() +
                        std::chrono::seconds{20}};
    while (!port_accepts_connections(listener_port())) {
      require_running();
      pbt::require(std::chrono::steady_clock::now() < deadline,
                   "the replication source listener did not accept "
                   "connections in 20 s");
      wait_ms(20U);
    }
  }

private:
  void wait_for_log(std::string_view text, const std::string &timeout_message) {
    const auto deadline{std::chrono::steady_clock::now() +
                        std::chrono::seconds{20}};
    while (read_file_if_exists(storage_.log()).find(text) == std::string::npos) {
      require_running();
      pbt::require(std::chrono::steady_clock::now() < deadline, timeout_message);
      wait_ms(10U);
    }
  }

  hegel::TestCase &tc_;
  const pbs_storage &storage_;
  std::size_t runs_{0U};
  std::unique_ptr<background_run> process_;
};

// 'pull' has everything: the server's dump thread for it reports that it
// has sent the whole binary log (seen twice 300 ms apart, then 500 ms for
// PBS to process the last events). Without checkpointing the storage is
// written only when pull stops, so the server's view is the only one.
void wait_until_pull_caught_up(hegel::TestCase &tc, sql_connection &sql,
                               pull_process &pull) {
  const auto deadline{std::chrono::steady_clock::now() +
                      std::chrono::seconds{60}};
  const auto all_sent{[&sql]() {
    return !sql.query("SELECT id FROM information_schema.processlist "
                      "WHERE command LIKE 'Binlog Dump%' AND state LIKE "
                      "'%has sent all binlog%'")
                .empty();
  }};
  std::size_t seen{0U};
  while (seen < 2U) {
    pull.require_running();
    if (std::chrono::steady_clock::now() >= deadline) {
      pbt::note(tc, "(pull did not report catching up in 60 s)");
      break;
    }
    seen = all_sent() ? seen + 1U : 0U;
    wait_ms(300U);
  }
  wait_ms(500U);
}

// ---------------------------------------------------------------------------
// a binlog dump client (libmysqlclient MYSQL_RPL, as PBS itself uses it):
// the events a replica receives, non-blocking (ends at the last event)
// ---------------------------------------------------------------------------
struct dump_result {
  std::vector<std::string> events;  // without the 0x00 packet prefix
  std::string error;                // empty: the dump reached the end
};

dump_result dump_binlog(std::uint16_t port, const std::string &user,
                        const std::string &password, const std::string &file,
                        std::uint64_t position, bool checksum) {
  dump_result result;
  MYSQL *handle{::mysql_init(nullptr)};
  if (handle == nullptr) {
    throw std::runtime_error{"mysql_init failed"};
  }
  const unsigned timeout{10U};
  ::mysql_options(handle, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
  ::mysql_options(handle, MYSQL_OPT_READ_TIMEOUT, &timeout);
  const bool get_public_key{true};
  ::mysql_options(handle, MYSQL_OPT_GET_SERVER_PUBLIC_KEY, &get_public_key);
  ::mysql_options(handle, MYSQL_DEFAULT_AUTH, "caching_sha2_password");
  const auto fail{[&](const std::string &what) {
    result.error = what + ": " + ::mysql_error(handle);
    ::mysql_close(handle);
    return result;
  }};
  if (::mysql_real_connect(handle, "127.0.0.1", user.c_str(), password.c_str(),
                           nullptr, port, nullptr, 0) == nullptr) {
    return fail("connect");
  }
  // the statement PBS's own client sends (easymysql::connection)
  const std::string label{checksum ? "CRC32" : "NONE"};
  const std::string statement{"SET @source_binlog_checksum = '" + label +
                              "', @master_binlog_checksum = '" + label + "'"};
  if (::mysql_real_query(handle, statement.data(), statement.size()) != 0) {
    return fail("SET @source_binlog_checksum");
  }
  constexpr unsigned binlog_dump_non_block{1U};
  MYSQL_RPL rpl{.file_name_length = file.size(),
                .file_name = file.c_str(),
                .start_position = position,
                .server_id = 6161U,
                .flags = MYSQL_RPL_SKIP_HEARTBEAT | binlog_dump_non_block,
                .gtid_set_encoded_size = 0U,
                .fix_gtid_set = nullptr,
                .gtid_set_arg = nullptr,
                .size = 0U,
                .buffer = nullptr};
  if (::mysql_binlog_open(handle, &rpl) != 0) {
    return fail("mysql_binlog_open");
  }
  for (;;) {
    if (::mysql_binlog_fetch(handle, &rpl) != 0) {
      result.error = std::string{"mysql_binlog_fetch: "} + ::mysql_error(handle);
      break;
    }
    if (rpl.size == 0U) {
      break;  // end of the non-blocking dump
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto *bytes{reinterpret_cast<const char *>(rpl.buffer)};
    if (bytes[0] != 0) {
      result.error = "event packet without the 0x00 prefix";
      break;
    }
    result.events.emplace_back(bytes + 1, rpl.size - 1U);
  }
  ::mysql_binlog_close(handle, &rpl);
  ::mysql_close(handle);
  return result;
}

std::string describe_event(const std::string &event) {
  if (event.size() < 19U) {
    return "(" + std::to_string(event.size()) + " bytes)";
  }
  return "type " +
         std::to_string(static_cast<unsigned>(static_cast<unsigned char>(event[4]))) +
         ", size " + std::to_string(event.size()) + ", log_pos " +
         std::to_string(le32(event, 13U)) + ", flags " +
         std::to_string(static_cast<unsigned>(static_cast<unsigned char>(event[17])) |
                        (static_cast<unsigned>(static_cast<unsigned char>(event[18])) << 8U));
}

// ---------------------------------------------------------------------------
// restarting / crashing the server (extra/qa/mysqld.sh, PBT_MYSQLD_CTL)
// ---------------------------------------------------------------------------
void control_server(const std::string &command) {
  const auto script{pbt::detail::env("PBT_MYSQLD_CTL")};
  if (!script) {
    pbt::infrastructure_error(
        "server restarts need PBT_MYSQLD_CTL (the private server of "
        "extra/qa/mysqld.sh, started by run_property_tests.py)");
  }
  std::string shell_command;
  if (const auto workdir{pbt::detail::env("PBT_MYSQLD_WORKDIR")}; workdir) {
    shell_command += "WORKDIR='" + *workdir + "' ";
  }
  shell_command += "'" + *script + "' " + command + " >/dev/null 2>&1";
  // NOLINTNEXTLINE(concurrency-mt-unsafe,cert-env33-c)
  if (std::system(shell_command.c_str()) != 0) {
    pbt::infrastructure_error("extra/qa/mysqld.sh " + command + " failed");
  }
}

} // namespace

// ===========================================================================
// D1: fetch copies the server's binlogs
// ===========================================================================

// After any workload one 'fetch' leaves the storage byte-identical to the
// server's binlog files (position and GTID mode, any checkpointing,
// verify_checksum on and off), and 'list' describes them.
namespace {

// gtid_off: the server runs with GTID_MODE=OFF (anonymous transactions),
// the storage in position mode (GTID mode needs a GTID_MODE=ON server)
void fetch_matches_server(hegel::TestCase &tc, bool s3, bool gtid_off = false) {
        server_session server;
        start_case(tc, server, gtid_off ? "OFF" : "ON");
        auto options{gtid_off ? draw_options(tc, false) : draw_options(tc)};
        options.s3 = s3;
        pbt::note(tc, "binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        pbt::note(tc, "workload:");
        apply_all(tc, server.sql, draw_workload(tc, server.info, 12U));
        fetch(tc, *storage);
        require_storage_matches_server(tc, server.sql, server.info, *storage);
        if (gtid_off) {
          // the setup: only anonymous transactions
          for (const auto &binlog : server_binlogs(server.sql)) {
            const auto events{walk(server_file(server.info, binlog.name),
                                   "server " + binlog.name)};
            pbt::require(std::ranges::none_of(events.types, [](std::uint8_t type) {
                           return type == gtid_event || type == gtid_tagged_event;
                         }),
                         "a GTID_MODE=OFF server wrote a GTID event");
          }
        }
}

} // namespace

BOOST_AUTO_TEST_CASE(FetchMatchesServerBinlogsMySQL) {
  pbt::check([](hegel::TestCase &tc) { fetch_matches_server(tc, false); },
             {.test_cases = 100U, .slow = true});
}

// The same with the S3 storage backend (SeaweedFS).
BOOST_AUTO_TEST_CASE(FetchMatchesServerBinlogsOnS3MySQL) {
  pbt::check([](hegel::TestCase &tc) { fetch_matches_server(tc, true); },
             {.test_cases = 30U, .slow = true});
}

// The same with a GTID_MODE=OFF server (anonymous transactions) and a
// position-mode storage.
BOOST_AUTO_TEST_CASE(FetchMatchesServerBinlogsGtidOffMySQL) {
  pbt::check([](hegel::TestCase &tc) { fetch_matches_server(tc, false, true); },
             {.test_cases = 40U, .slow = true});
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
        pbt::note(tc, "binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        pbt::note(tc, "workload:");
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
namespace {

// gtid_off: the server runs with GTID_MODE=OFF (anonymous transactions),
// the storage in position mode (GTID mode needs a GTID_MODE=ON server)
void repeated_fetch_resumes(hegel::TestCase &tc, bool s3, bool gtid_off = false) {
        server_session server;
        start_case(tc, server, gtid_off ? "OFF" : "ON");
        auto options{gtid_off ? draw_options(tc, false) : draw_options(tc)};
        options.s3 = s3;
        pbt::note(tc, "binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        const auto rounds{tc.draw(
            "rounds", gs::integers<std::size_t>({.min_value = 2U, .max_value = 4U}))};
        for (std::size_t round{0U}; round < rounds; ++round) {
          pbt::note(tc, "round " + std::to_string(round + 1U) + ":");
          apply_all(tc, server.sql, draw_workload(tc, server.info, 5U));
          fetch(tc, *storage);
          require_storage_matches_server(tc, server.sql, server.info, *storage);
        }
}

} // namespace

BOOST_AUTO_TEST_CASE(RepeatedFetchResumesMySQL) {
  pbt::check([](hegel::TestCase &tc) { repeated_fetch_resumes(tc, false); },
             {.test_cases = 60U, .slow = true});
}

// The same with the S3 storage backend (SeaweedFS).
BOOST_AUTO_TEST_CASE(RepeatedFetchResumesOnS3MySQL) {
  pbt::check([](hegel::TestCase &tc) { repeated_fetch_resumes(tc, true); },
             {.test_cases = 20U, .slow = true});
}

// The same with a GTID_MODE=OFF server (anonymous transactions) and a
// position-mode storage.
BOOST_AUTO_TEST_CASE(RepeatedFetchResumesGtidOffMySQL) {
  pbt::check([](hegel::TestCase &tc) { repeated_fetch_resumes(tc, false, true); },
             {.test_cases = 20U, .slow = true});
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
        pbt::note(tc, "binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        pbt::note(tc, "round 1:");
        auto first{draw_workload(tc, server.info, 3U)};
        first.push_back(operation{.kind = op_kind::insert_rows, .count = 1});
        apply_all(tc, server.sql, first);
        fetch(tc, *storage);
        pbt::note(tc, "round 2:");
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
namespace {

// gtid_off: the server runs with GTID_MODE=OFF (anonymous transactions),
// the storage in position mode (GTID mode needs a GTID_MODE=ON server)
void killed_pull_resumes(hegel::TestCase &tc, bool s3, bool gtid_off = false) {
        server_session server;
        start_case(tc, server, gtid_off ? "OFF" : "ON");
        auto options{gtid_off ? draw_options(tc, false) : draw_options(tc)};
        options.s3 = s3;
        pbt::note(tc, "binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        pull_process pull{tc, *storage};
        const auto steps{tc.draw(
            "steps", gs::integers<std::size_t>({.min_value = 1U, .max_value = 8U}))};
        for (std::size_t step{0U}; step < steps; ++step) {
          const auto action{tc.draw(
              "action", gs::sampled_from<std::string>(
                            {"workload", "workload", "sleep", "kill", "stop"}))};
          if (action == "workload") {
            pbt::note(tc, "workload:");
            apply_all(tc, server.sql, draw_workload(tc, server.info, 3U));
          } else if (action == "sleep") {
            const auto delay{tc.draw("sleep_ms", gs::sampled_from<std::uint64_t>(
                                                     {10U, 100U, 400U, 1500U}))};
            pbt::note(tc, "sleep " + std::to_string(delay) + " ms");
            wait_ms(delay);
          } else {
            const auto delay{tc.draw(
                "delay_ms", gs::sampled_from<std::uint64_t>({0U, 5U, 50U, 300U}))};
            wait_ms(delay);
            pull.require_running();
            if (action == "kill") {
              pbt::note(tc, "kill -9 pull after " + std::to_string(delay) + " ms");
              pull.kill();
            } else {
              pbt::note(tc, "SIGTERM pull after " + std::to_string(delay) + " ms");
              pull.stop();
            }
            pull.launch();
          }
        }
        wait_until_pull_caught_up(tc, server.sql, pull);
        pull.require_running();
        pbt::note(tc, "SIGTERM pull, compare, then fetch and compare again");
        pull.stop();
        require_storage_matches_server(tc, server.sql, server.info, *storage);
        fetch(tc, *storage);
        require_storage_matches_server(tc, server.sql, server.info, *storage);
}

} // namespace

BOOST_AUTO_TEST_CASE(KilledPullResumesMySQL) {
  pbt::check([](hegel::TestCase &tc) { killed_pull_resumes(tc, false); },
             {.test_cases = 20U, .slow = true});
}

// The same with the S3 storage backend (SeaweedFS).
BOOST_AUTO_TEST_CASE(KilledPullResumesOnS3MySQL) {
  pbt::check([](hegel::TestCase &tc) { killed_pull_resumes(tc, true); },
             {.test_cases = 20U, .slow = true});
}

// The same with a GTID_MODE=OFF server (anonymous transactions) and a
// position-mode storage.
BOOST_AUTO_TEST_CASE(KilledPullResumesGtidOffMySQL) {
  pbt::check([](hegel::TestCase &tc) { killed_pull_resumes(tc, false, true); },
             {.test_cases = 15U, .slow = true});
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
        pbt::note(tc, "workload:");
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
        pbt::note(tc, "search_by_gtid_set " + query.str());
        const auto result{run({"search_by_gtid_set", storage->config_path, query.str()})};
        pbt::note(tc, "output: " + result.output);
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
        pbt::note(tc, "binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        const auto rounds{tc.draw(
            "rounds", gs::integers<std::size_t>({.min_value = 1U, .max_value = 3U}))};
        for (std::size_t round{0U}; round < rounds; ++round) {
          pbt::note(tc, "round " + std::to_string(round + 1U) + ":");
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
          const auto data{storage->plaintext(name)};
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

// ===========================================================================
// D6: the replication source listener serves the server's events
// ===========================================================================

// After a fetch, 'pull' serves the storage on its replication source
// listener. A binlog dump from the listener (libmysqlclient MYSQL_RPL,
// COM_BINLOG_DUMP, non-blocking) starting at any event of any stored binlog
// receives exactly the events a dump from the MySQL server itself
// receives from the same position, including the artificial ROTATE /
// FORMAT_DESCRIPTION at the start and the ROTATEs between binlogs.
namespace {

void listener_serves_server_events(hegel::TestCase &tc, bool s3) {
        server_session server;
        start_case(tc, server);
        auto options{draw_options(tc)};
        options.s3 = s3;
        pbt::note(tc, "binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        pbt::note(tc, "workload:");
        auto workload{draw_workload(tc, server.info, 10U)};
        // the newest binlog ends with a transaction (PBT-020)
        workload.push_back(operation{.kind = op_kind::insert_rows, .count = 1});
        apply_all(tc, server.sql, workload);
        // that last insert can itself rotate (max_binlog_size): the new
        // binlog would be stored as the magic only (PBT-020), and the
        // listener could not send its preamble; one more small insert
        for (int attempt{0}; attempt < 3; ++attempt) {
          const auto newest{server_binlogs(server.sql).back().name};
          if (walk(server_file(server.info, newest), "server " + newest)
                  .transactions != 0U) {
            break;
          }
          apply_all(tc, server.sql,
                    {operation{.kind = op_kind::insert_rows, .count = 1}});
        }
        fetch(tc, *storage);
        require_storage_matches_server(tc, server.sql, server.info, *storage);

        // a stored binlog and one of its event boundaries
        std::vector<std::string> names;
        const auto stored_listing{list(tc, *storage)};
        for (const auto &item : stored_listing.at("result").as_array()) {
          names.emplace_back(item.at("name").as_string());
        }
        const auto file{names[tc.draw(
            "file", gs::integers<std::size_t>({.max_value = names.size() - 1U}))]};
        const auto offsets{walk(server_file(server.info, file), "server " + file).offsets};
        const auto position{offsets[tc.draw(
            "event", gs::integers<std::size_t>({.max_value = offsets.size() - 1U}))]};
        const auto checksum{tc.draw("checksum", gs::booleans())};
        pbt::note(tc, "dump from " + file + ":" + std::to_string(position) +
                (checksum ? ", CRC32" : ", NONE"));

        pull_process pull{tc, *storage};
        pull.wait_until_listening();
        const auto server_settings_value{settings()};
        const auto direct{dump_binlog(static_cast<std::uint16_t>(server_settings_value.port),
                                      server_settings_value.user,
                                      server_settings_value.password, file,
                                      position, checksum)};
        const auto served{dump_binlog(listener_port(), "replica",
                                      "replica-password", file, position,
                                      checksum)};
        pull.stop();
        pbt::require(direct.error.empty(),
                     "dump from the MySQL server failed: " + direct.error);
        pbt::require(direct.events.size() >= 2U,
                     "the server's dump starts with ROTATE and "
                     "FORMAT_DESCRIPTION");
        pbt::note(tc, "server sent " + std::to_string(direct.events.size()) +
                " events, the listener " + std::to_string(served.events.size()));
        pbt::require(served.error.empty(),
                     "dump from the replication source listener failed: " +
                         served.error);
        const auto common{std::min(direct.events.size(), served.events.size())};
        for (std::size_t i{0U}; i < common; ++i) {
          if (direct.events[i] != served.events[i]) {
            pbt::note(tc, "event " + std::to_string(i) + ": server " +
                    describe_event(direct.events[i]) + "; listener " +
                    describe_event(served.events[i]) + "; " +
                    first_difference(direct.events[i], served.events[i]));
          }
          pbt::require(direct.events[i] == served.events[i],
                       "the listener sent an event that differs from the "
                       "server's");
        }
        pbt::require_equal(served.events.size(), direct.events.size(),
                           "number of events");
}

} // namespace

BOOST_AUTO_TEST_CASE(ListenerServesServerEventsMySQL) {
  pbt::check([](hegel::TestCase &tc) { listener_serves_server_events(tc, false); },
             {.test_cases = 40U, .slow = true});
}

// The same with the S3 storage backend (SeaweedFS).
BOOST_AUTO_TEST_CASE(ListenerServesServerEventsOnS3MySQL) {
  pbt::check([](hegel::TestCase &tc) { listener_serves_server_events(tc, true); },
             {.test_cases = 20U, .slow = true});
}
// A binlog dump request the storage cannot serve - a binlog it does not
// have, a position beyond the end of a binlog, a position inside an event
// - is answered like MySQL answers it: with an error packet (MySQL 1236:
// "Could not find first log file name in binary log index file", "Client
// requested source to start replication from position > file size",
// "binlog truncated in the middle of event"), not with a dropped
// connection and not with an empty, successful dump. A replica stops on
// the error; on a lost connection it retries for ever.
BOOST_AUTO_TEST_CASE(ListenerRejectsInvalidDumpRequestsMySQL) {
  pbt::check(
      [](hegel::TestCase &tc) {
        server_session server;
        start_case(tc, server);
        const auto options{draw_options(tc)};
        pbt::note(tc, "binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        pbt::note(tc, "workload:");
        auto workload{draw_workload(tc, server.info, 4U)};
        workload.push_back(operation{.kind = op_kind::insert_rows, .count = 1});
        apply_all(tc, server.sql, workload);
        fetch(tc, *storage);
        std::vector<std::pair<std::string, std::uint64_t>> stored;
        const auto listing{list(tc, *storage)};
        for (const auto &item : listing.at("result").as_array()) {
          stored.emplace_back(item.at("name").as_string(),
                              item.at("size").to_number<std::uint64_t>());
        }
        const auto index{tc.draw(
            "binlog", gs::integers<std::size_t>({.max_value = stored.size() - 1U}))};
        const auto &[name, size]{stored[index]};
        const auto request{tc.draw(
            "request", gs::sampled_from<std::string>(
                           {"unknown binlog", "beyond the end", "inside an event"}))};
        std::string file{name};
        std::uint64_t position{4U};
        if (request == "unknown binlog") {
          // the next name after the newest stored binlog, or another base
          const auto &newest{stored.back().first};
          const auto number{std::stoul(newest.substr(newest.rfind('.') + 1U))};
          std::string digits{std::to_string(number + 1U)};
          digits.insert(0U, digits.size() < 6U ? 6U - digits.size() : 0U, '0');
          file = tc.draw("other_base", gs::booleans())
                     ? std::string{"nosuch.000001"}
                     : newest.substr(0U, newest.rfind('.') + 1U) + digits;
        } else if (request == "beyond the end") {
          position = size + tc.draw("beyond", gs::sampled_from<std::uint64_t>(
                                                   {1U, 1000U, 1000000U}));
        } else {
          const auto events{walk(storage->plaintext(name), "stored " + name)};
          const auto event{tc.draw(
              "event", gs::integers<std::size_t>({.max_value = events.offsets.size() - 1U}))};
          const auto start{events.offsets[event]};
          const auto end{event + 1U < events.offsets.size()
                             ? events.offsets[event + 1U]
                             : static_cast<std::size_t>(size)};
          position = start + 1U +
                     tc.draw("inside", gs::integers<std::size_t>(
                                           {.max_value = end - start - 2U}));
        }
        pbt::note(tc, "dump " + file + ":" + std::to_string(position) + " (" +
                          request + ")");
        pull_process pull{tc, *storage};
        pull.wait_until_listening();
        const auto served{dump_binlog(listener_port(), "replica",
                                      "replica-password", file, position, true)};
        pbt::note(tc, "listener: " + (served.error.empty() ? std::string{"no error"}
                                                          : served.error) +
                          ", " + std::to_string(served.events.size()) + " events");
        pull.require_running();
        pull.stop();
        // one requirement for both wrong answers (an empty, successful dump
        // or a dropped connection): which of the two a request inside an
        // event gets depends on timing
        pbt::require(!served.error.empty() &&
                         served.error.find("Lost connection") == std::string::npos,
                     "the listener did not answer an invalid dump request "
                     "with an error (" + request + ")");
      },
      {.test_cases = 30U, .slow = true});
}

// ===========================================================================
// D7: a MySQL replica of the replication source listener
// ===========================================================================

namespace {

std::string field(const std::vector<std::pair<std::string, std::string>> &columns,
                  std::string_view name) {
  for (const auto &[key, value] : columns) {
    if (key == name) {
      return value;
    }
  }
  return {};
}

// the second private server (mysqld.sh --replica), reset to an empty
// replica with the test schema
class replica_session {
public:
  explicit replica_session(const std::string &gtid_mode = "ON")
      : sql_{[]() -> std::uint16_t {
          const auto port{pbt::detail::env("PBT_MYSQL_REPLICA_PORT")};
          if (!port) {
            pbt::infrastructure_error(
                "replica server not configured: PBT_MYSQL_REPLICA_PORT "
                "(extra/qa/mysqld.sh --replica start; run_property_tests.py "
                "does this)");
          }
          return static_cast<std::uint16_t>(std::stoul(*port));
        }()} {
    sql_.exec("STOP REPLICA");
    sql_.exec("RESET REPLICA ALL");
    server_info info{inspect(sql_)};
    set_gtid_mode(sql_, info, gtid_mode);
    sql_.exec(info.reset_statement());
    sql_.exec("SET SESSION sql_log_bin = 0");
    sql_.exec("DROP DATABASE IF EXISTS pbt");
    sql_.exec("CREATE DATABASE pbt");
    sql_.exec("CREATE TABLE pbt.t1 (id INT NOT NULL AUTO_INCREMENT PRIMARY "
              "KEY, v INT NOT NULL, b LONGBLOB) ENGINE=InnoDB");
    sql_.exec("SET SESSION sql_log_bin = 1");
  }
  replica_session(const replica_session &) = delete;
  replica_session &operator=(const replica_session &) = delete;
  replica_session(replica_session &&) = delete;
  replica_session &operator=(replica_session &&) = delete;
  ~replica_session() {
    try {
      sql_.exec("STOP REPLICA");
    } catch (const std::exception &) {
    }
  }

  sql_connection &sql() { return sql_; }

private:
  sql_connection sql_;
};

} // namespace

// The README: "real MySQL replicas (position-based ...) can dump binary
// log events from the utility". A MySQL server configured as a
// position-based replica of the listener (from the first binlog) while
// 'pull' runs and the workload continues ends up with the source's
// gtid_executed and the same table contents.
BOOST_AUTO_TEST_CASE(ReplicaOfListenerMatchesSourceMySQL) {
  pbt::check(
      [](hegel::TestCase &tc) {
        server_session server;
        replica_session replica;
        start_case(tc, server);
        // the listener reports GTID_MODE from the storage's mode (README):
        // the replica runs with gtid_mode=ON, and MySQL refuses an ON
        // replica of an OFF source, so the storage is in GTID mode
        // (the replication itself is position-based)
        const auto options{draw_options(tc, true)};
        pbt::note(tc, "binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        pull_process pull{tc, *storage};
        pull.wait_until_listening();
        const auto first{server_binlogs(server.sql).front().name};
        replica.sql().exec(
            "CHANGE REPLICATION SOURCE TO SOURCE_HOST = '127.0.0.1', "
            "SOURCE_PORT = " + std::to_string(listener_port()) +
            ", SOURCE_USER = 'replica', SOURCE_PASSWORD = 'replica-password', "
            "SOURCE_LOG_FILE = '" + first + "', SOURCE_LOG_POS = 4, "
            "GET_SOURCE_PUBLIC_KEY = 1, SOURCE_CONNECT_RETRY = 1");
        replica.sql().exec("START REPLICA");
        const auto require_no_replica_error{[&]() {
          const auto status{replica.sql().first_row_named("SHOW REPLICA STATUS")};
          const auto io_error{field(status, "Last_IO_Error")};
          const auto sql_error{field(status, "Last_SQL_Error")};
          if (!io_error.empty() || !sql_error.empty()) {
            pbt::note(tc, "Last_IO_Error: " + io_error);
            pbt::note(tc, "Last_SQL_Error: " + sql_error);
            pbt::note(tc, "binlog_server log:" + log_tail(*storage));
          }
          pbt::require(io_error.empty(), "replica I/O thread error: " + io_error);
          pbt::require(sql_error.empty(), "replica SQL thread error: " + sql_error);
          return status;
        }};
        // the replica connects and waits for events (checked before any
        // workload: a connection failure does not depend on it)
        {
          const auto deadline{std::chrono::steady_clock::now() +
                              std::chrono::seconds{15}};
          for (;;) {
            const auto status{require_no_replica_error()};
            if (field(status, "Replica_IO_State").find("Waiting for") !=
                    std::string::npos &&
                field(status, "Replica_IO_Running") == "Yes") {
              break;
            }
            pbt::require(std::chrono::steady_clock::now() < deadline,
                         "the replica did not connect to the listener in 15 s");
            wait_ms(50U);
          }
        }
        pbt::note(tc, "workload while replicating:");
        auto workload{draw_workload(tc, server.info, 8U)};
        workload.push_back(operation{.kind = op_kind::insert_rows, .count = 1});
        apply_all(tc, server.sql, workload);

        const auto expected{server_executed(server.sql)};
        const auto deadline{std::chrono::steady_clock::now() +
                            std::chrono::seconds{30}};
        for (;;) {
          std::ignore = require_no_replica_error();
          if (server_executed(replica.sql()) == expected) {
            break;
          }
          pull.require_running();
          pbt::require(std::chrono::steady_clock::now() < deadline,
                       "the replica did not catch up with the source in 30 s");
          wait_ms(100U);
        }
        const auto checksum_of{[](sql_connection &sql) {
          return sql.query("CHECKSUM TABLE pbt.t1").at(0).at(1).value_or("NULL");
        }};
        pbt::require_equal(checksum_of(replica.sql()), checksum_of(server.sql),
                           "CHECKSUM TABLE pbt.t1 on the replica and the source");
        pull.stop();
      },
      {.test_cases = 20U, .slow = true});
}

// ===========================================================================
// D8: the server restarts / crashes while pull runs
// ===========================================================================

// The MySQL server is restarted (graceful shutdown: the binlog ends with
// STOP) or killed with SIGKILL and restarted (crash recovery: the binlog
// ends without STOP / ROTATE) at random points while 'pull' runs. 'pull'
// treats it as a network error, reconnects and never exits; afterwards the
// storage matches the server, also after a 'fetch'.
BOOST_AUTO_TEST_CASE(ServerRestartDuringPullMySQL) {
  pbt::check(
      [](hegel::TestCase &tc) {
        auto server{std::make_unique<server_session>()};
        start_case(tc, *server);
        const auto options{draw_options(tc)};
        pbt::note(tc, "binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        // a crash in a binlog without transactions is PBT-035 (checked by
        // CrashInBinlogWithoutTransactionsMySQL); after a restart the new
        // binlog starts with the account statements of mysqld.sh's
        // init file
        apply_all(tc, server->sql,
                  {operation{.kind = op_kind::insert_rows, .count = 1}});
        pull_process pull{tc, *storage};
        const auto steps{tc.draw(
            "steps", gs::integers<std::size_t>({.min_value = 1U, .max_value = 5U}))};
        for (std::size_t step{0U}; step < steps; ++step) {
          const auto action{tc.draw(
              "action", gs::sampled_from<std::string>(
                            {"workload", "workload", "restart", "crash", "sleep"}))};
          if (action == "workload") {
            pbt::note(tc, "workload:");
            apply_all(tc, server->sql, draw_workload(tc, server->info, 3U));
          } else if (action == "sleep") {
            const auto delay{tc.draw("sleep_ms", gs::sampled_from<std::uint64_t>(
                                                     {50U, 500U, 2500U}))};
            pbt::note(tc, "sleep " + std::to_string(delay) + " ms");
            wait_ms(delay);
          } else {
            pbt::note(tc, action == "restart" ? "restart the server"
                                        : "kill -9 the server and restart it");
            server.reset();
            control_server(action);
            server = std::make_unique<server_session>();
            pull.require_running();
          }
        }
        // reconnects every idle_time (1 s) while the server is down
        wait_until_pull_caught_up(tc, server->sql, pull);
        pull.require_running();
        pbt::note(tc, "SIGTERM pull, compare, then fetch and compare again");
        pull.stop();
        require_storage_matches_server(tc, server->sql, server->info, *storage);
        fetch(tc, *storage);
        require_storage_matches_server(tc, server->sql, server->info, *storage);
      },
      {.test_cases = 12U, .slow = true});
}

// The server is killed while its newest binlog has no transaction yet
// (right after a reset or FLUSH BINARY LOGS), so after crash recovery that
// binlog ends without ROTATE / STOP and the server continues in a new one.
// A fetch then succeeds and the storage (contents, list timestamps)
// matches the server.
BOOST_AUTO_TEST_CASE(CrashInBinlogWithoutTransactionsMySQL) {
  pbt::check(
      [](hegel::TestCase &tc) {
        auto server{std::make_unique<server_session>()};
        start_case(tc, *server);
        const auto options{draw_options(tc)};
        pbt::note(tc, "binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        pbt::note(tc, "workload:");
        auto workload{draw_workload(tc, server->info, 3U)};
        if (!workload.empty()) {
          workload.push_back(operation{.kind = op_kind::flush});
        }
        apply_all(tc, server->sql, workload);
        if (tc.draw("fetch_before_crash", gs::booleans())) {
          pbt::note(tc, "fetch");
          fetch(tc, *storage);
        }
        pbt::note(tc, "kill -9 the server and restart it");
        server.reset();
        control_server("crash");
        server = std::make_unique<server_session>();
        fetch(tc, *storage);
        require_storage_matches_server(tc, server->sql, server->info, *storage);
      },
      {.test_cases = 10U, .slow = true});
}

// ===========================================================================
// D9: PURGE BINARY LOGS on the server
// ===========================================================================

// The server purges binlogs (PURGE BINARY LOGS TO) before / after the
// storage got them. When everything the storage still needs is on the
// server (position mode: its newest binlog; GTID mode: gtid_purged is
// covered by the stored GTIDs; an empty storage starts at the oldest
// binlog / from gtid_purged) the next fetch succeeds and the storage
// matches the server's remaining binlogs, with the binlogs only the
// storage has left unchanged. Otherwise fetch fails (exit code 1, no
// crash) and leaves the storage unchanged.
BOOST_AUTO_TEST_CASE(PurgeOnServerMySQL) {
  pbt::check(
      [](hegel::TestCase &tc) {
        server_session server;
        start_case(tc, server);
        const auto options{draw_options(tc)};
        pbt::note(tc, "binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        pbt::note(tc, "round 1:");
        auto first{draw_workload(tc, server.info, 4U)};
        first.push_back(operation{.kind = op_kind::insert_rows, .count = 1});
        first.push_back(operation{.kind = op_kind::flush});
        apply_all(tc, server.sql, first);
        const auto fetch_before{tc.draw("fetch_before_purge", gs::booleans())};
        if (fetch_before) {
          pbt::note(tc, "fetch");
          fetch(tc, *storage);
        }
        pbt::note(tc, "round 2:");
        auto second{draw_workload(tc, server.info, 4U)};
        second.push_back(operation{.kind = op_kind::flush});
        second.push_back(operation{.kind = op_kind::insert_rows, .count = 1});
        apply_all(tc, server.sql, second);

        const auto before_purge{server_binlogs(server.sql)};
        const auto keep_from{tc.draw(
            "purge_to", gs::integers<std::size_t>({.max_value = before_purge.size() - 1U}))};
        pbt::note(tc, "PURGE BINARY LOGS TO '" + before_purge[keep_from].name + "'");
        server.sql.exec("PURGE BINARY LOGS TO '" + before_purge[keep_from].name + "'");
        const auto remaining{server_binlogs(server.sql)};

        // the storage before the second fetch
        std::vector<std::pair<std::string, std::string>> snapshot;
        gtids::gtid_set stored_gtids;
        const auto listing{list(tc, *storage)};
        for (const auto &item : listing.at("result").as_array()) {
          const std::string name{item.at("name").as_string()};
          snapshot.emplace_back(name, storage->stored(name));
          if (options.gtid_mode) {
            stored_gtids += gtids::gtid_set{std::string_view{item.at("previous_gtids").as_string()}};
            stored_gtids += gtids::gtid_set{std::string_view{item.at("added_gtids").as_string()}};
          }
        }
        const auto on_server{[&](const std::string &name) {
          return std::ranges::any_of(remaining, [&](const server_binlog &binlog) {
            return binlog.name == name;
          });
        }};
        // true: must succeed; false: must fail; nullopt: either
        std::optional<bool> resumable;
        if (snapshot.empty()) {
          resumable = true;
        } else if (options.gtid_mode) {
          const gtids::gtid_set purged{strip_spaces(
              server.sql.scalar("SELECT @@GLOBAL.gtid_purged"))};
          resumable = (purged - stored_gtids).is_empty();
        } else if (on_server(snapshot.back().first)) {
          resumable = true;
        }

        const auto result{run({"fetch", storage->config_path})};
        pbt::note(tc, "fetch: " + result.describe());
        pbt::require(!result.signaled, "fetch crashed (" + result.describe() + ")");
        const bool succeeded{result.exit_code == 0};
        if (resumable.has_value()) {
          if (*resumable && !succeeded) {
            pbt::note(tc, "binlog_server log:" + log_tail(*storage));
          }
          pbt::require_equal(succeeded, *resumable,
                             "fetch succeeds exactly when the server still has "
                             "what the storage needs");
        }
        std::vector<std::string> only_in_storage;
        for (const auto &[name, content] : snapshot) {
          if (!on_server(name)) {
            only_in_storage.push_back(name);
            pbt::require(storage->stored(name) == content,
                         "fetch changed " + name +
                             ", which the server has purged");
          }
        }
        if (succeeded) {
          require_storage_matches_server(tc, server.sql, server.info, *storage,
                                         {}, only_in_storage);
        } else {
          const auto after{list(tc, *storage)};
          const auto count{after.at("result").as_array().size()};
          pbt::require_equal(count, snapshot.size(),
                             "a failed fetch adds no binlog");
          for (const auto &[name, content] : snapshot) {
            pbt::require(storage->stored(name) == content,
                         "a failed fetch changed " + name);
          }
        }
      },
      {.test_cases = 40U, .slow = true});
}

// ===========================================================================
// D12: S3 outages
// ===========================================================================
//
// The expected behaviour (agreed with the project, not in the README yet):
// PBS relies on the AWS SDK's retries and has no application-level loop
// that waits for S3 to come back. After the SDK gives up:
//  - collecting binlogs from MySQL and writing them to S3: the error is
//    logged, the process stops and exits with a failure exit code;
//  - reading S3 during startup: startup fails, exit with failure;
//  - reading binlogs from S3 to serve a replication client: the exception
//    is logged and that client's session is closed; the process keeps
//    running (if the collector does not fail as well) and the client must
//    reconnect.
// In every case the storage stays consistent: once S3 is back, a fetch
// resumes and the storage matches the server.
//
// The outage is a TCP proxy in the test process between PBS and SeaweedFS
// (storage.uri points at the proxy): "down" closes every open connection
// and closes new ones right after accepting them. The oracle reads the
// objects through the SeaweedFS filer, which the proxy does not affect.

namespace {

class tcp_proxy {
public:
  explicit tcp_proxy(std::uint16_t upstream_port) : upstream_port_{upstream_port} {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      throw std::runtime_error{"proxy: socket failed"};
    }
    const int reuse{1};
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0 ||
        ::listen(listen_fd_, 64) != 0) {
      ::close(listen_fd_);
      throw std::runtime_error{"proxy: bind / listen failed"};
    }
    socklen_t length{sizeof address};
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&address), &length);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    port_ = ntohs(address.sin_port);
    acceptor_ = std::thread{[this]() { accept_loop(); }};
  }
  tcp_proxy(const tcp_proxy &) = delete;
  tcp_proxy &operator=(const tcp_proxy &) = delete;
  tcp_proxy(tcp_proxy &&) = delete;
  tcp_proxy &operator=(tcp_proxy &&) = delete;
  ~tcp_proxy() {
    stop_ = true;
    acceptor_.join();
    std::vector<std::thread> pumps;
    {
      const std::scoped_lock lock{mutex_};
      pumps.swap(pumps_);
    }
    for (auto &pump : pumps) {
      pump.join();
    }
    ::close(listen_fd_);
  }

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  void set_down(bool down) { down_ = down; }

private:
  void accept_loop() {
    while (!stop_) {
      pollfd waiting{.fd = listen_fd_, .events = POLLIN, .revents = 0};
      if (::poll(&waiting, 1, 50) <= 0) {
        continue;
      }
      const int client{::accept(listen_fd_, nullptr, nullptr)};
      if (client < 0) {
        continue;
      }
      if (down_) {
        ::close(client);  // the outage: connection closed at once
        continue;
      }
      const std::scoped_lock lock{mutex_};
      pumps_.emplace_back([this, client]() { pump(client); });
    }
  }

  void pump(int client) {
    const int upstream{::socket(AF_INET, SOCK_STREAM, 0)};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(upstream_port_);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (upstream < 0 || ::connect(upstream, reinterpret_cast<sockaddr *>(&address),
                                  sizeof address) != 0) {
      ::close(client);
      if (upstream >= 0) {
        ::close(upstream);
      }
      return;
    }
    std::array<char, 65536> buffer{};
    std::array<pollfd, 2> fds{pollfd{.fd = client, .events = POLLIN, .revents = 0},
                              pollfd{.fd = upstream, .events = POLLIN, .revents = 0}};
    while (!stop_ && !down_) {
      if (::poll(fds.data(), fds.size(), 50) <= 0) {
        continue;
      }
      bool closed{false};
      for (std::size_t i{0U}; i < fds.size() && !closed; ++i) {
        if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
          continue;
        }
        const auto count{::read(fds[i].fd, buffer.data(), buffer.size())};
        if (count <= 0) {
          closed = true;
          break;
        }
        const int target{fds[1U - i].fd};
        std::size_t written{0U};
        while (written < static_cast<std::size_t>(count)) {
          const auto result{::write(target, buffer.data() + written,
                                    static_cast<std::size_t>(count) - written)};
          if (result <= 0) {
            closed = true;
            break;
          }
          written += static_cast<std::size_t>(result);
        }
      }
      if (closed) {
        break;
      }
    }
    ::close(client);
    ::close(upstream);
  }

  std::uint16_t upstream_port_;
  std::uint16_t port_{};
  int listen_fd_{-1};
  std::atomic<bool> stop_{false};
  std::atomic<bool> down_{false};
  std::thread acceptor_;
  std::mutex mutex_;
  std::vector<std::thread> pumps_;
};

// the proxy in front of PBT_S3_ENDPOINT
std::unique_ptr<tcp_proxy> make_s3_proxy() {
  const auto endpoint{pbt::detail::env("PBT_S3_ENDPOINT")};
  if (!endpoint) {
    pbt::infrastructure_error("S3 endpoint not configured (PBT_S3_ENDPOINT)");
  }
  const auto colon{endpoint->rfind(':')};
  if (colon == std::string::npos) {
    pbt::infrastructure_error("PBT_S3_ENDPOINT must be http://host:port");
  }
  return std::make_unique<tcp_proxy>(
      static_cast<std::uint16_t>(std::stoul(endpoint->substr(colon + 1U))));
}

// the SDK gives up within this time (generous for ASan builds)
constexpr std::chrono::seconds s3_give_up_time{180};

} // namespace

// Collecting binlogs and writing them to S3 while S3 is down: pull exits
// on its own with a failure exit code (no crash, no hang); once S3 is back
// a fetch resumes and the storage matches the server.
BOOST_AUTO_TEST_CASE(S3OutageWhileCollectingStopsPullMySQL) {
  pbt::check(
      [](hegel::TestCase &tc) {
        server_session server;
        start_case(tc, server);
        auto options{draw_options(tc)};
        const auto proxy{make_s3_proxy()};
        options.s3 = true;
        options.s3_port = proxy->port();
        // every transaction has to reach S3
        options.checkpoint_size = "1";
        pbt::note(tc, "binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        pbt::note(tc, "workload before the outage:");
        auto before{draw_workload(tc, server.info, 4U)};
        before.push_back(operation{.kind = op_kind::insert_rows, .count = 1});
        apply_all(tc, server.sql, before);
        pull_process pull{tc, *storage};
        wait_until_pull_caught_up(tc, server.sql, pull);
        pull.require_running();

        pbt::note(tc, "S3 down; workload during the outage:");
        proxy->set_down(true);
        auto during{draw_workload(tc, server.info, 3U)};
        during.push_back(operation{.kind = op_kind::insert_rows, .count = 1});
        apply_all(tc, server.sql, during);
        const auto exited{pull.wait_for_exit(s3_give_up_time)};
        if (!exited || exited->signaled || exited->exit_code == 0) {
          pbt::note(tc, "binlog_server log:" + log_tail(*storage));
        }
        pbt::require(exited.has_value(),
                     "pull still running long after S3 became unavailable "
                     "for writing");
        pbt::require(!exited->signaled,
                     "pull crashed after S3 became unavailable (" +
                         exited->describe() + ")");
        pbt::require(exited->exit_code != 0,
                     "pull exited with code 0 after failing to write to S3");

        pbt::note(tc, "S3 up; fetch");
        proxy->set_down(false);
        fetch(tc, *storage);
        require_storage_matches_server(tc, server.sql, server.info, *storage);
      },
      {.test_cases = 6U, .slow = true});
}

// Starting fetch / pull / list while S3 is down: the operation fails with
// a failure exit code (list: a JSON error), no crash, no hang, and the
// storage is unchanged; once S3 is back everything works.
BOOST_AUTO_TEST_CASE(S3OutageAtStartupFailsMySQL) {
  pbt::check(
      [](hegel::TestCase &tc) {
        server_session server;
        start_case(tc, server);
        auto options{draw_options(tc)};
        const auto proxy{make_s3_proxy()};
        options.s3 = true;
        options.s3_port = proxy->port();
        pbt::note(tc, "binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        pbt::note(tc, "workload:");
        auto workload{draw_workload(tc, server.info, 4U)};
        workload.push_back(operation{.kind = op_kind::insert_rows, .count = 1});
        apply_all(tc, server.sql, workload);
        std::vector<std::pair<std::string, std::string>> snapshot;
        if (tc.draw("fetch_before_outage", gs::booleans())) {
          pbt::note(tc, "fetch");
          fetch(tc, *storage);
          const auto listing{list(tc, *storage)};
          for (const auto &item : listing.at("result").as_array()) {
            const std::string name{item.at("name").as_string()};
            snapshot.emplace_back(name, storage->stored(name));
          }
        }
        const auto operation_name{tc.draw(
            "operation", gs::sampled_from<std::string>({"fetch", "pull", "list"}))};
        pbt::note(tc, "S3 down; " + operation_name);
        proxy->set_down(true);
        background_run process{{operation_name, storage->config_path}};
        const auto [result, in_time]{process.wait_for(s3_give_up_time)};
        if (!in_time || result.signaled || result.exit_code == 0) {
          pbt::note(tc, "binlog_server log:" + log_tail(*storage));
        }
        pbt::require(in_time, operation_name +
                                  " did not give up although S3 is unavailable");
        pbt::require(!result.signaled,
                     operation_name + " crashed while S3 is unavailable (" +
                         result.describe() + ")");
        pbt::require(result.exit_code != 0,
                     operation_name + " succeeded while S3 is unavailable");

        pbt::note(tc, "S3 up");
        proxy->set_down(false);
        for (const auto &[name, content] : snapshot) {
          pbt::require(storage->stored(name) == content,
                       "the failed " + operation_name + " changed " + name);
        }
        fetch(tc, *storage);
        require_storage_matches_server(tc, server.sql, server.info, *storage);
      },
      {.test_cases = 6U, .slow = true});
}

// Serving a replication client from S3 while S3 is down (and the collector
// has nothing to write): the client's dump may fail, but pull keeps
// running, and once S3 is back a new dump receives exactly the server's
// events.
BOOST_AUTO_TEST_CASE(S3OutageWhileServingKeepsPullRunningMySQL) {
  pbt::check(
      [](hegel::TestCase &tc) {
        server_session server;
        start_case(tc, server);
        auto options{draw_options(tc)};
        const auto proxy{make_s3_proxy()};
        options.s3 = true;
        options.s3_port = proxy->port();
        // nothing for the collector to write during the outage
        options.checkpoint_size.reset();
        options.checkpoint_interval.reset();
        pbt::note(tc, "binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        pbt::note(tc, "workload:");
        auto workload{draw_workload(tc, server.info, 4U)};
        // the first binlog is closed and only in S3
        workload.push_back(operation{.kind = op_kind::insert_rows, .count = 1});
        workload.push_back(operation{.kind = op_kind::flush});
        workload.push_back(operation{.kind = op_kind::insert_rows, .count = 1});
        apply_all(tc, server.sql, workload);
        fetch(tc, *storage);
        // the oldest stored binlog (GTID auto-positioning may have skipped
        // server binlogs without transactions); with two or more stored it
        // is closed and only in S3
        std::vector<std::string> stored_names;
        const auto listing{list(tc, *storage)};
        for (const auto &item : listing.at("result").as_array()) {
          stored_names.emplace_back(item.at("name").as_string());
        }
        pbt::require(stored_names.size() >= 2U,
                     "the workload ends with a rotation and a transaction");
        const auto first{stored_names.front()};

        pull_process pull{tc, *storage};
        pull.wait_until_listening();
        wait_until_pull_caught_up(tc, server.sql, pull);
        const auto server_settings_value{settings()};
        const auto direct{dump_binlog(
            static_cast<std::uint16_t>(server_settings_value.port),
            server_settings_value.user, server_settings_value.password, first, 4U,
            true)};
        pbt::require(direct.error.empty(),
                     "dump from the MySQL server failed: " + direct.error);

        pbt::note(tc, "S3 down; dump " + first + " from the listener");
        proxy->set_down(true);
        const auto during{dump_binlog(listener_port(), "replica",
                                      "replica-password", first, 4U, true)};
        pbt::note(tc, "dump during the outage: " +
                          (during.error.empty() ? std::string{"no error"}
                                                : during.error) +
                          ", " + std::to_string(during.events.size()) + " events");
        // the first binlog exists only in S3: the session is closed
        pbt::require(!during.error.empty(),
                     "a dump that needs S3 succeeded during the outage");
        wait_ms(2000U);
        pull.require_running();

        // "the client must reconnect": the listener runs its sessions on
        // one thread and S3 reads are synchronous, so until the SDK gives
        // up on the outage session's read every new session waits too
        pbt::note(tc, "S3 up; dump again (until it succeeds)");
        proxy->set_down(false);
        const auto recovery_start{std::chrono::steady_clock::now()};
        auto after{dump_binlog(listener_port(), "replica", "replica-password",
                               first, 4U, true)};
        while (!after.error.empty() &&
               std::chrono::steady_clock::now() - recovery_start < s3_give_up_time) {
          pull.require_running();
          wait_ms(1000U);
          after = dump_binlog(listener_port(), "replica", "replica-password",
                              first, 4U, true);
        }
        pbt::note(tc, "listener usable again after " +
                          std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                             std::chrono::steady_clock::now() - recovery_start)
                                             .count()) +
                          " s");
        pull.require_running();
        pbt::require(after.error.empty(),
                     "dump from the listener after the outage failed: " +
                         after.error);
        pbt::require_equal(after.events.size(), direct.events.size(),
                           "number of events after the outage");
        for (std::size_t i{0U}; i < after.events.size(); ++i) {
          pbt::require(after.events[i] == direct.events[i],
                       "the listener sent an event that differs from the "
                       "server's after the outage");
        }
        pull.stop();
      },
      {.test_cases = 8U, .slow = true});
}

// ===========================================================================
// D14: GTID_MODE other than ON
// ===========================================================================

// The server changes its GTID mode between workload rounds (one step at a
// time through OFF / OFF_PERMISSIVE / ON_PERMISSIVE / ON), so binlogs hold
// GTID and anonymous transactions, also mixed in one binlog. A
// position-mode storage fetched after every round stays a byte-for-byte
// copy of the server's binlogs.
BOOST_AUTO_TEST_CASE(GtidModeTransitionsMySQL) {
  pbt::check(
      [](hegel::TestCase &tc) {
        server_session server;
        static const std::vector<std::string> modes{"OFF", "OFF_PERMISSIVE",
                                                    "ON_PERMISSIVE", "ON"};
        start_case(tc, server, tc.draw("initial_gtid_mode", gs::sampled_from(modes)));
        const auto options{draw_options(tc, false)};
        pbt::note(tc, "binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        const auto rounds{tc.draw(
            "rounds", gs::integers<std::size_t>({.min_value = 2U, .max_value = 5U}))};
        for (std::size_t round{0U}; round < rounds; ++round) {
          const auto mode{tc.draw("gtid_mode", gs::sampled_from(modes))};
          pbt::note(tc, "round " + std::to_string(round + 1U) + ": gtid_mode " + mode);
          set_gtid_mode(server.sql, server.info, mode);
          apply_all(tc, server.sql, draw_workload(tc, server.info, 4U));
          fetch(tc, *storage);
          require_storage_matches_server(tc, server.sql, server.info, *storage);
        }
      },
      {.test_cases = 30U, .slow = true});
}

// A GTID-mode storage needs a GTID_MODE=ON server (the dump uses
// auto-positioning, which MySQL refuses otherwise): against a server in
// another mode fetch and pull exit with a failure exit code (no crash, no
// endless retrying) and leave the storage as it was - also when the
// storage was filled while the server was still ON.
BOOST_AUTO_TEST_CASE(GtidStorageNeedsGtidOnServerMySQL) {
  pbt::check(
      [](hegel::TestCase &tc) {
        server_session server;
        start_case(tc, server);
        const auto options{draw_options(tc, true)};
        pbt::note(tc, "binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        std::vector<std::pair<std::string, std::string>> snapshot;
        if (tc.draw("fetch_while_on", gs::booleans())) {
          pbt::note(tc, "workload and fetch while gtid_mode is ON:");
          auto workload{draw_workload(tc, server.info, 4U)};
          workload.push_back(operation{.kind = op_kind::insert_rows, .count = 1});
          apply_all(tc, server.sql, workload);
          fetch(tc, *storage);
        }
        const auto listing{list(tc, *storage)};
        for (const auto &item : listing.at("result").as_array()) {
          const std::string name{item.at("name").as_string()};
          snapshot.emplace_back(name, storage->stored(name));
        }
        const auto mode{tc.draw("gtid_mode", gs::sampled_from<std::string>(
                                                 {"ON_PERMISSIVE", "OFF_PERMISSIVE", "OFF"}))};
        pbt::note(tc, "gtid_mode " + mode + ", workload:");
        set_gtid_mode(server.sql, server.info, mode);
        auto workload{draw_workload(tc, server.info, 3U)};
        workload.push_back(operation{.kind = op_kind::insert_rows, .count = 1});
        apply_all(tc, server.sql, workload);
        const auto operation_name{tc.draw(
            "operation", gs::sampled_from<std::string>({"fetch", "pull"}))};
        pbt::note(tc, operation_name);
        background_run process{{operation_name, storage->config_path}};
        const auto [result, in_time]{process.wait_for(std::chrono::seconds{60})};
        if (!in_time || result.signaled || result.exit_code == 0) {
          pbt::note(tc, "binlog_server log:" + log_tail(*storage));
        }
        pbt::require(in_time, operation_name + " in GTID mode keeps running "
                                               "against a server whose "
                                               "gtid_mode is not ON");
        pbt::require(!result.signaled, operation_name + " crashed (" +
                                           result.describe() + ")");
        pbt::require(result.exit_code != 0,
                     operation_name + " in GTID mode succeeded against a "
                                      "server whose gtid_mode is not ON");
        std::size_t count{0U};
        const auto after{list(tc, *storage)};
        count = after.at("result").as_array().size();
        pbt::require_equal(count, snapshot.size(), "the failed " + operation_name +
                                                       " adds no binlog");
        for (const auto &[name, content] : snapshot) {
          pbt::require(storage->stored(name) == content,
                       "the failed " + operation_name + " changed " + name);
        }
      },
      {.test_cases = 15U, .slow = true});
}

// The README: real MySQL replicas (position-based) can replicate from the
// listener, which reports GTID_MODE from the storage's mode. Source, a
// position-mode storage and the replica all with GTID_MODE=OFF: the
// replica (from the first binlog) catches up with the source's binlog
// position while pull runs and the workload continues, with the same table
// contents.
BOOST_AUTO_TEST_CASE(ReplicaOfPositionStorageGtidOffMySQL) {
  pbt::check(
      [](hegel::TestCase &tc) {
        server_session server;
        replica_session replica{"OFF"};
        start_case(tc, server, "OFF");
        const auto options{draw_options(tc, false)};
        pbt::note(tc, "binlog_server: " + describe(options));
        const auto storage{make_storage(options)};
        pull_process pull{tc, *storage};
        pull.wait_until_listening();
        const auto first{server_binlogs(server.sql).front().name};
        replica.sql().exec(
            "CHANGE REPLICATION SOURCE TO SOURCE_HOST = '127.0.0.1', "
            "SOURCE_PORT = " + std::to_string(listener_port()) +
            ", SOURCE_USER = 'replica', SOURCE_PASSWORD = 'replica-password', "
            "SOURCE_LOG_FILE = '" + first + "', SOURCE_LOG_POS = 4, "
            "GET_SOURCE_PUBLIC_KEY = 1, SOURCE_CONNECT_RETRY = 1");
        replica.sql().exec("START REPLICA");
        pbt::note(tc, "workload while replicating:");
        auto workload{draw_workload(tc, server.info, 8U)};
        workload.push_back(operation{.kind = op_kind::insert_rows, .count = 1});
        apply_all(tc, server.sql, workload);

        const auto status_row{server.sql.first_row_named(server.info.binlog_status_statement())};
        const auto source_file{field(status_row, "File")};
        const auto source_position{field(status_row, "Position")};
        pbt::note(tc, "source at " + source_file + ":" + source_position);
        const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{30}};
        for (;;) {
          const auto status{replica.sql().first_row_named("SHOW REPLICA STATUS")};
          const auto io_error{field(status, "Last_IO_Error")};
          const auto sql_error{field(status, "Last_SQL_Error")};
          if (!io_error.empty() || !sql_error.empty()) {
            pbt::note(tc, "Last_IO_Error: " + io_error);
            pbt::note(tc, "Last_SQL_Error: " + sql_error);
            pbt::note(tc, "binlog_server log:" + log_tail(*storage));
          }
          pbt::require(io_error.empty(), "replica I/O thread error: " + io_error);
          pbt::require(sql_error.empty(), "replica SQL thread error: " + sql_error);
          if (field(status, "Relay_Source_Log_File") == source_file &&
              field(status, "Exec_Source_Log_Pos") == source_position) {
            break;
          }
          pull.require_running();
          if (std::chrono::steady_clock::now() >= deadline) {
            pbt::note(tc, "replica at " + field(status, "Relay_Source_Log_File") + ":" +
                              field(status, "Exec_Source_Log_Pos"));
          }
          pbt::require(std::chrono::steady_clock::now() < deadline,
                       "the replica did not catch up with the source in 30 s");
          wait_ms(100U);
        }
        const auto checksum_of{[](sql_connection &sql) {
          return sql.query("CHECKSUM TABLE pbt.t1").at(0).at(1).value_or("NULL");
        }};
        pbt::require_equal(checksum_of(replica.sql()), checksum_of(server.sql),
                           "CHECKSUM TABLE pbt.t1 on the replica and the source");
        pull.stop();
      },
      {.test_cases = 15U, .slow = true});
}
