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

// Property-based tests for binlog storage (src/binsrv/storage.cpp,
// storage_core.cpp, filesystem_storage_backend.cpp), on the local
// filesystem backend in a temporary directory.
//
// The collector writes every received event with storage::write_event()
// (together with "at transaction boundary", the transaction GTID, the
// event timestamp and the sequence number), flushes at checkpoints and on
// close, discards incomplete transactions when the connection times out
// and is restarted at any time. Properties:
//  - a stateful model: after every operation of a random sequence (open
//    new / current binlog, write, flush, discard, close, restart) the
//    storage reports the position, GTIDs, sequence number and binlog
//    records of the model, and after every restart the files on disk and
//    the data read back hold exactly the flushed transactions (never part
//    of a transaction, never a lost complete one);
//  - fetch_event_block() and purge_binlogs() follow their documented
//    contracts (storage_core.hpp);
//  - recovery: unrecorded bytes at the end of the last binlog are
//    truncated, other size mismatches are rejected, a crash while a binlog
//    is created does not make the storage unusable, querying_only mode
//    skips broken metadata;
//  - indexed_event_block splits blocks into events and terminates on any
//    (damaged) data;
//  - encryption at rest: the model with every KEK / data cipher
//    combination, independently decrypted on-disk files, the documented
//    cipher restrictions, switching encryption on, the KEK being
//    required, a storage started without its encryption section not
//    writing plaintext;
//  - the model on the S3 backend (SeaweedFS, PBT_S3_* environment);
//  - a long checkpoint_interval does not flush early.
//
// Built against pbs_test_binsrv (see tests/property/CMakeLists.txt and
// extra/qa/todo.md T1).

#define BOOST_TEST_MODULE StoragePropertyTests

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <csignal>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <openssl/evp.h>

#include "binsrv/indexed_event_block.hpp"
#include "binsrv/main_config.hpp"
#include "binsrv/null_logger.hpp"
#include "binsrv/replication_mode_type.hpp"
#include "binsrv/storage.hpp"
#include "binsrv/storage_core.hpp"

#include "binsrv/gtids/gtid.hpp"
#include "binsrv/gtids/gtid_set.hpp"
#include "binsrv/gtids/tag.hpp"
#include "binsrv/gtids/uuid.hpp"

#include "binsrv/events/common_types.hpp"
#include "binsrv/events/composite_binlog_name.hpp"
#include "binsrv/events/protocol_traits_fwd.hpp"

#include "util/byte_range.hpp"
#include "util/byte_span_fwd.hpp"
#include "util/ctime_timestamp.hpp"
#include "util/ctime_timestamp_range.hpp"
#include "util/dynamic_byte_buffer_fwd.hpp"

#include "common/gtid_model.hpp"
#include "common/pbt_helpers.hpp"

namespace {

namespace gs = hegel::generators;
namespace model = pbt::gtid_model;
namespace fs = std::filesystem;

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

util::const_byte_span cspan(const bytes &data) {
  return {// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
          reinterpret_cast<const std::byte *>(std::data(data)),
          std::size(data)};
}

bytes to_bytes(const util::dynamic_byte_buffer &data) {
  bytes result(std::size(data));
  for (std::size_t i{0U}; i < std::size(data); ++i) {
    result[i] = std::to_integer<std::uint8_t>(data[i]);
  }
  return result;
}

bytes read_file(const fs::path &path) {
  std::ifstream input{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}

bytes magic_payload() {
  bytes result;
  for (const auto value : binsrv::events::magic_binlog_payload) {
    result.push_back(std::to_integer<std::uint8_t>(value));
  }
  return result;
}

// A fresh directory for one test case, removed afterwards.
class temporary_directory {
public:
  temporary_directory() {
    std::string pattern{
        (fs::temp_directory_path() / "pbs-pbt-storage-XXXXXX").string()};
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (::mkdtemp(std::data(pattern)) == nullptr) {
      throw std::runtime_error{"mkdtemp failed"};
    }
    path_ = pattern;
  }
  temporary_directory(const temporary_directory &) = delete;
  temporary_directory &operator=(const temporary_directory &) = delete;
  temporary_directory(temporary_directory &&) = delete;
  temporary_directory &operator=(temporary_directory &&) = delete;
  ~temporary_directory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  [[nodiscard]] const fs::path &path() const noexcept { return path_; }

private:
  fs::path path_;
};

struct keyring_key {
  std::string id;
  std::string cipher;
  bytes data;
};

struct storage_settings {
  bool gtid_mode{};
  // storage.checkpoint_size in bytes (0: not set)
  std::uint64_t checkpoint_size{};
  // storage.checkpoint_interval (not set when empty)
  std::string checkpoint_interval{};
  // keyring file content (no keyring section when empty)
  std::vector<keyring_key> keyring{};
  // storage.encryption: KEK id and data cipher (no section when empty)
  std::string kek_id{};
  std::string data_cipher{};
  // storage.uri of an S3 backend (filesystem backend in <dir>/vault when
  // empty)
  std::string s3_uri{};
};

// main_config for a filesystem storage in <directory>/vault
std::unique_ptr<binsrv::main_config>
make_config(const temporary_directory &directory,
            const storage_settings &settings) {
  const auto vault{directory.path() / "vault"};
  fs::create_directories(vault);
  // README <instance>: mandatory, an existing directory (instance_id file)
  const auto local_settings{directory.path() / "local-settings"};
  fs::create_directories(local_settings);
  std::ostringstream json;
  json << R"({
  "instance": {"local_settings_path": ")"
       << local_settings.generic_string() << R"("},
  "logger": {"level": "error", "file": ""},
  "connection": {"host": "127.0.0.1", "port": 3306, "user": "u",
                 "password": "", "connect_timeout": 1, "read_timeout": 1,
                 "write_timeout": 1},
  "replication": {"server_id": 1, "idle_time": 1, "verify_checksum": true,
                  "mode": ")"
       << (settings.gtid_mode ? "gtid" : "position") << R"("},
  "replication_source": {"port": 3307, "server_id": 142,
                         "server_uuid": "00000000-0000-0000-0000-00000000008e",
                         "read_timeout": 1, "write_timeout": 1,
                         "require_secure_transport": false,
                         "authentication": {"user": "u", "password": "p",
                                            "plugin": "caching_sha2_password"}},)";
  if (!settings.keyring.empty()) {
    const auto keyring_path{directory.path() / "keyring.json"};
    std::ostringstream keyring;
    keyring << R"({"version": 1, "keys": [)";
    for (std::size_t i{0U}; i < settings.keyring.size(); ++i) {
      const auto &key{settings.keyring[i]};
      keyring << (i == 0U ? "" : ", ") << R"({"id": ")" << key.id
              << R"(", "cipher": ")" << key.cipher << R"(", "data_hex": ")"
              << hex(key.data) << R"("})";
    }
    keyring << "]}\n";
    std::ofstream output{keyring_path};
    output << keyring.str();
    json << R"(
  "keyring": {"uri": "file://)"
         << keyring_path.generic_string() << R"("},)";
  }
  if (settings.s3_uri.empty()) {
    json << R"(
  "storage": {"backend": "file", "uri": "file://)"
         << vault.generic_string() << '"';
  } else {
    json << R"(
  "storage": {"backend": "s3", "uri": ")"
         << settings.s3_uri << '"';
  }
  if (settings.checkpoint_size != 0U) {
    json << R"(, "checkpoint_size": ")" << settings.checkpoint_size << '"';
  }
  if (!settings.checkpoint_interval.empty()) {
    json << R"(, "checkpoint_interval": ")" << settings.checkpoint_interval
         << '"';
  }
  if (!settings.kek_id.empty()) {
    json << R"(, "encryption": {"format": "generic", "kek_id": ")"
         << settings.kek_id << R"(", "cipher": ")" << settings.data_cipher
         << R"("})";
  }
  // README <telemetry>: off, it would write outside $TMPDIR (only 'pull'
  // writes reports, but keep every configuration of the tests the same)
  json << "},\n  \"telemetry\": {\"enabled\": false}\n}\n";
  const auto config_path{directory.path() / "config.json"};
  {
    std::ofstream output{config_path};
    output << json.str();
  }
  return std::make_unique<binsrv::main_config>(config_path.string());
}

std::unique_ptr<binsrv::storage>
make_storage(const binsrv::main_config &config,
             binsrv::storage_construction_mode_type mode =
                 binsrv::storage_construction_mode_type::streaming) {
  return std::make_unique<binsrv::storage>(
      std::make_shared<binsrv::null_logger>(), config, mode);
}

binsrv::gtids::uuid to_uuid(const model::raw_uuid &raw) {
  binsrv::gtids::uuid_storage storage{};
  for (std::size_t i{0U}; i < raw.size(); ++i) {
    storage[i] = std::byte{raw[i]};
  }
  return binsrv::gtids::uuid{storage};
}

// ---------------------------------------------------------------------------
// independent decryption oracle (OpenSSL EVP + the metadata JSON)
// ---------------------------------------------------------------------------
bytes from_hex(std::string_view text) {
  const auto digit{[](char value) -> std::uint8_t {
    if (value >= '0' && value <= '9') {
      return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
      return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
      return static_cast<std::uint8_t>(value - 'A' + 10);
    }
    throw std::invalid_argument{"invalid hex digit"};
  }};
  bytes result;
  for (std::size_t i{0U}; i + 1U < text.size(); i += 2U) {
    result.push_back(
        static_cast<std::uint8_t>((digit(text[i]) << 4U) | digit(text[i + 1U])));
  }
  return result;
}

bytes evp_decrypt(const std::string &cipher_name, const bytes &key,
                  const bytes &iv, const bytes &tag, const bytes &input) {
  const EVP_CIPHER *cipher{EVP_get_cipherbyname(cipher_name.c_str())};
  pbt::require(cipher != nullptr, "oracle: unknown cipher " + cipher_name);
  const std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx{
      EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free};
  pbt::require(EVP_DecryptInit_ex(ctx.get(), cipher, nullptr, key.data(),
                                  iv.empty() ? nullptr : iv.data()) == 1,
               "oracle: EVP_DecryptInit_ex");
  EVP_CIPHER_CTX_set_padding(ctx.get(), 0);
  bytes output(input.size() + 32U);
  int length{0};
  pbt::require(EVP_DecryptUpdate(ctx.get(), output.data(), &length,
                                 input.data(),
                                 static_cast<int>(input.size())) == 1,
               "oracle: EVP_DecryptUpdate");
  auto total{static_cast<std::size_t>(length)};
  if (!tag.empty()) {
    pbt::require(
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_TAG,
                            static_cast<int>(tag.size()),
                            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
                            const_cast<std::uint8_t *>(tag.data())) == 1,
        "oracle: set tag");
  }
  pbt::require(EVP_DecryptFinal_ex(ctx.get(), output.data() + total, &length) ==
                   1,
               "oracle: EVP_DecryptFinal_ex (authentication)");
  total += static_cast<std::size_t>(length);
  output.resize(total);
  return output;
}

std::optional<bytes> optional_hex(const boost::json::object &object,
                                  std::string_view key) {
  const auto *value{object.if_contains(key)};
  if (value == nullptr || value->is_null()) {
    return std::nullopt;
  }
  return from_hex(value->as_string());
}

