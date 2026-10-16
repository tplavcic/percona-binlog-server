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

// Property-based tests for the JSON configuration file
// (src/binsrv/main_config.cpp and the *_config validate() functions),
// against the "JSON Configuration file" reference in README.md.
//
// Properties:
//  - every configuration the README describes loads, and the values read
//    back are the ones written;
//  - a configuration with a single documented rule broken (missing
//    required key, wrong JSON type, number out of range, unknown enum
//    value, host/port vs dns_srv_name, replication_source limits, rewrite
//    rules, encryption rules, malformed size / time values) is rejected
//    when it is loaded, with an exception;
//  - a misspelled key is not silently ignored;
//  - storage_config::get_masked_uri(), used when the configuration is
//    logged, never contains the S3 secret access key.

#define BOOST_TEST_MODULE ConfigPropertyTests

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include "binsrv/log_severity.hpp"
#include "binsrv/main_config.hpp"
#include "binsrv/replication_mode_type.hpp"
#include "binsrv/storage_backend_type.hpp"
#include "binsrv/storage_config.hpp"

#include "easymysql/ssl_mode_type.hpp"

#include "common/pbt_helpers.hpp"

namespace {

namespace gs = hegel::generators;
namespace fs = std::filesystem;
namespace json = boost::json;

class temporary_file {
public:
  temporary_file() {
    std::string pattern{
        (fs::temp_directory_path() / "pbs-pbt-config-XXXXXX").string()};
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const int descriptor{::mkstemp(std::data(pattern))};
    if (descriptor < 0) {
      throw std::runtime_error{"mkstemp failed"};
    }
    ::close(descriptor);
    path_ = pattern;
  }
  temporary_file(const temporary_file &) = delete;
  temporary_file &operator=(const temporary_file &) = delete;
  temporary_file(temporary_file &&) = delete;
  temporary_file &operator=(temporary_file &&) = delete;
  ~temporary_file() {
    std::error_code ignored;
    fs::remove(path_, ignored);
  }
  [[nodiscard]] const fs::path &path() const noexcept { return path_; }

private:
  fs::path path_;
};

std::unique_ptr<binsrv::main_config> load(const json::object &config,
                                          hegel::TestCase &tc) {
  const auto text{json::serialize(config)};
  tc.note("config = " + text);
  const temporary_file file;
  {
    std::ofstream output{file.path()};
    output << text;
  }
  return std::make_unique<binsrv::main_config>(file.path().string());
}

// ---------------------------------------------------------------------------
// valid configurations (README "JSON Configuration file")
// ---------------------------------------------------------------------------
gs::Generator<std::string> names() {
  return gs::from_regex("[a-z][a-z0-9_]{0,10}", true);
}

json::object valid_config(hegel::TestCase &tc) {
  const auto draw_u32{[&tc](std::uint32_t min_value) {
    return tc.draw(gs::integers<std::uint32_t>({.min_value = min_value}));
  }};
  json::object logger;
  logger["level"] = tc.draw(gs::sampled_from<std::string>(
      {"trace", "debug", "info", "warning", "error", "fatal"}));
  logger["file"] = tc.draw(gs::sampled_from<std::string>(
      {"", "binsrv.log", "/var/log/pbs/binsrv.log"}));

  json::object connection;
  if (tc.draw(gs::booleans())) {
    connection["host"] = tc.draw(gs::sampled_from<std::string>(
        {"127.0.0.1", "192.168.0.100", "dbsrv.mydomain.com"}));
    connection["port"] = tc.draw(gs::integers<std::uint16_t>());
  } else {
    connection["dns_srv_name"] = "_mysql._tcp.example.com";
  }
  connection["user"] = tc.draw(names());
  connection["password"] = tc.draw(gs::sampled_from<std::string>({"", "secret"}));
  connection["connect_timeout"] = draw_u32(0U);
  connection["read_timeout"] = draw_u32(0U);
  connection["write_timeout"] = draw_u32(0U);
  if (tc.draw(gs::booleans())) {
    json::object ssl;
    ssl["mode"] = tc.draw(gs::sampled_from<std::string>(
        {"disabled", "preferred", "required", "verify_ca", "verify_identity"}));
    for (const auto *key :
         {"ca", "capath", "crl", "crlpath", "cert", "key", "cipher"}) {
      if (tc.draw(gs::booleans())) {
        ssl[key] = std::string{"/etc/mysql/"} + key;
      }
    }
    connection["ssl"] = ssl;
  }
  if (tc.draw(gs::booleans())) {
    json::object tls;
    if (tc.draw(gs::booleans())) {
      tls["ciphersuites"] = "TLS_AES_256_GCM_SHA384";
    }
    if (tc.draw(gs::booleans())) {
      tls["version"] = "TLSv1.3";
    }
    connection["tls"] = tls;
  }

  json::object replication;
  replication["server_id"] = draw_u32(0U);
  replication["idle_time"] = draw_u32(0U);
  replication["verify_checksum"] = tc.draw(gs::booleans());
  const bool gtid_mode{tc.draw(gs::booleans())};
  replication["mode"] = gtid_mode ? "gtid" : "position";
  if (gtid_mode && tc.draw(gs::booleans())) {
    json::object rewrite;
    rewrite["base_file_name"] = tc.draw(names());
    rewrite["file_size"] = tc.draw(gs::sampled_from<std::string>(
        {"1024", "1K", "2048", "128M", "1G", "3T"}));
    replication["rewrite"] = rewrite;
  }

  json::object authentication;
  authentication["user"] = tc.draw(names());
  authentication["password"] = "rpl_password";
  authentication["plugin"] = "caching_sha2_password";
  json::object replication_source;
  replication_source["port"] =
      tc.draw(gs::integers<std::uint16_t>({.min_value = 1U}));
  replication_source["read_timeout"] = draw_u32(1U);
  replication_source["write_timeout"] = draw_u32(1U);
  replication_source["authentication"] = authentication;

  json::object storage;
  if (tc.draw(gs::booleans())) {
    storage["backend"] = "file";
    storage["uri"] = "file:///home/user/vault";
  } else {
    storage["backend"] = "s3";
    storage["uri"] = tc.draw(gs::sampled_from<std::string>(
        {"s3://binsrv-bucket/vault", "s3://key_id:secret@binsrv-bucket.us-east-1/vault",
         "http://key_id:secret@localhost:9000/binsrv-bucket/vault"}));
    if (tc.draw(gs::booleans())) {
      storage["fs_buffer_directory"] = "/tmp/binsrv";
    }
  }
  if (tc.draw(gs::booleans())) {
    storage["checkpoint_size"] =
        tc.draw(gs::sampled_from<std::string>({"0", "42", "2M", "1G"}));
  }
  if (tc.draw(gs::booleans())) {
    storage["checkpoint_interval"] =
        tc.draw(gs::sampled_from<std::string>({"0", "30", "30s", "5m", "1h", "2d"}));
  }

  json::object root;
  root["logger"] = logger;
  root["connection"] = connection;
  root["replication"] = replication;
  root["replication_source"] = replication_source;
  if (tc.draw(gs::booleans())) {
    json::object encryption;
    encryption["format"] = "generic";
    encryption["kek_id"] = "alpha";
    encryption["cipher"] = tc.draw(gs::sampled_from<std::string>(
        {"AES-128-CTR", "AES-192-CTR", "AES-256-CTR", "ARIA-256-CTR"}));
    storage["encryption"] = encryption;
    json::object keyring;
    keyring["uri"] = "file:///var/lib/pbs/keyring/keyring_data.json";
    root["keyring"] = keyring;
  }
  root["storage"] = storage;
  return root;
}

// ---------------------------------------------------------------------------
// paths into the JSON object
// ---------------------------------------------------------------------------
json::object *parent_of(json::object &root, const std::vector<std::string> &path) {
  json::object *current{&root};
  for (std::size_t i{0U}; i + 1U < path.size(); ++i) {
    auto *next{current->if_contains(path[i])};
    if (next == nullptr || !next->is_object()) {
      return nullptr;
    }
    current = &next->as_object();
  }
  return current;
}

std::string join(const std::vector<std::string> &path) {
  std::string result;
  for (const auto &item : path) {
    result += (result.empty() ? "" : ".") + item;
  }
  return result;
}

// every path that exists in the configuration
void collect_paths(const json::object &object, std::vector<std::string> prefix,
                   std::vector<std::vector<std::string>> &result) {
  for (const auto &[key, value] : object) {
    auto path{prefix};
    path.emplace_back(key);
    result.push_back(path);
    if (value.is_object()) {
      collect_paths(value.as_object(), path, result);
    }
  }
}

bool is_required(const std::vector<std::string> &path) {
  // README: optional are connection.ssl / .tls (and every key in them except
  // ssl.mode), host / port / dns_srv_name (one form required), keyring,
  // replication.rewrite, storage.fs_buffer_directory / checkpoint_size /
  // checkpoint_interval / encryption
  static const std::vector<std::string> optional{
      "connection.ssl",          "connection.ssl.ca",
      "connection.ssl.capath",   "connection.ssl.crl",
      "connection.ssl.crlpath",  "connection.ssl.cert",
      "connection.ssl.key",      "connection.ssl.cipher",
      "connection.tls",          "connection.tls.ciphersuites",
      "connection.tls.version",  "keyring",
      "replication.rewrite",     "storage.fs_buffer_directory",
      "storage.checkpoint_size", "storage.checkpoint_interval",
      "storage.encryption"};
  const auto name{join(path)};
  if (name == "keyring.uri") {
    return true;
  }
  return std::ranges::find(optional, name) == optional.end();
}

struct violation {
  std::string description;
  std::vector<std::string> path;
  // value to set; null with remove=true removes the key
  json::value value;
  bool remove{false};
};

std::vector<violation> applicable_violations(const json::object &config) {
  std::vector<violation> result;
  std::vector<std::vector<std::string>> paths;
  collect_paths(config, {}, paths);
  const auto has{[&config](const std::vector<std::string> &path) {
    const json::value *current{nullptr};
    const json::object *object{&config};
    for (const auto &item : path) {
      if (object == nullptr) {
        return false;
      }
      current = object->if_contains(item);
      if (current == nullptr) {
        return false;
      }
      object = current->is_object() ? &current->as_object() : nullptr;
    }
    return true;
  }};
  const auto add{[&result](std::string description,
                           std::vector<std::string> path, json::value value,
                           bool remove = false) {
    result.push_back({std::move(description), std::move(path), std::move(value),
                      remove});
  }};

  for (const auto &path : paths) {
    if (is_required(path) && path.back() != "host" && path.back() != "port" &&
        path.back() != "dns_srv_name") {
      add("missing required key", path, nullptr, true);
    }
  }
  // wrong JSON types
  for (const auto &path : paths) {
    const auto name{join(path)};
    for (const auto *numeric :
         {"connection.port", "connection.connect_timeout",
          "connection.read_timeout", "connection.write_timeout",
          "replication.server_id", "replication.idle_time",
          "replication_source.port", "replication_source.read_timeout",
          "replication_source.write_timeout"}) {
      if (name == numeric) {
        add("number given as a string", path, "42");
        add("negative number", path, -1);
        add("fractional number", path, 1.5);
        add("number above the type's range", path,
            name.ends_with("port") ? std::int64_t{65536}
                                   : std::int64_t{4294967296LL});
      }
    }
    if (name == "replication.verify_checksum") {
      add("boolean given as a string", path, "true");
      add("boolean given as a number", path, 1);
    }
    for (const auto *text :
         {"logger.file", "connection.user", "connection.password",
          "storage.uri", "replication_source.authentication.user"}) {
      if (name == text) {
        add("string given as a number", path, 42);
      }
    }
  }
  // unknown enum values
  const std::vector<std::pair<std::string, std::vector<std::string>>> enums{
      {"logger.level", {"verbose", "", "information", "warn"}},
      {"replication.mode", {"gtids", "", "row", "positional"}},
      {"storage.backend", {"s4", "", "filesystem", "minio"}},
      {"connection.ssl.mode", {"enabled", "", "verify"}},
      {"storage.encryption.format", {"custom", ""}}};
  for (const auto &[name, values] : enums) {
    for (const auto &path : paths) {
      if (join(path) == name) {
        for (const auto &value : values) {
          add("unknown enum value", path, json::value(value));
        }
      }
    }
  }
  // host / port vs dns_srv_name
  if (has({"connection", "dns_srv_name"})) {
    add("dns_srv_name together with host", {"connection", "host"}, "127.0.0.1");
    add("dns_srv_name together with port", {"connection", "port"}, 3306);
    add("neither dns_srv_name nor host / port", {"connection", "dns_srv_name"},
        nullptr, true);
  } else {
    add("host without port", {"connection", "port"}, nullptr, true);
    add("port without host", {"connection", "host"}, nullptr, true);
    add("host / port together with dns_srv_name",
        {"connection", "dns_srv_name"}, "_mysql._tcp.example.com");
  }
  // replication_source
  add("replication_source.port 0", {"replication_source", "port"}, 0);
  add("replication_source.read_timeout 0",
      {"replication_source", "read_timeout"}, 0);
  add("replication_source.write_timeout 0",
      {"replication_source", "write_timeout"}, 0);
  add("empty authentication.user",
      {"replication_source", "authentication", "user"}, "");
  add("empty authentication.password",
      {"replication_source", "authentication", "password"}, "");
  for (const auto *plugin : {"mysql_native_password", "", "sha256_password",
                             "caching_sha2_password "}) {
    add("unsupported authentication.plugin",
        {"replication_source", "authentication", "plugin"},
        json::value(plugin));
  }
  // rewrite
  if (has({"replication", "rewrite"})) {
    add("rewrite in position mode", {"replication", "mode"}, "position");
    for (const auto *size : {"0", "1", "1000", "1023"}) {
      add("rewrite.file_size below 1024 bytes",
          {"replication", "rewrite", "file_size"}, json::value(size));
    }
  }
  // storage values
  for (const auto *size : {"10KB", "-1", "1.5M", "M", "k"}) {
    add("malformed checkpoint_size", {"storage", "checkpoint_size"},
        json::value(size));
  }
  for (const auto *interval : {"5x", "-1", "1.5h", "s", "10 m"}) {
    add("malformed checkpoint_interval", {"storage", "checkpoint_interval"},
        json::value(interval));
  }
  if (has({"storage", "encryption"})) {
    for (const auto *cipher :
         {"AES-256-GCM", "AES-128-CBC", "AES-256-ECB", "NOPE-128-CTR"}) {
      add("non-CTR data cipher", {"storage", "encryption", "cipher"},
          json::value(cipher));
    }
  }
  return result;
}

void apply(json::object &config, const violation &chosen) {
  auto *parent{parent_of(config, chosen.path)};
  if (parent == nullptr) {
    // create the missing parent objects
    json::object *current{&config};
    for (std::size_t i{0U}; i + 1U < chosen.path.size(); ++i) {
      auto &next{(*current)[chosen.path[i]]};
      if (!next.is_object()) {
        next = json::object{};
      }
      current = &next.as_object();
    }
    parent = current;
  }
  if (chosen.remove) {
    parent->erase(chosen.path.back());
  } else {
    (*parent)[chosen.path.back()] = chosen.value;
  }
}

} // namespace

