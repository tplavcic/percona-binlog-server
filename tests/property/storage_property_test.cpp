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
//    cipher restrictions, switching encryption on / off, the KEK being
//    required;
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
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
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
  std::ostringstream json;
  json << R"({
  "logger": {"level": "error", "file": ""},
  "connection": {"host": "127.0.0.1", "port": 3306, "user": "u",
                 "password": "", "connect_timeout": 1, "read_timeout": 1,
                 "write_timeout": 1},
  "replication": {"server_id": 1, "idle_time": 1, "verify_checksum": true,
                  "mode": ")"
       << (settings.gtid_mode ? "gtid" : "position") << R"("},
  "replication_source": {"port": 3307, "read_timeout": 1, "write_timeout": 1,
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
  json << "}\n}\n";
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
  auto name{file.name};
  // length 0 for a file without events (offset == size would continue into
  // the next file)
  util::byte_range range{binsrv::events::magic_binlog_offset,
                         file.flushed.size() -
                             binsrv::events::magic_binlog_offset};
  util::dynamic_byte_buffer buffer;
  pbt::require(storage.fetch_event_block(name, range, buffer),
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

void run_storage_model(hegel::TestCase &tc, const storage_settings &base) {
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
            "steps", gs::integers<std::size_t>({.min_value = 1U, .max_value = 30U}))};
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
          auto name{file.name};
          util::byte_range range{offset, length};
          util::dynamic_byte_buffer buffer;
          pbt::require(storage->fetch_event_block(name, range, buffer),
                       "fetch_event_block() of a range succeeds");
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

struct fetch_result {
  bool success{};
  std::string name;
  std::uint64_t offset{};
  std::uint64_t length{};
  bytes data;
};

// The contract documented in storage_core.hpp, applied to the model files.
fetch_result expected_fetch(const std::vector<model_file> &files,
                            const fetch_request &request) {
  fetch_result unchanged{false, request.name, request.offset,
                         request.length.value_or(0U), {}};
  if (request.offset < binsrv::events::magic_binlog_offset) {
    return unchanged;
  }
  if (!request.length) {
    return unchanged;
  }
  if (request.name.empty() &&
      request.offset != binsrv::events::magic_binlog_offset) {
    return unchanged;
  }
  if (files.empty()) {
    return {true, request.name, binsrv::events::magic_binlog_offset, 0U, {}};
  }
  std::size_t index{0U};
  if (!request.name.empty()) {
    const auto found{std::ranges::find_if(files, [&](const model_file &file) {
      return file.name.str() == request.name;
    })};
    if (found == files.end()) {
      return unchanged;
    }
    index = static_cast<std::size_t>(found - files.begin());
  }
  if (*request.length == 0U) {
    return {true, files[index].name.str(), request.offset, 0U, {}};
  }
  auto offset{request.offset};
  if (offset > files[index].flushed.size()) {
    return unchanged;
  }
  if (offset == files[index].flushed.size()) {
    ++index;
    if (index == files.size()) {
      return {true, request.name, offset, 0U, {}};
    }
    offset = binsrv::events::magic_binlog_offset;
  }
  const auto &file{files[index]};
  const auto length{std::min<std::uint64_t>(*request.length,
                                            file.flushed.size() - offset)};
  const auto first{file.flushed.begin() + static_cast<std::ptrdiff_t>(offset)};
  return {true, file.name.str(), offset, length,
          bytes(first, first + static_cast<std::ptrdiff_t>(length))};
}

} // namespace

// fetch_event_block() (used by the 'pull' replication source and
// search operations) follows the rules documented in storage_core.hpp for
// every combination of binlog name (empty, existing, missing), offset
// (before / inside / at / after the end of a file) and length (none, 0,
// more than available), including continuing into the next file; on
// failure the arguments are left untouched.
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
          tc.note("fetch " + request_text.str());

          binsrv::events::composite_binlog_name name{};
          if (!request.name.empty()) {
            name = binsrv::events::composite_binlog_name::parse(request.name);
          }
          util::byte_range range{request.offset, request.length};
          util::dynamic_byte_buffer buffer{std::byte{0x5A}};
          const bool success{storage->fetch_event_block(name, range, buffer)};
          pbt::require_equal(success, wanted.success, "return value");
          pbt::require_equal(name.is_empty() ? std::string{} : name.str(),
                             wanted.name, "binlog_name after the call");
          pbt::require_equal(range.get_offset(), wanted.offset,
                             "range offset after the call");
          if (success) {
            pbt::require_equal(range.get_length(), wanted.length,
                               "range length after the call");
            pbt::require_equal(hex(to_bytes(buffer)), hex(wanted.data),
                               "data");
          } else {
            pbt::require_equal(hex(to_bytes(buffer)), std::string{"5a"},
                               "buffer untouched on failure");
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
    pbt::require_equal(indexed.get_number_of_events(), sizes.size(),
                       "number of complete events");
    pbt::require_equal(indexed.get_actual_size(), complete_size,
                       "size of the parsed part");
    std::size_t offset{0U};
    for (std::size_t i{0U}; i < sizes.size(); ++i) {
      const auto event{indexed.get_event(i)};
      pbt::require_equal(std::size(event), sizes[i], "event size");
      pbt::require(std::data(event) ==
                       std::data(indexed.get_unparsed_tail()) -
                           static_cast<std::ptrdiff_t>(complete_size - offset),
                   "event position");
      offset += sizes[i];
    }
    pbt::require_equal(std::size(indexed.get_unparsed_tail()), tail_length,
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
            if (indexed.get_actual_size() > buffer.size()) {
              code = child_beyond_block;
            }
            for (std::size_t i{0U}; i < indexed.get_number_of_events(); ++i) {
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
BOOST_AUTO_TEST_CASE(CrashWhileCreatingBinlogIsRecovered) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const temporary_directory directory;
        storage_model expected;
        expected.settings.gtid_mode = tc.draw("gtid_mode", gs::booleans());
        const auto config{make_config(directory, expected.settings)};
        const auto number_of_files{
            tc.draw("files", gs::integers<std::size_t>({.max_value = 3U}))};
        populate(*config, expected, tc, number_of_files);

        const auto index_path{vault_path(directory, "binlog.index")};
        const bool had_index{fs::exists(index_path)};
        const auto index_before{had_index ? read_text(index_path) : ""};
        const binsrv::events::composite_binlog_name interrupted{
            "binlog", static_cast<std::uint32_t>(number_of_files + 1U)};
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
      },
      {.test_cases = 200U});
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

// Encryption can be switched on or off for an existing storage: binlogs
// keep the format they were created with, new binlogs follow the current
// configuration, and all of them read back. A storage that has encrypted
// binlogs cannot be opened without a keyring that holds their KEK (README:
// the keyring section "must be present if the storage has at least one
// encrypted binlog file").
BOOST_AUTO_TEST_CASE(EncryptionCanBeToggledButNeedsTheKek) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const temporary_directory directory;
        storage_model expected;
        const auto encryption{draw_encryption(tc, true)};
        const bool encrypted_first{tc.draw("encrypted_first", gs::booleans())};
        storage_settings first{encrypted_first ? encryption : storage_settings{}};
        storage_settings second{encrypted_first ? storage_settings{}
                                                : encryption};
        first.gtid_mode = second.gtid_mode = tc.draw("gtid_mode", gs::booleans());
        if (encrypted_first) {
          // the KEK must stay available after encryption is switched off
          second.keyring = encryption.keyring;
        }
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
        run_storage_model(tc, settings);
      },
      {.test_cases = 100U});
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