// The plaintext of a stored binlog file, decrypted with the KEK from the
// keyring and the envelopes in <binlog>.json, following the README
// ("encryption at rest"): file key = KEK-decrypt(file_key_envelope),
// data = <file_data_envelope.cipher>-decrypt(file key, iv) from offset 0.
// Returns the file content unchanged for an unencrypted binlog.
bytes stored_plaintext(const temporary_directory &directory,
                       const storage_settings &settings,
                       const std::string &binlog_name, bool &encrypted) {
  const auto data{read_file(directory.path() / "vault" / binlog_name)};
  const auto metadata_bytes{
      read_file(directory.path() / "vault" / (binlog_name + ".json"))};
  const std::string metadata_text(metadata_bytes.begin(), metadata_bytes.end());
  const auto metadata{boost::json::parse(metadata_text).as_object()};
  const auto *encryption{metadata.if_contains("encryption")};
  encrypted = encryption != nullptr && !encryption->is_null();
  if (!encrypted) {
    return data;
  }
  const auto &key_envelope{
      encryption->as_object().at("file_key_envelope").as_object()};
  const auto &data_envelope{
      encryption->as_object().at("file_data_envelope").as_object()};
  const std::string kek_id{key_envelope.at("kek_id").as_string()};
  const auto kek{std::ranges::find_if(settings.keyring,
                                      [&kek_id](const keyring_key &key) {
                                        return key.id == kek_id;
                                      })};
  pbt::require(kek != settings.keyring.end(),
               "oracle: KEK of the binlog is in the keyring");
  const auto file_key{evp_decrypt(
      kek->cipher, kek->data,
      optional_hex(key_envelope, "iv_hex").value_or(bytes{}),
      optional_hex(key_envelope, "tag_hex").value_or(bytes{}),
      from_hex(key_envelope.at("data_hex").as_string()))};
  return evp_decrypt(std::string{data_envelope.at("cipher").as_string()},
                     file_key, from_hex(data_envelope.at("iv_hex").as_string()),
                     {}, data);
}

// ---------------------------------------------------------------------------
// model
// ---------------------------------------------------------------------------
struct model_file {
  binsrv::events::composite_binlog_name name;
  bytes flushed;
  binsrv::gtids::gtid_set previous_gtids;
  binsrv::gtids::gtid_set added_gtids;
  util::ctime_timestamp_range timestamps;
  binsrv::events::seq_no_t last_sequence_number{0U};
  // created while storage.encryption was configured
  bool encrypted{false};
};

struct storage_model {
  storage_settings settings;
  binsrv::gtids::gtid_set purged;
  std::vector<model_file> files;
  bool open{false};
  // event buffer: everything written and not flushed / discarded
  bytes buffer;
  std::size_t boundary{0U};
  binsrv::gtids::gtid_set buffer_gtids;
  util::ctime_timestamp_range ready_timestamps;
  util::ctime_timestamp_range incomplete_timestamps;
  binsrv::events::seq_no_t ready_sequence_number{0U};
  binsrv::events::seq_no_t incomplete_sequence_number{0U};
  std::uint64_t last_checkpoint_position{0U};

  [[nodiscard]] binsrv::gtids::gtid_set all_gtids() const {
    if (!settings.gtid_mode) {
      return {};
    }
    if (files.empty()) {
      return purged;
    }
    auto result{files.back().previous_gtids};
    result += files.back().added_gtids;
    return result;
  }
  [[nodiscard]] std::uint64_t flushed_position() const {
    return files.empty() ? 0U : files.back().flushed.size();
  }
  [[nodiscard]] std::uint64_t current_position() const {
    return flushed_position() + buffer.size();
  }

  void flush() {
    if (boundary == 0U) {
      return;
    }
    auto &file{files.back()};
    file.flushed.insert(file.flushed.end(), buffer.begin(),
                        buffer.begin() + static_cast<std::ptrdiff_t>(boundary));
    if (settings.gtid_mode) {
      file.added_gtids += buffer_gtids;
    }
    file.timestamps.add_range(ready_timestamps);
    file.last_sequence_number = ready_sequence_number;
    buffer.erase(buffer.begin(),
                 buffer.begin() + static_cast<std::ptrdiff_t>(boundary));
    boundary = 0U;
    buffer_gtids.clear();
    ready_timestamps.clear();
  }

  void open_binlog(const binsrv::events::composite_binlog_name &name) {
    if (files.empty() || files.back().name != name) {
      model_file file{.name = name,
                      .flushed = magic_payload(),
                      .previous_gtids = all_gtids(),
                      .added_gtids = {},
                      .timestamps = {},
                      .last_sequence_number = 0U,
                      .encrypted = !settings.kek_id.empty()};
      files.push_back(std::move(file));
      ready_sequence_number = 0U;
      incomplete_sequence_number = 0U;
    } else {
      ready_sequence_number = files.back().last_sequence_number;
      incomplete_sequence_number = ready_sequence_number;
    }
    open = true;
    last_checkpoint_position = current_position();
  }

  void write(const bytes &data, bool at_boundary,
             const binsrv::gtids::gtid &gtid,
             const util::ctime_timestamp &timestamp,
             binsrv::events::seq_no_t sequence_number) {
    buffer.insert(buffer.end(), data.begin(), data.end());
    incomplete_timestamps.add_timestamp(timestamp);
    if (sequence_number != 0U) {
      incomplete_sequence_number = sequence_number;
    }
    if (at_boundary) {
      boundary = buffer.size();
      if (settings.gtid_mode && !gtid.is_empty()) {
        buffer_gtids += gtid;
      }
      ready_timestamps.add_range(incomplete_timestamps);
      incomplete_timestamps.clear();
      ready_sequence_number = incomplete_sequence_number;
    }
    if (boundary != 0U && settings.checkpoint_size != 0U) {
      const auto ready_position{flushed_position() + boundary};
      if (ready_position >=
          last_checkpoint_position + settings.checkpoint_size) {
        flush();
        last_checkpoint_position = ready_position;
      }
    }
  }

  void discard() {
    buffer.resize(boundary);
    incomplete_timestamps.clear();
    incomplete_sequence_number = ready_sequence_number;
  }

  void close() {
    flush();
    buffer.clear();
    open = false;
    last_checkpoint_position = current_position();
  }

  // the process ends (the storage destructor flushes) and starts again
  void restart() {
    if (open) {
      flush();
    }
    buffer.clear();
    boundary = 0U;
    buffer_gtids.clear();
    ready_timestamps.clear();
    incomplete_timestamps.clear();
    ready_sequence_number = 0U;
    incomplete_sequence_number = 0U;
    open = false;
    last_checkpoint_position = 0U;
    // purged GTIDs are kept in memory until the first binlog is created
    // (they become its previous_gtids); the collector sets them again
    // (SELECT @@GLOBAL.gtid_purged) on every connect to an empty storage
    if (files.empty()) {
      purged.clear();
    }
  }
};

std::string describe(const util::ctime_timestamp_range &range) {
  return "[" + std::to_string(range.get_min_timestamp().get_value()) + ", " +
         std::to_string(range.get_max_timestamp().get_value()) + "]";
}

// what the storage reports must be what the model says
void check_reported_state(const binsrv::storage &storage,
                          const storage_model &expected) {
  pbt::require_equal(storage.get_current_position(),
                     expected.current_position(), "get_current_position()");
  pbt::require_equal(storage.is_empty(), expected.files.empty(), "is_empty()");
  pbt::require_equal(storage.get_gtids(), expected.all_gtids(), "get_gtids()");
  pbt::require_equal(storage.get_last_transaction_sequence_number(),
                     expected.incomplete_sequence_number,
                     "get_last_transaction_sequence_number()");
  pbt::require_equal(storage.is_binlog_open(), expected.open,
                     "is_binlog_open()");
  const auto records{storage.get_binlog_records()};
  pbt::require_equal(records.size(), expected.files.size(),
                     "number of binlog records");
  for (std::size_t i{0U}; i < records.size(); ++i) {
    const auto &record{records[i]};
    const auto &file{expected.files[i]};
    pbt::require_equal(record.name.str(), file.name.str(), "binlog name");
    pbt::require_equal(record.size, std::uint64_t{file.flushed.size()},
                       "binlog record size (flushed bytes)");
    if (expected.settings.gtid_mode) {
      pbt::require(record.previous_gtids.has_value() &&
                       record.added_gtids.has_value(),
                   "GTID sets present in GTID mode");
      pbt::require_equal(*record.previous_gtids, file.previous_gtids,
                         "binlog record previous_gtids");
      pbt::require_equal(*record.added_gtids, file.added_gtids,
                         "binlog record added_gtids");
    }
    pbt::require_equal(describe(record.timestamps), describe(file.timestamps),
                       "binlog record timestamps (flushed events)");
    pbt::require_equal(record.last_sequence_number, file.last_sequence_number,
                       "binlog record last_sequence_number");
  }
}

// what is persisted must be exactly the flushed data
void check_fetched_binlog(const binsrv::storage &storage,
                          const model_file &file) {
  const auto name{file.name};
  const util::byte_range range{binsrv::events::magic_binlog_offset,
                               file.flushed.size() -
                                   binsrv::events::magic_binlog_offset};
  util::dynamic_byte_buffer buffer;
  const auto result{storage.fetch_event_block(name, range, buffer)};
  // a binlog without events: the offset is its end, which is EOF (of the
  // active binlog or of an older one)
  const bool expected_result{
      file.flushed.size() == binsrv::events::magic_binlog_offset
          ? (result == binsrv::storage_fetch_result_type::active_file_eof ||
             result == binsrv::storage_fetch_result_type::non_active_file_eof)
          : result == binsrv::storage_fetch_result_type::success_full};
  pbt::require(expected_result,
               "fetch_event_block() of a whole binlog succeeds");
  const bytes payload(file.flushed.begin() + 4, file.flushed.end());
  pbt::require_equal(hex(to_bytes(buffer)), hex(payload),
                     "fetch_event_block() returns the flushed data");
}

void check_persisted_state(const temporary_directory &directory,
                           const binsrv::storage &storage,
                           const storage_model &expected) {
  for (const auto &file : expected.files) {
    if (!expected.settings.s3_uri.empty()) {
      // S3: the objects are checked through fetch_event_block() only
      check_fetched_binlog(storage, file);
      continue;
    }
    bool encrypted{false};
    const auto plaintext{stored_plaintext(directory, expected.settings,
                                          file.name.str(), encrypted)};
    pbt::require_equal(encrypted, file.encrypted,
                       "binlog encrypted exactly when it was created with "
                       "encryption enabled");
    pbt::require_equal(hex(plaintext), hex(file.flushed),
                       "binlog file content on disk = flushed data");
    if (encrypted) {
      pbt::require(read_file(directory.path() / "vault" / file.name.str()) !=
                       file.flushed,
                   "encrypted binlog is not stored in plaintext");
    }
    check_fetched_binlog(storage, file);
  }
}

enum class operation : std::uint8_t {
  open_new,
  open_current,
  write,
  flush,
  discard,
  close,
  restart
};

constexpr std::array operation_names{"open_new", "open_current", "write",
                                     "flush",    "discard",      "close",
                                     "restart"};

gs::Generator<binsrv::gtids::gtid> gtids() {
  // a small pool of TSIDs so that sets have ranges, sometimes tagged
  return gs::compose([](const hegel::TestCase &tc) {
    const auto uuid{tc.draw(gs::sampled_from<model::raw_uuid>(
        {model::raw_uuid{0x11, 0x11, 0x11, 0x11},
         model::raw_uuid{0x22, 0x22, 0x22, 0x22}}))};
    const auto tag{tc.draw(gs::sampled_from<std::string>({"", "", "t1"}))};
    const auto gno{
        tc.draw(gs::integers<std::uint64_t>({.min_value = 1U, .max_value = 30U}))};
    return binsrv::gtids::gtid{to_uuid(uuid), binsrv::gtids::tag{tag}, gno};
  });
}

} // namespace