// Every configuration described in the README loads; the values read back
// equal the ones written.
BOOST_AUTO_TEST_CASE(ValidConfigurationLoads) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto config{valid_config(tc)};
        std::unique_ptr<binsrv::main_config> loaded;
        try {
          loaded = load(config, tc);
        } catch (const std::exception &e) {
          pbt::fail(std::string{"valid configuration rejected: "} + e.what());
        }
        const auto &root{loaded->root()};
        pbt::require_equal(
            std::string{binsrv::to_string_view(
                root.get<"logger">().get<"level">())},
            std::string{config.at("logger").at("level").as_string()},
            "logger.level");
        pbt::require_equal(
            root.get<"replication">().get<"server_id">(),
            config.at("replication").at("server_id").to_number<std::uint32_t>(),
            "replication.server_id");
        pbt::require_equal(
            root.get<"replication">().get<"mode">() ==
                binsrv::replication_mode_type::gtid,
            config.at("replication").at("mode").as_string() == "gtid",
            "replication.mode");
        pbt::require_equal(
            root.get<"storage">().get<"uri">(),
            std::string{config.at("storage").at("uri").as_string()},
            "storage.uri");
        pbt::require_equal(
            root.get<"replication_source">().get<"port">(),
            config.at("replication_source").at("port").to_number<std::uint16_t>(),
            "replication_source.port");
        pbt::require_equal(root.get<"storage">().has_encryption(),
                           config.at("storage").as_object().contains("encryption"),
                           "storage.encryption present");
      },
      {.test_cases = 300U});
}