// ===========================================================================
// stateful model
// ===========================================================================

// A random sequence of the operations the collector performs, in the order
// it may perform them (writes only into an open binlog, close only at a
// transaction boundary, open only with an empty event buffer). After every
// operation the storage reports the model's state; after every restart
// (and at the end) the files and fetch_event_block() hold exactly the
// flushed bytes.
namespace {

void run_storage_model(hegel::TestCase &tc, const storage_settings &base,
                       std::size_t max_steps = 30U) {
        const temporary_directory directory;
        storage_model expected;
        expected.settings = base;
        expected.settings.gtid_mode = tc.draw("gtid_mode", gs::booleans());
        expected.settings.checkpoint_size = tc.draw(
            "checkpoint_size",
            gs::sampled_from<std::uint64_t>({0U, 0U, 1U, 40U, 200U}));
        const auto config{make_config(directory, expected.settings)};
        auto storage{make_storage(*config)};

        if (expected.settings.gtid_mode && tc.draw("purged", gs::booleans())) {
          binsrv::gtids::gtid_set purged;
          purged += tc.draw("purged_gtid", gtids());
          storage->set_purged_gtids(purged);
          expected.purged = purged;
        }
        check_reported_state(*storage, expected);

        std::uint32_t next_number{tc.draw(
            "first_binlog_number",
            gs::integers<std::uint32_t>({.min_value = 1U, .max_value = 999U}))};
        binsrv::events::seq_no_t next_sequence_number{1U};
        const auto steps{tc.draw(
            "steps",
            gs::integers<std::size_t>({.min_value = 1U, .max_value = max_steps}))};
        for (std::size_t step{0U}; step < steps; ++step) {
          std::vector<operation> possible{operation::restart};
          if (!expected.open) {
            possible.push_back(operation::open_new);
            if (!expected.files.empty()) {
              possible.push_back(operation::open_current);
            }
          } else {
            possible.insert(possible.end(),
                            {operation::write, operation::write,
                             operation::write, operation::flush,
                             operation::discard});
            if (expected.buffer.size() == expected.boundary) {
              possible.push_back(operation::close);
            }
          }
          const auto op{tc.draw("operation", gs::sampled_from(possible))};
          tc.note(std::string{"step "} + std::to_string(step) + ": " +
                  operation_names[static_cast<std::size_t>(op)]);
          switch (op) {
          case operation::open_new: {
            const binsrv::events::composite_binlog_name name{"binlog",
                                                             next_number};
            ++next_number;
            std::ignore = storage->open_binlog(name);
            expected.open_binlog(name);
            next_sequence_number = 1U;
            break;
          }
          case operation::open_current: {
            const auto name{expected.files.back().name};
            std::ignore = storage->open_binlog(name);
            expected.open_binlog(name);
            next_sequence_number = expected.files.back().last_sequence_number + 1U;
            break;
          }
          case operation::write: {
            const auto data{tc.draw(
                "event", gs::binary({.min_size = 1U, .max_size = 48U}))};
            const auto at_boundary{tc.draw("at_boundary", gs::booleans())};
            binsrv::gtids::gtid gtid{};
            if (expected.settings.gtid_mode) {
              gtid = tc.draw("gtid", gtids());
            }
            const util::ctime_timestamp timestamp{
                tc.draw("timestamp", gs::integers<std::time_t>(
                                         {.min_value = 1,
                                          .max_value = 2000000000}))};
            binsrv::events::seq_no_t sequence_number{0U};
            if (tc.draw("starts_transaction", gs::booleans())) {
              sequence_number = next_sequence_number++;
            }
            storage->write_event(cspan(data), at_boundary, gtid, timestamp,
                                 sequence_number);
            expected.write(data, at_boundary, gtid, timestamp,
                           sequence_number);
            break;
          }
          case operation::flush:
            storage->flush_event_buffer();
            expected.flush();
            break;
          case operation::discard:
            storage->discard_incomplete_transaction_events();
            expected.discard();
            break;
          case operation::close:
            storage->close_binlog();
            expected.close();
            break;
          case operation::restart:
            storage.reset();
            expected.restart();
            storage = make_storage(*config);
            check_persisted_state(directory, *storage, expected);
            break;
          }
          check_reported_state(*storage, expected);
        }
        storage.reset();
        expected.restart();
        storage = make_storage(*config);
        check_reported_state(*storage, expected);
        check_persisted_state(directory, *storage, expected);

        // random ranges inside the binlogs read back the flushed bytes
        // (for encrypted binlogs: CTR decryption from any offset)
        for (const auto &file : expected.files) {
          if (file.flushed.size() <= binsrv::events::magic_binlog_offset) {
            continue;
          }
          const auto offset{tc.draw(
              "fetch_offset",
              gs::integers<std::uint64_t>(
                  {.min_value = binsrv::events::magic_binlog_offset,
                   .max_value = file.flushed.size() - 1U}))};
          const auto length{tc.draw(
              "fetch_length",
              gs::integers<std::uint64_t>(
                  {.min_value = 1U, .max_value = file.flushed.size() - offset}))};
          const auto name{file.name};
          const util::byte_range range{offset, length};
          util::dynamic_byte_buffer buffer;
          pbt::require(storage->fetch_event_block(name, range, buffer) ==
                           binsrv::storage_fetch_result_type::success_full,
                       "fetch_event_block() of a range succeeds (success_full)");
          const auto first{file.flushed.begin() +
                           static_cast<std::ptrdiff_t>(offset)};
          pbt::require_equal(
              hex(to_bytes(buffer)),
              hex(bytes(first, first + static_cast<std::ptrdiff_t>(length))),
              "fetch_event_block() of a range returns the flushed bytes");
        }
}

} // namespace

BOOST_AUTO_TEST_CASE(StorageMatchesModel) {
  pbt::check([](hegel::TestCase &tc) { run_storage_model(tc, {}); },
             {.test_cases = 300U});
}

// ===========================================================================
// fetch_event_block
// ===========================================================================
namespace {

struct fetch_request {
  std::string name;
  std::uint64_t offset{};
  std::optional<std::uint64_t> length;
};

std::ostream &operator<<(std::ostream &output, const fetch_request &obj) {
  output << "{name='" << obj.name << "', offset=" << obj.offset << ", length=";
  if (obj.length) {
    output << *obj.length;
  } else {
    output << "-";
  }
  return output << "}";
}

enum class fetch_outcome : std::uint8_t {
  throws,
  success_full,
  success_truncated,
  non_active_file_eof,
  active_file_eof
};

std::string to_string(fetch_outcome outcome) {
  switch (outcome) {
  case fetch_outcome::throws:
    return "throws std::runtime_error";
  case fetch_outcome::success_full:
    return "success_full";
  case fetch_outcome::success_truncated:
    return "success_truncated";
  case fetch_outcome::non_active_file_eof:
    return "non_active_file_eof";
  case fetch_outcome::active_file_eof:
    return "active_file_eof";
  }
  return "?";
}

fetch_outcome to_outcome(binsrv::storage_fetch_result_type result) {
  switch (result) {
  case binsrv::storage_fetch_result_type::success_full:
    return fetch_outcome::success_full;
  case binsrv::storage_fetch_result_type::success_truncated:
    return fetch_outcome::success_truncated;
  case binsrv::storage_fetch_result_type::non_active_file_eof:
    return fetch_outcome::non_active_file_eof;
  case binsrv::storage_fetch_result_type::active_file_eof:
    return fetch_outcome::active_file_eof;
  }
  return fetch_outcome::throws;
}

struct fetch_result {
  fetch_outcome outcome{};
  bytes data;  // the buffer afterwards (untouched when it throws)
};

// The contract documented in storage_core.hpp (fetch_event_block()),
// applied to the model files: the name and range are not modified, a call
// never reads across binlog files.
fetch_result expected_fetch(const std::vector<model_file> &files,
                            const fetch_request &request) {
  const fetch_result throws{fetch_outcome::throws, {}};
  if (request.name.empty() || !request.length ||
      request.offset < binsrv::events::magic_binlog_offset) {
    return throws;
  }
  const auto found{std::ranges::find_if(files, [&](const model_file &file) {
    return file.name.str() == request.name;
  })};
  if (found == files.end()) {
    return throws;
  }
  const auto &file{*found};
  const std::uint64_t size{file.flushed.size()};
  if (request.offset > size) {
    return throws;
  }
  if (request.offset == size) {
    // even for an empty range; the buffer is cleared
    return {std::next(found) == files.end() ? fetch_outcome::active_file_eof
                                            : fetch_outcome::non_active_file_eof,
            {}};
  }
  if (*request.length == 0U) {
    return {fetch_outcome::success_full, {}};
  }
  const auto length{std::min<std::uint64_t>(*request.length, size - request.offset)};
  const auto first{file.flushed.begin() + static_cast<std::ptrdiff_t>(request.offset)};
  return {length == *request.length ? fetch_outcome::success_full
                                    : fetch_outcome::success_truncated,
          bytes(first, first + static_cast<std::ptrdiff_t>(length))};
}

} // namespace

// fetch_event_block() (used by the 'pull' replication source and
// search operations) follows the rules documented in storage_core.hpp for
// every combination of binlog name (empty, existing, missing), offset
// (before / inside / at / after the end of a file) and length (none, 0,
// more than available): invalid requests throw std::runtime_error and leave
// the buffer untouched; at the end of a binlog the result is EOF (of the
// active binlog or of an older one); otherwise the result says whether the
// whole range was read or it was truncated at the end of the binlog.
BOOST_AUTO_TEST_CASE(FetchEventBlockFollowsContract) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const temporary_directory directory;
        storage_model expected;
        expected.settings.gtid_mode = false;
        const auto config{make_config(directory, expected.settings)};
        {
          auto storage{make_storage(*config)};
          const auto number_of_files{tc.draw(
              "files", gs::integers<std::size_t>({.max_value = 3U}))};
          for (std::size_t i{0U}; i < number_of_files; ++i) {
            const binsrv::events::composite_binlog_name name{
                "binlog", static_cast<std::uint32_t>(i + 1U)};
            std::ignore = storage->open_binlog(name);
            expected.open_binlog(name);
            const auto events{tc.draw(
                "events", gs::vectors(gs::binary({.min_size = 1U,
                                                  .max_size = 40U}),
                                      {.max_size = 4U}))};
            for (const auto &data : events) {
              storage->write_event(cspan(data), true, {},
                                   util::ctime_timestamp{1}, 0U);
              expected.write(data, true, {}, util::ctime_timestamp{1}, 0U);
            }
            storage->close_binlog();
            expected.close();
          }
        }
        const auto storage{make_storage(
            *config, binsrv::storage_construction_mode_type::querying_only)};

        std::vector<std::string> names{"", "binlog.000099"};
        std::uint64_t max_size{8U};
        for (const auto &file : expected.files) {
          names.push_back(file.name.str());
          max_size = std::max<std::uint64_t>(max_size, file.flushed.size());
        }
        for (int query{0}; query < 8; ++query) {
          fetch_request request;
          request.name = tc.draw("name", gs::sampled_from(names));
          request.offset = tc.draw(
              "offset", gs::integers<std::uint64_t>({.max_value = max_size + 2U}));
          if (tc.draw("has_length", gs::booleans())) {
            request.length = tc.draw(
                "length",
                gs::integers<std::uint64_t>({.max_value = max_size + 2U}));
          }
          const auto wanted{expected_fetch(expected.files, request)};
          std::ostringstream request_text;
          request_text << request;
          tc.note("fetch " + request_text.str() + ", expected " +
                  to_string(wanted.outcome));

          binsrv::events::composite_binlog_name name{};
          if (!request.name.empty()) {
            name = binsrv::events::composite_binlog_name::parse(request.name);
          }
          const util::byte_range range{request.offset, request.length};
          util::dynamic_byte_buffer buffer{std::byte{0x5A}};
          fetch_outcome outcome{fetch_outcome::throws};
          try {
            outcome = to_outcome(storage->fetch_event_block(name, range, buffer));
          } catch (const std::runtime_error &) {
            outcome = fetch_outcome::throws;
          }
          pbt::require_equal(to_string(outcome), to_string(wanted.outcome),
                             "result");
          if (outcome == fetch_outcome::throws) {
            pbt::require_equal(hex(to_bytes(buffer)), std::string{"5a"},
                               "buffer untouched when the request is rejected");
          } else {
            pbt::require_equal(hex(to_bytes(buffer)), hex(wanted.data), "data");
          }
        }
      },
      {.test_cases = 300U});
}

// ===========================================================================
// purge_binlogs
// ===========================================================================
namespace {

// streams the given files (GTID mode, every event a complete transaction)
// through a streaming storage, as the collector does
void populate(const binsrv::main_config &config, storage_model &expected,
              hegel::TestCase &tc, std::size_t number_of_files) {
  auto storage{make_storage(config)};
  for (std::size_t i{0U}; i < number_of_files; ++i) {
    const binsrv::events::composite_binlog_name name{
        "binlog", static_cast<std::uint32_t>(i + 1U)};
    std::ignore = storage->open_binlog(name);
    expected.open_binlog(name);
    const auto transactions{tc.draw(
        "transactions", gs::integers<std::size_t>({.max_value = 3U}))};
    for (std::size_t t{0U}; t < transactions; ++t) {
      const auto data{
          tc.draw("event", gs::binary({.min_size = 1U, .max_size = 32U}))};
      const auto gtid{tc.draw("gtid", gtids())};
      const util::ctime_timestamp timestamp{static_cast<std::time_t>(t + 1U)};
      storage->write_event(cspan(data), true, gtid, timestamp, t + 1U);
      expected.write(data, true, gtid, timestamp, t + 1U);
    }
    storage->close_binlog();
    expected.close();
  }
  storage.reset();
  expected.restart();
}

// like populate(), appending binlogs after the existing ones
void populate_more(const binsrv::main_config &config, storage_model &expected,
                   hegel::TestCase &tc, std::size_t number_of_files) {
  auto storage{make_storage(config)};
  const auto first_number{expected.files.empty()
                              ? 1U
                              : expected.files.back().name.get_sequence_number() + 1U};
  for (std::size_t i{0U}; i < number_of_files; ++i) {
    const binsrv::events::composite_binlog_name name{
        "binlog", static_cast<std::uint32_t>(first_number + i)};
    std::ignore = storage->open_binlog(name);
    expected.open_binlog(name);
    const auto data{
        tc.draw("event", gs::binary({.min_size = 1U, .max_size = 32U}))};
    const auto gtid{tc.draw("gtid", gtids())};
    storage->write_event(cspan(data), true, gtid, util::ctime_timestamp{7}, 1U);
    expected.write(data, true, gtid, util::ctime_timestamp{7}, 1U);
    storage->close_binlog();
    expected.close();
  }
  storage.reset();
  expected.restart();
}

std::vector<std::string> vault_content(const temporary_directory &directory) {
  std::vector<std::string> result;
  for (const auto &entry : fs::directory_iterator{directory.path() / "vault"}) {
    result.push_back(entry.path().filename().string());
  }
  std::ranges::sort(result);
  return result;
}

} // namespace

// purge_binlogs(target) (the 'purge_binlogs' operation) removes the binlogs
// from the oldest one up to and including 'target' and keeps the rest
// usable; it refuses, without changing anything, an empty storage, a
// target with another base name, a missing target and the current (last)
// binlog. After a purge the storage reopens, reports the remaining binlogs
// with their data and GTIDs unchanged, and the purged files are gone.
BOOST_AUTO_TEST_CASE(PurgeBinlogsFollowsContract) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const temporary_directory directory;
        storage_model expected;
        expected.settings.gtid_mode = true;
        const auto config{make_config(directory, expected.settings)};
        const auto number_of_files{
            tc.draw("files", gs::integers<std::size_t>({.max_value = 4U}))};
        populate(*config, expected, tc, number_of_files);

        enum class target_kind : std::uint8_t { existing, other_base, missing };
        auto kind{tc.draw("target_kind",
                          gs::sampled_from<target_kind>({target_kind::existing,
                                                         target_kind::existing,
                                                         target_kind::other_base,
                                                         target_kind::missing}))};
        if (number_of_files == 0U) {
          kind = target_kind::missing;
        }
        binsrv::events::composite_binlog_name target{"binlog", 50U};
        std::size_t target_index{0U};
        if (kind == target_kind::existing) {
          target_index = tc.draw(
              "target_index",
              gs::integers<std::size_t>({.max_value = number_of_files - 1U}));
          target = expected.files[target_index].name;
        } else if (kind == target_kind::other_base) {
          target = binsrv::events::composite_binlog_name{"other", 1U};
        }
        tc.note("target: " + target.str());
        const bool must_succeed{kind == target_kind::existing &&
                                target_index + 1U < number_of_files};
        const auto content_before{vault_content(directory)};
        {
          auto purging{make_storage(
              *config, binsrv::storage_construction_mode_type::purging)};
          if (must_succeed) {
            const auto [removed, warning]{purging->purge_binlogs(target)};
            pbt::require(warning.empty(), "no cleanup warning");
            pbt::require_equal(removed.size(), target_index + 1U,
                               "number of purged binlogs");
            for (std::size_t i{0U}; i < removed.size(); ++i) {
              pbt::require_equal(removed[i].name.str(),
                                 expected.files[i].name.str(),
                                 "purged binlogs, oldest first");
            }
            const auto gtids_before{expected.all_gtids()};
            expected.files.erase(expected.files.begin(),
                                 expected.files.begin() +
                                     static_cast<std::ptrdiff_t>(
                                         target_index + 1U));
            pbt::require_equal(expected.all_gtids(), gtids_before,
                               "model sanity: GTIDs of the last binlog");
          } else {
            pbt::require_throws<std::runtime_error>(
                [&] { std::ignore = purging->purge_binlogs(target); },
                "invalid purge target must be rejected");
            pbt::require(vault_content(directory) == content_before,
                         "a rejected purge leaves the storage unchanged");
          }
        }
        auto reopened{make_storage(*config)};
        check_reported_state(*reopened, expected);
        check_persisted_state(directory, *reopened, expected);
        for (std::size_t i{0U}; i < target_index + 1U && must_succeed; ++i) {
          const auto name{"binlog." + std::string(5U, '0') +
                          std::to_string(i + 1U)};
          pbt::require(!fs::exists(directory.path() / "vault" / name),
                       "purged binlog file removed");
        }
      },
      {.test_cases = 300U});
}

// The GTIDs purged before the first stored binlog (set from
// @@GLOBAL.gtid_purged on an empty storage) are reported by
// get_purged_gtids() before and after a restart alike.
BOOST_AUTO_TEST_CASE(PurgedGtidsSurviveRestart) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const temporary_directory directory;
        storage_model expected;
        expected.settings.gtid_mode = true;
        const auto config{make_config(directory, expected.settings)};
        binsrv::gtids::gtid_set purged;
        purged += tc.draw("purged_gtid", gtids());
        {
          auto storage{make_storage(*config)};
          storage->set_purged_gtids(purged);
          std::ignore = storage->open_binlog(
              binsrv::events::composite_binlog_name{"binlog", 1U});
          const auto data{
              tc.draw("event", gs::binary({.min_size = 1U, .max_size = 32U}))};
          storage->write_event(cspan(data), true, tc.draw("gtid", gtids()),
                               util::ctime_timestamp{1}, 1U);
          pbt::require_equal(storage->get_purged_gtids(), purged,
                             "get_purged_gtids() before the restart");
        }
        const auto reopened{make_storage(*config)};
        pbt::require_equal(reopened->get_purged_gtids(), purged,
                           "get_purged_gtids() after the restart");
      },
      {.test_cases = 100U});
}

// ===========================================================================
// indexed_event_block (splits blocks fetched from storage into events for
// the 'pull' replication source, src/operations/sender_context.cpp)
// ===========================================================================
namespace {

// an event of the given total size with event_size in its common header
bytes event_of_size(std::size_t size, std::uint8_t fill) {
  bytes result(std::max<std::size_t>(size, 19U), fill);
  result.at(4U) = 2U; // QUERY
  for (std::size_t i{0U}; i < 4U; ++i) {
    result.at(9U + i) = static_cast<std::uint8_t>(size >> (8U * i));
  }
  return result;
}

util::dynamic_byte_buffer to_buffer(const bytes &data) {
  util::dynamic_byte_buffer result(data.size());
  for (std::size_t i{0U}; i < data.size(); ++i) {
    result[i] = std::byte{data[i]};
  }
  return result;
}

} // namespace

// A block that starts at an event boundary is split into exactly its
// complete events; the bytes of a trailing incomplete event are returned
// as the unparsed tail.
BOOST_AUTO_TEST_CASE(IndexedEventBlockSplitsCompleteEvents) {
  pbt::check([](hegel::TestCase &tc) {
    const auto sizes{tc.draw(
        "event_sizes",
        gs::vectors(gs::integers<std::size_t>({.min_value = 19U,
                                               .max_value = 80U}),
                    {.max_size = 6U}))};
    bytes block;
    for (std::size_t i{0U}; i < sizes.size(); ++i) {
      const auto event{event_of_size(sizes[i], static_cast<std::uint8_t>(i))};
      block.insert(block.end(), event.begin(), event.end());
    }
    const auto complete_size{block.size()};
    const auto tail_event_size{tc.draw(
        "tail_event_size",
        gs::integers<std::size_t>({.min_value = 19U, .max_value = 80U}))};
    const auto tail_length{tc.draw(
        "tail_length",
        gs::integers<std::size_t>({.max_value = tail_event_size - 1U}))};
    const auto tail_event{event_of_size(tail_event_size, 0xEEU)};
    block.insert(block.end(), tail_event.begin(),
                 tail_event.begin() + static_cast<std::ptrdiff_t>(tail_length));

    const binsrv::indexed_event_block indexed{to_buffer(block)};
    pbt::require_equal(indexed.get_number_of_matching_events(), sizes.size(),
                       "number of complete events");
    pbt::require_equal(indexed.get_parsed_size(), complete_size,
                       "size of the parsed part");
    std::size_t offset{0U};
    for (std::size_t i{0U}; i < sizes.size(); ++i) {
      const auto event{indexed.get_event(i)};
      pbt::require_equal(std::size(event), sizes[i], "event size");
      pbt::require(std::data(event) ==
                       std::data(indexed.get_unparsed_tail_span()) -
                           static_cast<std::ptrdiff_t>(complete_size - offset),
                   "event position");
      offset += sizes[i];
    }
    pbt::require_equal(std::size(indexed.get_unparsed_tail_span()), tail_length,
                       "unparsed tail size");
  });
}