// A configuration with exactly one documented rule broken is rejected when
// it is loaded (main_config constructor), not later when the value is used.
BOOST_AUTO_TEST_CASE(InvalidConfigurationIsRejected) {
  pbt::check(
      [](hegel::TestCase &tc) {
        auto config{valid_config(tc)};
        const auto candidates{applicable_violations(config)};
        std::vector<std::string> kinds;
        for (const auto &candidate : candidates) {
          if (std::ranges::find(kinds, candidate.description) == kinds.end()) {
            kinds.push_back(candidate.description);
          }
        }
        const auto kind{kinds[tc.draw(
            "violation_kind",
            gs::integers<std::size_t>({.max_value = kinds.size() - 1U}))]};
        std::vector<violation> of_kind;
        for (const auto &candidate : candidates) {
          if (candidate.description == kind) {
            of_kind.push_back(candidate);
          }
        }
        const auto chosen{of_kind[tc.draw(
            "violation",
            gs::integers<std::size_t>({.max_value = of_kind.size() - 1U}))]};
        apply(config, chosen);
        tc.note("violation: " + chosen.description + " at " +
                join(chosen.path) +
                (chosen.remove ? " (removed)"
                               : " = " + json::serialize(chosen.value)));
        pbt::require_throws<std::exception>(
            [&] { std::ignore = load(config, tc); },
            "invalid configuration must be rejected when it is loaded");
      },
      {.test_cases = 1000U});
}