// Storage data is not validated when it is read back ("TODO: add integrity
// checks" in storage_core.cpp), so a damaged binlog file can hand any bytes
// to indexed_event_block. Splitting must terminate and must only index
// events at least as long as a common header (19 bytes): an event_size of
// 0 cannot advance the parser, 1..18 would split the block inside a header.
// Splitting runs in a forked child with a 50 ms timer, so that an endless
// loop is reported (instead of exhausting memory) and never touches the
// test harness.
BOOST_AUTO_TEST_CASE(IndexedEventBlockTerminatesOnAnyInput) {
  enum : int {
    child_ok = 0,
    child_beyond_block = 1,
    child_short_event = 2,
    child_out_of_memory = 3
  };
  pbt::check(
      [](hegel::TestCase &tc) {
        // at least one common header (shorter blocks are never parsed)
        auto block{
            tc.draw("block", gs::binary({.min_size = 19U, .max_size = 120U}))};
        // a valid type code, so that the header is not rejected
        block.at(4U) = 2U;
        if (tc.draw("small_event_size", gs::booleans())) {
          // an event_size field below the common header size
          const auto event_size{tc.draw(
              "event_size", gs::integers<std::uint32_t>({.max_value = 18U}))};
          for (std::size_t i{0U}; i < 4U; ++i) {
            block[9U + i] = static_cast<std::uint8_t>(event_size >> (8U * i));
          }
        }
        tc.note("block = " + hex(block));
        const auto buffer{to_buffer(block)};
        const pid_t child{::fork()};
        if (child == 0) {
          // Boost.Test installs its own signal handlers (and would continue
          // running the suite in the child): restore the default actions
          for (const int signal_number : {SIGALRM, SIGABRT, SIGSEGV, SIGBUS,
                                          SIGFPE, SIGILL}) {
            std::ignore = std::signal(signal_number, SIG_DFL);
          }
          const itimerval timer{{0, 0}, {0, 50000}};
          ::setitimer(ITIMER_REAL, &timer, nullptr);
          int code{child_ok};
          try {
            const binsrv::indexed_event_block indexed{buffer};
            if (indexed.get_parsed_size() > buffer.size()) {
              code = child_beyond_block;
            }
            for (std::size_t i{0U}; i < indexed.get_number_of_matching_events(); ++i) {
              if (std::size(indexed.get_event(i)) < 19U) {
                code = child_short_event;
              }
            }
          } catch (const std::bad_alloc &) {
            code = child_out_of_memory;
          } catch (...) {
            // rejecting a damaged block with an exception is fine
            code = child_ok;
          }
          ::_exit(code);
        }
        pbt::require(child > 0, "fork() failed");
        int status{0};
        ::waitpid(child, &status, 0);
        pbt::require(!(WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM),
                     "indexed_event_block does not terminate");
        if (WIFSIGNALED(status)) {
          pbt::fail("indexed_event_block crashed with signal " +
                    std::to_string(WTERMSIG(status)));
        }
        const int code{WEXITSTATUS(status)};
        pbt::require(code != child_out_of_memory,
                     "indexed_event_block exhausts memory");
        pbt::require(code != child_short_event,
                     "indexed_event_block indexed an event shorter than a "
                     "common header");
        pbt::require(code != child_beyond_block,
                     "parsed part larger than the block");
      },
      {.test_cases = 300U});
}

// ===========================================================================
// recovery after a crash (process killed, no destructor)
// ===========================================================================
namespace {

fs::path vault_path(const temporary_directory &directory,
                    const std::string &name) {
  return directory.path() / "vault" / name;
}

void append_bytes(const fs::path &path, const bytes &data) {
  std::ofstream output{path, std::ios::binary | std::ios::app};
  output.write(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<const char *>(data.data()),
      static_cast<std::streamsize>(data.size()));
}

void write_text(const fs::path &path, const std::string &text) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output << text;
}

std::string read_text(const fs::path &path) {
  const auto content{read_file(path)};
  return {content.begin(), content.end()};
}

} // namespace

// The binlog data is written before its metadata (size, GTIDs), so a crash
// in between leaves the last binlog file longer than its metadata says.
// storage_core recovers this by truncating the file (storage_core.cpp,
// "performing recovery"): after reopening, the records, the file and the
// data read back are those of the last completed write.
BOOST_AUTO_TEST_CASE(TailBinlogWithUnrecordedBytesIsTruncated) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const temporary_directory directory;
        storage_model expected;
        expected.settings.gtid_mode = tc.draw("gtid_mode", gs::booleans());
        const auto config{make_config(directory, expected.settings)};
        populate(*config, expected, tc,
                 tc.draw("files", gs::integers<std::size_t>(
                                      {.min_value = 1U, .max_value = 3U})));
        append_bytes(
            vault_path(directory, expected.files.back().name.str()),
            tc.draw("unrecorded", gs::binary({.min_size = 1U, .max_size = 64U})));
        const auto reopened{make_storage(*config)};
        check_reported_state(*reopened, expected);
        check_persisted_state(directory, *reopened, expected);
      },
      {.test_cases = 200U});
}

// Size mismatches that a crash cannot produce are reported instead of
// "recovered": a binlog other than the last one that differs from its
// metadata, or a last binlog shorter than its metadata.
BOOST_AUTO_TEST_CASE(InconsistentBinlogSizeIsRejected) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const temporary_directory directory;
        storage_model expected;
        expected.settings.gtid_mode = tc.draw("gtid_mode", gs::booleans());
        const auto config{make_config(directory, expected.settings)};
        populate(*config, expected, tc,
                 tc.draw("files", gs::integers<std::size_t>(
                                      {.min_value = 1U, .max_value = 3U})));
        const auto index{tc.draw(
            "file", gs::integers<std::size_t>(
                        {.max_value = expected.files.size() - 1U}))};
        const auto path{vault_path(directory, expected.files[index].name.str())};
        const bool last{index + 1U == expected.files.size()};
        if (last || tc.draw("shorten", gs::booleans())) {
          fs::resize_file(path, tc.draw("new_size",
                                        gs::integers<std::uintmax_t>(
                                            {.max_value = fs::file_size(path) - 1U})));
        } else {
          append_bytes(path, tc.draw("extra", gs::binary({.min_size = 1U,
                                                          .max_size = 16U})));
        }
        pbt::require_throws<std::logic_error>(
            [&] { std::ignore = make_storage(*config); },
            "inconsistent binlog size must be rejected");
      },
      {.test_cases = 200U});
}

// Creating a binlog (storage_core::open_new_binlog_file_internal) takes
// three steps: create the binlog file (and write the magic bytes), save its
// metadata, save binlog.index. A process killed between them (e.g. during
// one of the fsyncs at a binlog rotation) leaves a binlog file, with or
// without metadata, that binlog.index does not list. Restarting must still
// work: the binlogs that were complete keep their data, and the source can
// send the interrupted binlog again (open_binlog() of the next name).
namespace {

void crash_while_creating_binlog(hegel::TestCase &tc, bool skip_numbers) {
  const temporary_directory directory;
  storage_model expected;
  expected.settings.gtid_mode = tc.draw("gtid_mode", gs::booleans());
  const auto config{make_config(directory, expected.settings)};
  const auto number_of_files{tc.draw(
      "files", gs::integers<std::size_t>({.min_value = skip_numbers ? 1U : 0U,
                                          .max_value = 3U}))};
  populate(*config, expected, tc, number_of_files);
  // the sequence number of the interrupted binlog: the next one, or
  // (skip_numbers) a later one, as a GTID-mode source may send after
  // binlogs without transactions
  const auto gap{skip_numbers
                     ? tc.draw("gap", gs::integers<std::size_t>({.min_value = 2U,
                                                                 .max_value = 4U}))
                     : std::size_t{1U}};

  const auto index_path{vault_path(directory, "binlog.index")};
  const bool had_index{fs::exists(index_path)};
  const auto index_before{had_index ? read_text(index_path) : ""};
  const binsrv::events::composite_binlog_name interrupted{
      "binlog", static_cast<std::uint32_t>(number_of_files + gap)};
  tc.note("interrupted creation of " + interrupted.str() + " after " +
          std::to_string(number_of_files) + " complete binlog(s)");
  {
    auto storage{make_storage(*config)};
    std::ignore = storage->open_binlog(interrupted);
  }
  // undo the steps the crash prevented
  enum class crash_point : std::uint8_t {
    after_create, after_magic, after_metadata };
  const auto point{tc.draw(
      "crash_point",
      gs::sampled_from<crash_point>({crash_point::after_create,
                                     crash_point::after_magic,
                                     crash_point::after_metadata}))};
  tc.note(std::string{"crash point: "} +
          (point == crash_point::after_create
               ? "after creating the file"
               : (point == crash_point::after_magic
                      ? "after writing the magic bytes"
                      : "after saving the metadata")));
  if (had_index) {
    write_text(index_path, index_before);
  } else {
    fs::remove(index_path);
  }
  if (point != crash_point::after_metadata) {
    fs::remove(vault_path(directory, interrupted.str() + ".json"));
  }
  if (point == crash_point::after_create) {
    fs::resize_file(vault_path(directory, interrupted.str()), 0U);
  }

  std::unique_ptr<binsrv::storage> reopened;
  try {
    reopened = make_storage(*config);
  } catch (const std::exception &e) {
    pbt::fail(std::string{"storage cannot be reopened after a crash "
                          "while creating a binlog: "} +
              e.what());
  }
  const auto records{reopened->get_binlog_records()};
  pbt::require(records.size() == expected.files.size() ||
                   (records.size() == expected.files.size() + 1U &&
                    records.back().name == interrupted &&
                    records.back().size <=
                        binsrv::events::magic_binlog_offset),
               "the complete binlogs and at most the empty interrupted "
               "one");
  for (std::size_t i{0U}; i < expected.files.size(); ++i) {
    pbt::require_equal(records[i].size,
                       std::uint64_t{expected.files[i].flushed.size()},
                       "complete binlog kept");
  }
  try {
    std::ignore = reopened->open_binlog(interrupted);
  } catch (const std::exception &e) {
    pbt::fail(std::string{"the interrupted binlog cannot be opened "
                          "again: "} +
              e.what());
  }
}

} // namespace

BOOST_AUTO_TEST_CASE(CrashWhileCreatingBinlogIsRecovered) {
  pbt::check([](hegel::TestCase &tc) { crash_while_creating_binlog(tc, false); },
             {.test_cases = 200U});
}

// The same when the interrupted binlog does not follow the last stored one
// directly: in GTID mode MySQL starts an auto-positioned dump at the newest
// binlog whose PREVIOUS_GTIDS the storage covers, so after binlogs without
// transactions (FLUSH BINARY LOGS on an idle source) PBS creates e.g.
// binlog.000003 after binlog.000001. A crash or a full disk while creating
// it must not leave a storage that cannot be opened (PBT-062: the recovery
// only recognises the next sequence number, Jenkins #29).
BOOST_AUTO_TEST_CASE(CrashWhileCreatingLaterBinlogIsRecovered) {
  pbt::check([](hegel::TestCase &tc) { crash_while_creating_binlog(tc, true); },
             {.test_cases = 100U});
}

// In querying_only mode (list / search operations running next to the
// collector) binlogs whose metadata is missing or unreadable are skipped
// ("this allows to query an otherwise unusable storage",
// storage_core.cpp); the others are reported normally - also when no
// binlog has readable metadata.
BOOST_AUTO_TEST_CASE(QueryingOnlyModeSkipsBrokenMetadata) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const temporary_directory directory;
        storage_model expected;
        expected.settings.gtid_mode = tc.draw("gtid_mode", gs::booleans());
        const auto config{make_config(directory, expected.settings)};
        populate(*config, expected, tc,
                 tc.draw("files", gs::integers<std::size_t>(
                                      {.min_value = 1U, .max_value = 3U})));
        std::vector<std::string> intact;
        for (const auto &file : expected.files) {
          const auto metadata{vault_path(directory, file.name.str() + ".json")};
          switch (tc.draw("damage", gs::integers<int>({.min_value = 0,
                                                       .max_value = 2}))) {
          case 0:
            intact.push_back(file.name.str());
            break;
          case 1:
            fs::remove(metadata);
            break;
          default:
            write_text(metadata, "{");
            break;
          }
        }
        tc.note("intact: " + std::to_string(intact.size()) + " of " +
                std::to_string(expected.files.size()));
        const auto storage{make_storage(
            *config, binsrv::storage_construction_mode_type::querying_only)};
        const auto records{storage->get_binlog_records()};
        std::vector<std::string> names;
        for (const auto &record : records) {
          names.push_back(record.name.str());
        }
        pbt::require(names == intact,
                     "records are the binlogs with readable metadata");
      },
      {.test_cases = 200U});
}

// ===========================================================================
// encryption at rest
// ===========================================================================
namespace {

const std::vector<std::string> &kek_ciphers() {
  static const std::vector<std::string> ciphers{
      "AES-128-ECB", "AES-256-ECB", "AES-128-CBC",  "AES-256-CBC",
      "AES-128-GCM", "AES-256-GCM", "AES-256-CTR",  "ARIA-256-GCM",
      "CAMELLIA-128-CBC"};
  return ciphers;
}

const std::vector<std::string> &data_ciphers() {
  static const std::vector<std::string> ciphers{
      "AES-128-CTR",  "AES-192-CTR",      "AES-256-CTR",     "ARIA-128-CTR",
      "ARIA-192-CTR", "CAMELLIA-128-CTR", "CAMELLIA-256-CTR"};
  return ciphers;
}

std::size_t key_size(const std::string &cipher) {
  const EVP_CIPHER *evp{EVP_get_cipherbyname(cipher.c_str())};
  if (evp == nullptr) {
    throw std::invalid_argument{"unknown cipher " + cipher};
  }
  return static_cast<std::size_t>(EVP_CIPHER_get_key_length(evp));
}

// README: an ECB / CBC KEK can only wrap file keys whose length is a
// multiple of 16 bytes (so not the 24-byte keys of 192-bit data ciphers)
bool compatible(const std::string &kek_cipher, const std::string &data_cipher) {
  const bool block_mode{kek_cipher.ends_with("-ECB") ||
                        kek_cipher.ends_with("-CBC")};
  return !block_mode || key_size(data_cipher) % 16U == 0U;
}

keyring_key draw_key(hegel::TestCase &tc, const std::string &id,
                     const std::string &cipher) {
  const auto size{key_size(cipher)};
  return {id, cipher,
          tc.draw("key_" + id, gs::binary({.min_size = size, .max_size = size}))};
}

// a keyring with the KEK "kek" (and an unrelated key) and encryption enabled
storage_settings draw_encryption(hegel::TestCase &tc, bool compatible_only) {
  storage_settings settings;
  const auto kek_cipher{tc.draw("kek_cipher", gs::sampled_from(kek_ciphers()))};
  auto data_cipher{tc.draw("data_cipher", gs::sampled_from(data_ciphers()))};
  if (compatible_only && !compatible(kek_cipher, data_cipher)) {
    data_cipher = "AES-256-CTR";
  }
  settings.keyring.push_back(draw_key(tc, "other", "AES-256-GCM"));
  settings.keyring.push_back(draw_key(tc, "kek", kek_cipher));
  settings.kek_id = "kek";
  settings.data_cipher = data_cipher;
  return settings;
}

} // namespace

// The stateful storage model with storage.encryption enabled, for every
// supported combination of KEK cipher (ECB, CBC, GCM, CTR; AES, ARIA,
// CAMELLIA) and data cipher (CTR, 128 / 192 / 256 bits). Everything the
// storage reports is unchanged by encryption, the files on disk are not the
// plaintext, decrypting them independently (OpenSSL + the envelopes in the
// binlog metadata) gives exactly the flushed data, and fetch_event_block()
// returns the plaintext for any range - also for binlogs appended to after a
// restart (CTR encryption continued at an offset).
BOOST_AUTO_TEST_CASE(EncryptedStorageMatchesModel) {
  pbt::check(
      [](hegel::TestCase &tc) {
        run_storage_model(tc, draw_encryption(tc, true));
      },
      {.test_cases = 300U});
}

// An ECB / CBC KEK cannot wrap a 24-byte file key, so such a KEK together
// with a 192-bit data cipher is rejected when the storage is created
// (README "storage.encryption"), not at the first binlog.
BOOST_AUTO_TEST_CASE(IncompatibleKekAndDataCipherIsRejected) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const temporary_directory directory;
        auto settings{draw_encryption(tc, false)};
        settings.keyring.back() = draw_key(
            tc, "kek",
            tc.draw("block_kek_cipher",
                    gs::sampled_from<std::string>(
                        {"AES-128-ECB", "AES-256-ECB", "AES-128-CBC",
                         "AES-256-CBC", "CAMELLIA-128-CBC"})));
        settings.data_cipher = tc.draw(
            "data_cipher_192",
            gs::sampled_from<std::string>({"AES-192-CTR", "ARIA-192-CTR"}));
        const auto config{make_config(directory, settings)};
        pbt::require_throws<std::runtime_error>(
            [&] { std::ignore = make_storage(*config); },
            "incompatible KEK / data cipher must be rejected");
      },
      {.test_cases = 50U});
}

// Encryption can be switched on for an existing storage: binlogs keep the
// format they were created with, new binlogs are encrypted, and all of them
// read back. A storage that has encrypted binlogs cannot be opened without a
// keyring that holds their KEK (README: the keyring section "must be present
// if the storage has at least one encrypted binlog file"). Switching it off
// is not silent: see StorageStartedWithoutEncryptionKeepsEncrypting.
BOOST_AUTO_TEST_CASE(EncryptionCanBeEnabledButNeedsTheKek) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const temporary_directory directory;
        storage_model expected;
        auto second{draw_encryption(tc, true)};
        storage_settings first{};
        first.gtid_mode = second.gtid_mode = tc.draw("gtid_mode", gs::booleans());
        expected.settings = first;
        populate(*make_config(directory, first), expected, tc,
                 tc.draw("files_before", gs::integers<std::size_t>(
                                             {.min_value = 1U, .max_value = 2U})));
        expected.settings = second;
        const auto second_config{make_config(directory, second)};
        populate_more(*second_config, expected, tc,
                      tc.draw("files_after", gs::integers<std::size_t>(
                                                 {.min_value = 1U,
                                                  .max_value = 2U})));
        {
          const auto reopened{make_storage(*second_config)};
          check_reported_state(*reopened, expected);
          check_persisted_state(directory, *reopened, expected);
        }

        // without the KEK the encrypted binlogs cannot be used
        storage_settings without{second};
        without.kek_id.clear();
        without.data_cipher.clear();
        if (tc.draw("drop_whole_keyring", gs::booleans())) {
          without.keyring.clear();
        } else {
          std::erase_if(without.keyring,
                        [](const keyring_key &key) { return key.id == "kek"; });
        }
        const auto config_without{make_config(directory, without)};
        pbt::require_throws<std::logic_error>(
            [&] { std::ignore = make_storage(*config_without); },
            "storage with encrypted binlogs must not open without their KEK");
      },
      {.test_cases = 100U});
}

// A storage whose binlogs are encrypted, started with the keyring (needed to
// read them) but without a storage.encryption section - a configuration
// template or config-management change that drops the section - must not
// silently write the next binlogs in plaintext: it either refuses to start
// or keeps encrypting them (with a KEK of the keyring), and they read back.
// Turning encryption off has to be an explicit decision. Hrvoje's ticket 54:
// validate_metadata() compares the configured encryption format with the
// one in metadata.json only when both are set, and
// generate_binlog_encryption_record() returns nothing without an active
// KEK, so metadata.json keeps saying "generic" while new binlogs are
// plaintext, and nothing is logged.
BOOST_AUTO_TEST_CASE(StorageStartedWithoutEncryptionKeepsEncrypting) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const temporary_directory directory;
        storage_model expected;
        auto encryption{draw_encryption(tc, true)};
        encryption.gtid_mode = tc.draw("gtid_mode", gs::booleans());
        expected.settings = encryption;
        populate(*make_config(directory, encryption), expected, tc,
                 tc.draw("files_before", gs::integers<std::size_t>(
                                             {.min_value = 1U, .max_value = 2U})));

        storage_settings without{encryption};
        without.kek_id.clear();
        without.data_cipher.clear();
        const auto config_without{make_config(directory, without)};
        try {
          std::ignore = make_storage(*config_without);
        } catch (const std::exception &error) {
          pbt::note(tc, std::string{"refused to start: "} + error.what());
          return;
        }

        const auto before{expected.files.size()};
        expected.settings = without;
        populate_more(*config_without, expected, tc,
                      tc.draw("files_after", gs::integers<std::size_t>(
                                                 {.min_value = 1U,
                                                  .max_value = 2U})));
        for (std::size_t i{before}; i < expected.files.size(); ++i) {
          const auto &file{expected.files[i]};
          bool encrypted{false};
          const auto plaintext{
              stored_plaintext(directory, encryption, file.name.str(), encrypted)};
          pbt::require(encrypted,
                       "started without an 'encryption' section but with the "
                       "keyring, the storage of encrypted binlogs wrote " +
                           file.name.str() + " in plaintext without an error");
          pbt::require_equal(hex(plaintext), hex(file.flushed),
                             "the binlog written after the restart reads back");
        }
      },
      {.test_cases = 100U});
}

// Integrity of encrypted binlogs (Hrvoje's ticket 53, PBS-126). Storage
// encryption is AES/ARIA/CAMELLIA-CTR without a MAC, and the binlog
// metadata is not authenticated either: someone who can write the storage
// but has no key can change the data (XOR into the ciphertext XORs the
// plaintext; the CRC32 footer can be patched to match because CRC32 is
// affine) or replace a binlog with plaintext of their choosing, and PBS
// serves it. A change to encrypted data must be detected when it is read.
namespace {

void write_file(const fs::path &path, const bytes &data) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output.write(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<const char *>(data.data()),
      static_cast<std::streamsize>(data.size()));
}

// the binlog's data after the magic as a restarted storage returns it;
// nullopt (and 'error') when PBS reports an error (the storage cannot be
// opened, fetch_event_block() throws or does not return success_full)
std::optional<bytes> read_back(const binsrv::main_config &config,
                               const binsrv::events::composite_binlog_name &name,
                               std::uint64_t size, std::string &error) {
  try {
    const auto storage{make_storage(config)};
    const util::byte_range range{binsrv::events::magic_binlog_offset,
                                 size - binsrv::events::magic_binlog_offset};
    util::dynamic_byte_buffer buffer;
    const auto result{storage->fetch_event_block(name, range, buffer)};
    if (result != binsrv::storage_fetch_result_type::success_full) {
      error = "fetch_event_block() did not return success_full";
      return std::nullopt;
    }
    return to_bytes(buffer);
  } catch (const std::exception &exception) {
    error = exception.what();
    return std::nullopt;
  }
}