// A misspelled key (e.g. "encrypton" for the optional "encryption" section,
// or "checkpoint_sise") is reported instead of being ignored: otherwise the
// option silently keeps its default - for "encryption" that means binlogs
// stored in plaintext although the configuration asks for encryption.
BOOST_AUTO_TEST_CASE(MisspelledKeyIsRejected) {
  pbt::check(
      [](hegel::TestCase &tc) {
        auto config{valid_config(tc)};
        std::vector<std::vector<std::string>> paths;
        collect_paths(config, {}, paths);
        const auto path{paths[tc.draw(
            "key", gs::integers<std::size_t>({.max_value = paths.size() - 1U}))]};
        auto *parent{parent_of(config, path)};
        const auto value{parent->at(path.back())};
        parent->erase(path.back());
        const auto misspelling{tc.draw(
            "misspelling", gs::integers<int>({.min_value = 0, .max_value = 2}))};
        auto key{path.back()};
        if (misspelling == 0) {
          key += "s";
        } else if (misspelling == 1) {
          key = key.substr(1U);
        } else {
          std::ranges::transform(key, key.begin(), [](char c) {
            return c == '_' ? '-' : static_cast<char>(std::toupper(c));
          });
        }
        (*parent)[key] = value;
        tc.note("renamed " + join(path) + " to '" + key + "'");
        pbt::require_throws<std::exception>(
            [&] { std::ignore = load(config, tc); },
            "configuration with an unknown (misspelled) key must be rejected");
      },
      {.test_cases = 300U});
}