// an encrypted storage (any compatible KEK / data cipher) with binlogs
// binlog.000001 .. binlog.<files>, each holding one event
storage_model encrypted_storage(hegel::TestCase &tc,
                                const temporary_directory &directory,
                                std::size_t min_files, std::size_t max_files) {
  storage_model expected;
  auto settings{draw_encryption(tc, true)};
  settings.gtid_mode = tc.draw("gtid_mode", gs::booleans());
  expected.settings = settings;
  populate_more(*make_config(directory, settings), expected, tc,
                tc.draw("files", gs::integers<std::size_t>(
                                     {.min_value = min_files, .max_value = max_files})));
  return expected;
}

} // namespace

// 1 - 4 consecutive bytes after the magic of an encrypted binlog XORed with
// a non-zero mask in the stored (encrypted) file - what an attacker without
// the keyring can do knowing only where the events lie: a restarted
// storage must not return the changed data without an error.
BOOST_AUTO_TEST_CASE(TamperedEncryptedBinlogIsDetected) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const temporary_directory directory;
        const auto expected{encrypted_storage(tc, directory, 1U, 2U)};
        const auto config{make_config(directory, expected.settings)};
        const auto &file{expected.files[tc.draw(
            "binlog", gs::integers<std::size_t>(
                          {.max_value = expected.files.size() - 1U}))]};
        const auto size{file.flushed.size()};
        const auto offset{tc.draw(
            "offset", gs::integers<std::size_t>(
                          {.min_value = binsrv::events::magic_binlog_offset,
                           .max_value = size - 1U}))};
        const auto count{tc.draw(
            "count", gs::integers<std::size_t>(
                         {.min_value = 1U, .max_value = std::min<std::size_t>(4U, size - offset)}))};
        const auto mask{static_cast<std::uint8_t>(
            tc.draw("mask", gs::integers<int>({.min_value = 1, .max_value = 255})))};
        const auto path{vault_path(directory, file.name.str())};
        auto data{read_file(path)};
        pbt::require_equal(data.size(), size, "stored size of " + file.name.str());
        for (std::size_t i{0U}; i < count; ++i) {
          data[offset + i] ^= mask;
        }
        write_file(path, data);
        const std::string change{std::to_string(count) + " byte(s) XORed with " +
                                 hex(bytes{mask}) + " at offset " + std::to_string(offset) +
                                 " of " + file.name.str() + " (" +
                                 std::to_string(size) + " bytes, data cipher " +
                                 expected.settings.data_cipher + ")"};
        pbt::note(tc, "changed: " + change);
        std::string error;
        const auto read{read_back(*config, file.name, size, error)};
        if (!read) {
          pbt::note(tc, "detected: " + error);
          return;
        }
        const bytes original(file.flushed.begin() + 4, file.flushed.end());
        pbt::require(*read == original,
                     "a change to encrypted binlog data was read back without an "
                     "error (" + change + ")");
      },
      {.test_cases = 100U});
}

// A binlog after an encrypted one replaced with plaintext of the attacker's
// choosing (same size) and its 'encryption' record removed from its
// metadata: no key, no keystream, no CRC32 arithmetic needed. In a storage
// whose binlogs were encrypted before it, an unencrypted binlog is never
// legitimate (switching encryption off is not supported silently, see
// StorageStartedWithoutEncryptionKeepsEncrypting), so a restarted storage
// must not serve it without an error. (A storage that had binlogs before
// encryption was enabled legitimately has unencrypted ones at its start.)
BOOST_AUTO_TEST_CASE(EncryptedBinlogStrippedOfEncryptionIsNotServed) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const temporary_directory directory;
        const auto expected{encrypted_storage(tc, directory, 2U, 3U)};
        const auto config{make_config(directory, expected.settings)};
        const auto &file{expected.files[tc.draw(
            "binlog", gs::integers<std::size_t>(
                          {.min_value = 1U, .max_value = expected.files.size() - 1U}))]};
        const auto size{file.flushed.size()};
        auto forged{magic_payload()};
        const auto payload{tc.draw("forged_events",
                                   gs::binary({.min_size = size - 4U, .max_size = size - 4U}))};
        forged.insert(forged.end(), payload.begin(), payload.end());
        write_file(vault_path(directory, file.name.str()), forged);
        const auto metadata_path{vault_path(directory, file.name.str() + ".json")};
        auto metadata{boost::json::parse(read_text(metadata_path)).as_object()};
        pbt::require(metadata.erase("encryption") == 1U,
                     "oracle: the metadata of " + file.name.str() +
                         " has an encryption record");
        write_text(metadata_path, boost::json::serialize(metadata));
        pbt::note(tc, file.name.str() + " (after an encrypted binlog) replaced with " +
                          std::to_string(size) +
                          " bytes of plaintext, its encryption record removed");
        std::string error;
        const auto read{read_back(*config, file.name, size, error)};
        if (!read) {
          pbt::note(tc, "detected: " + error);
          return;
        }
        pbt::require(*read != payload,
                     "a binlog stripped of its encryption was served as plaintext "
                     "without an error");
      },
      {.test_cases = 100U});
}

// ===========================================================================
// S3 backend (SeaweedFS)
// ===========================================================================
namespace {

// storage.uri for a fresh prefix in the S3 bucket given by PBT_S3_* (set by
// run_property_tests.py, which starts SeaweedFS with extra/qa/seaweedfs.sh)
std::string s3_uri_for(const temporary_directory &directory) {
  const auto endpoint{pbt::detail::env("PBT_S3_ENDPOINT")};
  const auto access_key{pbt::detail::env("PBT_S3_ACCESS_KEY")};
  const auto secret_key{pbt::detail::env("PBT_S3_SECRET_KEY")};
  const auto bucket{pbt::detail::env("PBT_S3_BUCKET")};
  if (!endpoint || !access_key || !secret_key || !bucket) {
    pbt::infrastructure_error(
        "S3 endpoint not configured: set PBT_S3_ENDPOINT / _ACCESS_KEY / "
        "_SECRET_KEY / _BUCKET (extra/qa/seaweedfs.sh env)");
  }
  const auto scheme_end{endpoint->find("://")};
  if (scheme_end == std::string::npos) {
    pbt::infrastructure_error("PBT_S3_ENDPOINT must be http[s]://host:port");
  }
  // README: http[s]://key:secret@host[:port]/bucket/path
  return endpoint->substr(0U, scheme_end + 3U) + *access_key + ":" +
         *secret_key + "@" + endpoint->substr(scheme_end + 3U) + "/" +
         *bucket + "/" + directory.path().filename().string();
}

} // namespace

// The stateful storage model on the S3 backend: the same operation sequences
// give the same observable state as on the filesystem (records, positions,
// GTIDs, data read back after every restart and for random ranges), with
// and without encryption. Every case uses its own prefix in the bucket.
BOOST_AUTO_TEST_CASE(StorageMatchesModelOnS3) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const temporary_directory prefix_source;
        storage_settings settings{};
        if (tc.draw("encrypted", gs::booleans())) {
          settings = draw_encryption(tc, true);
        }
        settings.s3_uri = s3_uri_for(prefix_source);
        // every write is a whole-object upload: fewer, shorter sequences
        run_storage_model(tc, settings, 15U);
      },
      {.test_cases = 20U, .slow = true});
}

// ===========================================================================
// checkpoint interval
// ===========================================================================

// storage.checkpoint_interval accepts any 64-bit number of seconds
// (time_unit, README "/\d+[smhd]?/"). A complete transaction written right
// after opening a binlog must not be flushed by the interval checkpoint
// before the interval has elapsed - also for intervals so long that they
// do not fit into std::chrono::steady_clock::duration (nanoseconds, about
// 292 years). Interval checkpoints themselves need a clock seam (todo.md
// T2); this only checks the "not yet" side, which needs no waiting.
BOOST_AUTO_TEST_CASE(LongCheckpointIntervalDoesNotFlushEarly) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const temporary_directory directory;
        storage_settings settings;
        settings.checkpoint_interval = tc.draw(
            "checkpoint_interval",
            gs::sampled_from<std::string>(
                {"1h", "30d", "36500d", "106751d", "106752d", "1000000d",
                 "9223372036s", "9223372037s", "9223372036854775807s",
                 "9223372036854775808s", "18446744073709551615s",
                 "213503982334601d"}));
        const auto config{make_config(directory, settings)};
        const auto storage{make_storage(*config)};
        const binsrv::events::composite_binlog_name name{"binlog", 1U};
        std::ignore = storage->open_binlog(name);
        const auto data{
            tc.draw("event", gs::binary({.min_size = 1U, .max_size = 32U}))};
        storage->write_event(cspan(data), true, {}, util::ctime_timestamp{1},
                             1U);
        const auto records{storage->get_binlog_records()};
        pbt::require_equal(records.back().size,
                           binsrv::events::magic_binlog_offset,
                           "transaction flushed before the checkpoint "
                           "interval elapsed");
      },
      {.test_cases = 100U});
}

// ---------------------------------------------------------------------------
// encryption: keystream reuse after crash recovery
// ---------------------------------------------------------------------------

// The data of an encrypted binlog is AES-CTR (or ARIA / CAMELLIA CTR)
// encrypted at its byte offset with one key and IV per file. A writer that
// stops after appending data but before saving the binlog's metadata (a
// crash) leaves an unrecorded tail; the next start truncates it and appends
// again at the same offset. When the new data differs from the lost tail,
// the same keystream must not encrypt both: whoever has both ciphertexts
// (an S3 object version, a snapshot or backup taken before the crash)
// learns the XOR of the two plaintexts (Hrvoje's ticket 32).
//
// The crash is simulated: transactions A are written and recorded, then B
// is written and recorded and the binlog's metadata is put back to its
// state after A (data written, metadata not saved). After the reopen
// (recovery truncates B), C is written - different from B. The keystream at
// each overlapping offset is ciphertext XOR plaintext (the plaintext is
// what was written); the two keystreams must differ.
BOOST_AUTO_TEST_CASE(RecoveredEncryptedBinlogNeverReusesKeystream) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const temporary_directory directory;
        auto settings{draw_encryption(tc, true)};
        const auto config{make_config(directory, settings)};
        const binsrv::events::composite_binlog_name name{"binlog", 1U};
        const auto data_path{vault_path(directory, name.str())};
        const auto metadata_path{vault_path(directory, name.str() + ".json")};
        const auto write_all{[&](const std::vector<bytes> &events) {
          auto storage{make_storage(*config)};
          std::ignore = storage->open_binlog(name);
          std::uint64_t sequence{1U};
          for (const auto &event : events) {
            storage->write_event(cspan(event), true, binsrv::gtids::gtid{},
                                 util::ctime_timestamp{7}, sequence++);
          }
          storage.reset();  // flushes and saves the metadata
        }};
        const auto draw_events{[&tc](const std::string &label) {
          const auto count{tc.draw(label + "_count",
                                   gs::integers<std::size_t>({.min_value = 1U, .max_value = 3U}))};
          std::vector<bytes> events;
          for (std::size_t i{0U}; i < count; ++i) {
            events.push_back(tc.draw(label, gs::binary({.min_size = 4U, .max_size = 48U})));
          }
          return events;
        }};
        const auto joined{[](const std::vector<bytes> &events) {
          bytes result;
          for (const auto &event : events) {
            result.insert(result.end(), event.begin(), event.end());
          }
          return result;
        }};

        write_all(draw_events("a"));
        const auto size_a{fs::file_size(data_path)};
        const auto metadata_a{read_text(metadata_path)};
        const auto b_events{draw_events("b")};
        write_all(b_events);
        const auto ciphertext_b{read_file(data_path)};
        const auto b{joined(b_events)};
        pbt::require_equal(ciphertext_b.size(), size_a + b.size(),
                           "binlog size after writing B");

        // the crash: B's data is in the file, the metadata is still A's
        write_text(metadata_path, metadata_a);
        auto c_events{draw_events("c")};
        auto c{joined(c_events)};
        const auto overlap{std::min(b.size(), c.size())};
        // C differs from B in at least its first 4 bytes, so that a fixed
        // storage fails the check only by chance (2^-32)
        for (std::size_t i{0U}; i < 4U; ++i) {
          if (c_events.front()[i] == b[i]) {
            c_events.front()[i] = static_cast<std::uint8_t>(b[i] ^ 0x5aU);
          }
        }
        c = joined(c_events);
        tc.note("A: " + std::to_string(size_a) + " bytes with the magic; B: " + hex(b) +
                "; C: " + hex(c));
        write_all(c_events);
        const auto ciphertext_c{read_file(data_path)};
        pbt::require_equal(ciphertext_c.size(), size_a + c.size(),
                           "binlog size after recovery and writing C");

        std::size_t differing{0U};
        std::size_t reused{0U};
        for (std::size_t i{0U}; i < overlap; ++i) {
          if (b[i] == c[i]) {
            continue;
          }
          ++differing;
          const auto keystream_b{static_cast<std::uint8_t>(ciphertext_b[size_a + i] ^ b[i])};
          const auto keystream_c{static_cast<std::uint8_t>(ciphertext_c[size_a + i] ^ c[i])};
          if (keystream_b == keystream_c) {
            ++reused;
          }
        }
        tc.note(std::to_string(reused) + " of " + std::to_string(differing) +
                " differing byte(s) encrypted with the same keystream byte");
        pbt::require(differing == 0U || reused < differing,
                     "after crash recovery new data was encrypted with the keystream of the "
                     "lost data at the same offset (" + std::to_string(reused) + " of " +
                     std::to_string(differing) + " differing bytes)");
      },
      {.test_cases = 100U});
}