// get_masked_uri() (logged at startup, see logger_helpers.cpp) hides the
// credentials of every documented S3 URI form, also secrets with the
// characters AWS secret keys contain (README: '+' as is, '/' as %2F).
BOOST_AUTO_TEST_CASE(MaskedStorageUriHidesTheSecret) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto key_id{tc.draw("key_id", gs::from_regex("[A-Z0-9]{16,20}", true))};
        const auto secret{
            tc.draw("secret", gs::from_regex("[A-Za-z0-9+/]{30,40}", true))};
        std::string encoded_secret;
        for (const char c : secret) {
          encoded_secret += (c == '/') ? std::string{"%2F"} : std::string(1U, c);
        }
        const auto form{tc.draw("form", gs::integers<int>({.min_value = 0,
                                                           .max_value = 3}))};
        std::string uri;
        switch (form) {
        case 0:
          uri = "s3://" + key_id + ":" + encoded_secret + "@binsrv-bucket/vault";
          break;
        case 1:
          uri = "s3://" + key_id + ":" + encoded_secret +
                "@binsrv-bucket.us-east-1/vault/sub";
          break;
        case 2:
          uri = "http://" + key_id + ":" + encoded_secret +
                "@localhost:9000/binsrv-bucket/vault";
          break;
        default:
          uri = "https://" + key_id + ":" + encoded_secret +
                "@192.168.0.100:9000/binsrv-bucket/vault";
          break;
        }
        tc.note("uri = " + uri);
        binsrv::storage_config storage{};
        storage.get<"backend">() = binsrv::storage_backend_type::s3;
        storage.get<"uri">() = uri;
        const auto masked{storage.get_masked_uri()};
        tc.note("masked = " + masked);
        pbt::require(masked.find(secret) == std::string::npos &&
                         masked.find(encoded_secret) == std::string::npos,
                     "masked URI contains the secret access key");
        pbt::require(masked.find(key_id) == std::string::npos,
                     "masked URI contains the access key id");
        pbt::require(masked.find("binsrv-bucket") != std::string::npos,
                     "masked URI keeps the bucket");
      },
      {.test_cases = 500U});
}

// replication.rewrite.base_file_name becomes the base of every generated
// binlog name (README: "rewritten_binlog" -> "rewritten_binlog.000001"),
// so a value that composite_binlog_name rejects (empty, containing '/')
// must be rejected when the configuration is loaded. Otherwise the
// collector starts, connects and fails at the first transaction.
BOOST_AUTO_TEST_CASE(RewriteBaseFileNameIsValidatedOnLoad) {
  pbt::check(
      [](hegel::TestCase &tc) {
        auto config{valid_config(tc)};
        auto &replication{config["replication"].as_object()};
        replication["mode"] = "gtid";
        json::object rewrite;
        rewrite["base_file_name"] = tc.draw(
            "base_file_name",
            gs::sampled_from<std::string>({"", "a/b", "/abs", "dir/"}));
        rewrite["file_size"] = "1M";
        replication["rewrite"] = rewrite;
        pbt::require_throws<std::exception>(
            [&] { std::ignore = load(config, tc); },
            "invalid rewrite.base_file_name must be rejected when the "
            "configuration is loaded");
      },
      {.test_cases = 50U});
}