// A purge whose delete of one purged binlog's data or metadata file fails
// (a filesystem error, a failed or timed-out S3 DELETE; here the object is
// replaced by a non-empty directory of the same name for the purge only)
// is the supported retention procedure going wrong once: purge_binlogs
// reports a warning (or an error), the file is left behind, the writer is
// started again. The storage must open, hold either the binlogs before or
// the binlogs after the purge (never serve a purged binlog's leftovers),
// and keep accepting new binlogs. Hrvoje's ticket 33 (fixed upstream in
// 9989a4a, PBS-59: leftovers of a purge are removed at the next start);
// InterruptedPurgeIsRecoveredMySQL covers a purge killed during its deletes.
BOOST_AUTO_TEST_CASE(PurgeWithFailedDeleteIsRecovered) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const temporary_directory directory;
        storage_model before;
        before.settings.gtid_mode = tc.draw("gtid_mode", gs::booleans());
        const auto config{make_config(directory, before.settings)};
        populate_more(*config, before, tc,
                      tc.draw("files", gs::integers<std::size_t>(
                                           {.min_value = 2U, .max_value = 4U})));
        const auto target_index{tc.draw(
            "target_index",
            gs::integers<std::size_t>({.max_value = before.files.size() - 2U}))};
        const auto &victim_file{before.files[tc.draw(
            "victim_binlog", gs::integers<std::size_t>({.max_value = target_index}))]};
        const bool metadata{tc.draw("victim_is_metadata", gs::booleans())};
        const auto victim{vault_path(directory, victim_file.name.str() +
                                                    (metadata ? ".json" : ""))};
        const auto content{read_file(victim)};
        fs::remove(victim);
        fs::create_directory(victim);
        write_file(victim / "blocker", bytes{1U});
        storage_model after{before};
        after.files.erase(after.files.begin(),
                          after.files.begin() +
                              static_cast<std::ptrdiff_t>(target_index + 1U));
        const auto target{before.files[target_index].name};

        std::string outcome;
        bool threw{false};
        try {
          auto purging{make_storage(*config, binsrv::storage_construction_mode_type::purging)};
          const auto [removed, warning]{purging->purge_binlogs(target)};
          outcome = warning.empty() ? std::string{"no warning"} : "warning: " + warning;
        } catch (const std::exception &error) {
          threw = true;
          outcome = std::string{"error: "} + error.what();
        }
        // the failure was transient: the file is back as the purge left it
        fs::remove_all(victim);
        write_file(victim, content);
        pbt::note(tc, "purge up to " + target.str() + " with the delete of " +
                          victim.filename().string() + " failing: " + outcome);

        std::unique_ptr<binsrv::storage> reopened;
        try {
          reopened = make_storage(*config);
        } catch (const std::exception &error) {
          pbt::fail(std::string{"storage cannot be opened after a purge whose delete of "} +
                    victim.filename().string() + " failed: " + error.what());
        }
        // a purge that returned committed the shorter index; one that threw
        // may have done so or not
        auto &expected{[&]() -> storage_model & {
          if (!threw) {
            return after;
          }
          return reopened->get_binlog_records().size() == after.files.size() ? after : before;
        }()};
        check_reported_state(*reopened, expected);
        check_persisted_state(directory, *reopened, expected);
        reopened.reset();
        populate_more(*config, expected, tc, 1U);
        const auto again{make_storage(*config)};
        check_reported_state(*again, expected);
      },
      {.test_cases = 100U});
}

// ===========================================================================
// file security of the filesystem backend (Hrvoje's tickets 56, 57)
// ===========================================================================
namespace {

// the process umask for one scope
class scoped_umask {
public:
  explicit scoped_umask(mode_t mask) : previous_{::umask(mask)} {}
  scoped_umask(const scoped_umask &) = delete;
  scoped_umask &operator=(const scoped_umask &) = delete;
  scoped_umask(scoped_umask &&) = delete;
  scoped_umask &operator=(scoped_umask &&) = delete;
  ~scoped_umask() { ::umask(previous_); }

private:
  mode_t previous_;
};

std::string octal(fs::perms perms) {
  std::ostringstream text;
  text << std::oct << static_cast<unsigned>(perms);
  return text.str();
}

// writes one binlog with one event through an open storage (as the
// collector does); returns the error text when the storage refuses
std::string write_next_binlog(binsrv::storage &storage, storage_model &expected,
                              hegel::TestCase &tc) {
  const auto name{expected.files.back().name.next()};
  try {
    std::ignore = storage.open_binlog(name);
    const auto data{tc.draw("event", gs::binary({.min_size = 1U, .max_size = 32U}))};
    const auto gtid{tc.draw("gtid", gtids())};
    storage.write_event(cspan(data), true, gtid, util::ctime_timestamp{7}, 1U);
    storage.close_binlog();
  } catch (const std::exception &error) {
    return error.what();
  }
  return {};
}

} // namespace

// Storage files are not accessible to other users, whatever the umask:
// with umask 000 / 002 / 022 / 027, after a storage is created and 1 - 3
// binlogs are written (plain or encrypted), no object in the storage
// directory (binlog data, binlog metadata, binlog.index, metadata.json) has
// any permission for others or write permission for the group (MySQL
// creates its binlogs 0640 whatever the umask). Hrvoje's ticket 56: files
// are created 0666 & ~umask - world-readable under 022, world-writable
// under 000.
BOOST_AUTO_TEST_CASE(StorageFilesAreNotWorldAccessible) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto mask{tc.draw("umask", gs::sampled_from<mode_t>({0000, 0002, 0022, 0027}))};
        const scoped_umask umask_scope{mask};
        const temporary_directory directory;
        storage_model expected;
        expected.settings = tc.draw("encrypted", gs::booleans()) ? draw_encryption(tc, true)
                                                                 : storage_settings{};
        expected.settings.gtid_mode = tc.draw("gtid_mode", gs::booleans());
        const auto config{make_config(directory, expected.settings)};
        populate_more(*config, expected, tc,
                      tc.draw("files", gs::integers<std::size_t>(
                                           {.min_value = 1U, .max_value = 3U})));
        std::string exposed;
        for (const auto &entry : fs::directory_iterator{directory.path() / "vault"}) {
          const auto perms{entry.symlink_status().permissions()};
          if ((perms & (fs::perms::others_all | fs::perms::group_write)) != fs::perms::none) {
            exposed += entry.path().filename().string() + " " + octal(perms) + "; ";
          }
        }
        pbt::note(tc, "umask " + octal(static_cast<fs::perms>(mask)));
        pbt::require(exposed.empty(),
                     "storage files accessible to other users (umask " +
                         octal(static_cast<fs::perms>(mask)) + "): " + exposed);
      },
      {.test_cases = 50U});
}

// PBS never writes through a symlink in the storage directory. While a
// storage is open (a running writer) with 1 - 2 binlogs, symlinks are
// planted under the names it writes next - the next binlog, its
// <binlog>.json.tmp, binlog.index.tmp - each pointing at a file outside the
// storage; then the next binlog is written. Either the storage refuses
// (an error), or it writes; in both cases every target file keeps its
// content, and after a successful write binlog.index and the new binlog's
// files are not symlinks. Hrvoje's ticket 57: std::ofstream and the
// write-then-rename helper follow symlinks with predictable names, so an
// account that can write the storage directory can make PBS overwrite any
// file PBS can write.
BOOST_AUTO_TEST_CASE(StorageNeverWritesThroughSymlinks) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const temporary_directory directory;
        storage_model expected;
        expected.settings.gtid_mode = tc.draw("gtid_mode", gs::booleans());
        const auto config{make_config(directory, expected.settings)};
        populate_more(*config, expected, tc,
                      tc.draw("files", gs::integers<std::size_t>(
                                           {.min_value = 1U, .max_value = 2U})));
        auto storage{make_storage(*config)};
        const auto next{expected.files.back().name.next().str()};
        const std::array<std::string, 3U> names{next, next + ".json.tmp", "binlog.index.tmp"};
        constexpr std::array plant_labels{"plant_next_binlog", "plant_metadata_tmp",
                                          "plant_index_tmp"};
        std::vector<std::string> planted;
        for (std::size_t i{0U}; i < names.size(); ++i) {
          if (tc.draw(plant_labels[i], gs::booleans())) {
            planted.push_back(names[i]);
          }
        }
        if (planted.empty()) {
          planted.push_back(
              names[tc.draw("plant_one", gs::integers<std::size_t>({.max_value = 2U}))]);
        }
        const auto outside{directory.path() / "outside"};
        fs::create_directories(outside);
        const bytes original{'k', 'e', 'e', 'p'};
        std::string planted_text;
        for (const auto &name : planted) {
          const auto target{outside / (name + ".target")};
          write_file(target, original);
          fs::create_symlink(target, vault_path(directory, name));
          planted_text += name + " ";
        }
        pbt::note(tc, "symlinks planted while the storage is open: " + planted_text);
        const auto error{write_next_binlog(*storage, expected, tc)};
        storage.reset();
        pbt::note(tc, error.empty() ? std::string{"the next binlog was written"}
                                    : "the storage refused: " + error);
        for (const auto &name : planted) {
          pbt::require(read_file(outside / (name + ".target")) == original,
                       "PBS wrote through a symlink planted in the storage directory (" + name +
                           " -> a file outside the storage)");
        }
        if (error.empty()) {
          for (const auto &name : {std::string{"binlog.index"}, next, next + ".json"}) {
            pbt::require(!fs::is_symlink(vault_path(directory, name)),
                         name + " is a symlink after the next binlog was written");
          }
        }
      },
      {.test_cases = 100U});
}
