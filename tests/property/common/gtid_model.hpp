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

#ifndef TESTS_PROPERTY_COMMON_GTID_MODEL_HPP
#define TESTS_PROPERTY_COMMON_GTID_MODEL_HPP

// An independent reference model of a MySQL GTID set plus Hegel generators
// for GTID components. The model deliberately shares no code with
// src/binsrv/gtids: it keeps closed intervals in a std::map and renders the
// text / binary forms directly from the MySQL documentation:
//  - text:
//  https://dev.mysql.com/doc/refman/8.4/en/replication-gtids-concepts.html
//            (UUIDs in order, intervals merged and ascending, m[-n])
//  - binary: mysql-server 8.4 sql/rpl_gtid_set.cc Gtid_set::encode()
//            (8-byte header, per TSID: 16-byte UUID, [varlen tag],
//             8-byte interval count, half-open [start, end) 8-byte pairs)
//
// It is reused by every property test that needs GTID sets (gtid_set,
// binlog file metadata, storage, search_by_gtid_set, ...).

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <ostream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <hegel/hegel.h>

namespace pbt::gtid_model {

namespace gs = hegel::generators;

inline constexpr std::uint64_t min_gno{1ULL};
inline constexpr std::uint64_t max_gno{9223372036854775807ULL}; // 2^63 - 1
inline constexpr std::size_t tag_max_length{32U};

using raw_uuid = std::array<std::uint8_t, 16U>;

// ---------------------------------------------------------------------------
// interval set of closed intervals [lower, upper], always kept merged
// ---------------------------------------------------------------------------
class interval_set {
public:
  void add(std::uint64_t lower, std::uint64_t upper) {
    // merge with every interval that overlaps or touches [lower, upper]
    auto it{intervals_.upper_bound(lower)};
    if (it != std::begin(intervals_)) {
      auto prev{std::prev(it)};
      if (prev->second >= lower || prev->second + 1ULL == lower) {
        it = prev;
      }
    }
    while (it != std::end(intervals_) &&
           (it->first <= upper || it->first == upper + 1ULL)) {
      lower = std::min(lower, it->first);
      upper = std::max(upper, it->second);
      it = intervals_.erase(it);
    }
    intervals_.emplace(lower, upper);
  }

  void subtract(std::uint64_t lower, std::uint64_t upper) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> to_add;
    auto it{intervals_.upper_bound(lower)};
    if (it != std::begin(intervals_)) {
      it = std::prev(it);
    }
    while (it != std::end(intervals_) && it->first <= upper) {
      const auto [cur_lower, cur_upper]{*it};
      if (cur_upper < lower) {
        ++it;
        continue;
      }
      it = intervals_.erase(it);
      if (cur_lower < lower) {
        to_add.emplace_back(cur_lower, lower - 1ULL);
      }
      if (cur_upper > upper) {
        to_add.emplace_back(upper + 1ULL, cur_upper);
      }
    }
    for (const auto &[lo, hi] : to_add) {
      intervals_.emplace(lo, hi);
    }
  }

  [[nodiscard]] bool contains(std::uint64_t value) const {
    auto it{intervals_.upper_bound(value)};
    if (it == std::begin(intervals_)) {
      return false;
    }
    return std::prev(it)->second >= value;
  }

  [[nodiscard]] bool empty() const noexcept { return intervals_.empty(); }
  [[nodiscard]] const std::map<std::uint64_t, std::uint64_t> &
  intervals() const noexcept {
    return intervals_;
  }

  friend bool operator==(const interval_set &, const interval_set &) = default;

private:
  std::map<std::uint64_t, std::uint64_t> intervals_;
};

struct tsid {
  raw_uuid uuid{};
  std::string tag{};
  friend auto operator<=>(const tsid &, const tsid &) = default;
};

inline std::string uuid_to_string(const raw_uuid &uuid, bool upper = false) {
  static constexpr std::string_view lower_digits{"0123456789abcdef"};
  static constexpr std::string_view upper_digits{"0123456789ABCDEF"};
  const auto digits{upper ? upper_digits : lower_digits};
  std::string result;
  for (std::size_t i{0U}; i < uuid.size(); ++i) {
    if (i == 4U || i == 6U || i == 8U || i == 10U) {
      result += '-';
    }
    result += digits[uuid[i] >> 4U];
    result += digits[uuid[i] & 0x0FU];
  }
  return result;
}

// ---------------------------------------------------------------------------
// GTID set model
// ---------------------------------------------------------------------------
class gtid_set {
public:
  void add(const tsid &key, std::uint64_t lower, std::uint64_t upper) {
    data_[key].add(lower, upper);
  }
  void subtract(const tsid &key, std::uint64_t lower, std::uint64_t upper) {
    auto it{data_.find(key)};
    if (it == std::end(data_)) {
      return;
    }
    it->second.subtract(lower, upper);
    if (it->second.empty()) {
      data_.erase(it);
    }
  }
  void add(const gtid_set &other) {
    for (const auto &[key, intervals] : other.data_) {
      for (const auto &[lo, hi] : intervals.intervals()) {
        add(key, lo, hi);
      }
    }
  }
  void subtract(const gtid_set &other) {
    for (const auto &[key, intervals] : other.data_) {
      for (const auto &[lo, hi] : intervals.intervals()) {
        subtract(key, lo, hi);
      }
    }
  }
  [[nodiscard]] bool contains(const tsid &key, std::uint64_t gno) const {
    const auto it{data_.find(key)};
    return it != std::end(data_) && it->second.contains(gno);
  }
  [[nodiscard]] bool is_empty() const noexcept { return data_.empty(); }
  [[nodiscard]] bool contains_tags() const {
    for (const auto &[key, intervals] : data_) {
      if (!key.tag.empty()) {
        return true;
      }
    }
    return false;
  }
  [[nodiscard]] std::size_t number_of_tsids() const noexcept {
    return data_.size();
  }
  [[nodiscard]] const std::map<tsid, interval_set> &data() const noexcept {
    return data_;
  }

  // canonical MySQL text form: "uuid[:tag]:a-b:c, uuid2:..."
  [[nodiscard]] std::string str() const {
    std::string result;
    const raw_uuid *previous_uuid{nullptr};
    for (const auto &[key, intervals] : data_) {
      if (previous_uuid == nullptr || *previous_uuid != key.uuid) {
        if (previous_uuid != nullptr) {
          result += ", ";
        }
        result += uuid_to_string(key.uuid);
        previous_uuid = &key.uuid;
      }
      if (!key.tag.empty()) {
        result += ':';
        result += key.tag;
      }
      for (const auto &[lo, hi] : intervals.intervals()) {
        result += ':';
        result += std::to_string(lo);
        if (hi != lo) {
          result += '-';
          result += std::to_string(hi);
        }
      }
    }
    return result;
  }

  // MySQL 8.4 Gtid_set::encode() layout
  [[nodiscard]] std::vector<std::uint8_t> encode() const {
    std::vector<std::uint8_t> result;
    const auto put_u64{[&result](std::uint64_t value) {
      for (std::size_t i{0U}; i < 8U; ++i) {
        result.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
      }
    }};
    const bool tagged{contains_tags()};
    const std::uint64_t n{data_.size()};
    if (tagged) {
      // byte 0: format (1), bytes 1..6: number of TSIDs, byte 7: format (1)
      put_u64((1ULL << 56U) | (n << 8U) | 1ULL);
    } else {
      put_u64(n);
    }
    for (const auto &[key, intervals] : data_) {
      result.insert(std::end(result), std::begin(key.uuid), std::end(key.uuid));
      if (tagged) {
        // varlen length (1 byte for lengths <= 127: value << 1) + data
        result.push_back(static_cast<std::uint8_t>(key.tag.size() << 1U));
        result.insert(std::end(result), std::begin(key.tag), std::end(key.tag));
      }
      put_u64(std::uint64_t{intervals.intervals().size()});
      for (const auto &[lo, hi] : intervals.intervals()) {
        put_u64(lo);
        put_u64(hi + 1ULL); // half-open end
      }
    }
    return result;
  }

  friend bool operator==(const gtid_set &, const gtid_set &) = default;

private:
  std::map<tsid, interval_set> data_;
};

// ---------------------------------------------------------------------------
// generators
// ---------------------------------------------------------------------------

// A small pool makes different TSIDs / intervals collide frequently; the
// arbitrary branch keeps the full (non-nil) UUID domain reachable.
inline gs::Generator<raw_uuid> uuids() {
  static const std::vector<raw_uuid> pool{
      raw_uuid{0x11, 0x11, 0x11, 0x11, 0xaa, 0xaa, 0x11, 0x11, 0xaa, 0xaa, 0x11,
               0x11, 0x11, 0x11, 0x11, 0x11},
      raw_uuid{0x22, 0x22, 0x22, 0x22, 0xbb, 0xbb, 0x22, 0x22, 0xbb, 0xbb, 0x22,
               0x22, 0x22, 0x22, 0x22, 0x22},
      raw_uuid{0x3e, 0x11, 0xfa, 0x47, 0x71, 0xca, 0x11, 0xe1, 0x9e, 0x33, 0xc8,
               0x0a, 0xa9, 0x42, 0x95, 0x62},
      raw_uuid{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x01},
      raw_uuid{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
               0xff, 0xff, 0xff, 0xff, 0xff}};
  auto arbitrary{
      gs::arrays<std::uint8_t, 16U>(gs::integers<std::uint8_t>())
          .filter([](const raw_uuid &uuid) { return uuid != raw_uuid{}; })};
  return gs::one_of<raw_uuid>({gs::sampled_from(pool), arbitrary});
}

// Valid, already-canonical (lower-case) tag names, including "" (untagged).
inline gs::Generator<std::string> tags() {
  static const std::vector<std::string> pool{
      "", "", "a", "_", "tag_1", "zz", "x0123456789012345678901234567890"};
  return gs::one_of<std::string>(
      {gs::sampled_from(pool),
       gs::from_regex("[a-z_][a-z0-9_]{0,31}", /*fullmatch=*/true)});
}

inline gs::Generator<tsid> tsids() { return gs::builds<tsid>(uuids(), tags()); }

// Any valid gno, biased towards small values (frequent overlaps) and values
// next to the upper bound (2^63 - 1).
inline gs::Generator<std::uint64_t> gnos() {
  return gs::one_of<std::uint64_t>(
      {gs::integers<std::uint64_t>({.min_value = min_gno, .max_value = 40ULL}),
       gs::integers<std::uint64_t>(
           {.min_value = max_gno - 40ULL, .max_value = max_gno}),
       gs::integers<std::uint64_t>(
           {.min_value = min_gno, .max_value = max_gno})});
}

struct closed_interval {
  std::uint64_t lower{};
  std::uint64_t upper{};
};

inline gs::Generator<closed_interval> intervals() {
  return gs::tuples(gnos(), gnos()).map([](auto pair) {
    auto [first, second]{pair};
    if (first > second) {
      std::swap(first, second);
    }
    // keep most intervals short so that sets have several of them
    if (second - first > 50ULL && (first % 2ULL) == 0ULL) {
      second = first + (second % 50ULL);
    }
    return closed_interval{first, second};
  });
}

// One (tsid, interval) element of a set specification.
struct element {
  tsid key{};
  closed_interval range{};
};

inline gs::Generator<element> elements() {
  return gs::builds<element>(tsids(), intervals());
}

inline gs::Generator<std::vector<element>>
element_lists(std::size_t max_size = 12U) {
  return gs::vectors(elements(), {.max_size = max_size});
}

// stream operators, used by Hegel to print drawn values in failure reports
inline std::ostream &operator<<(std::ostream &output, const tsid &obj) {
  return output << uuid_to_string(obj.uuid)
                << (obj.tag.empty() ? "" : ":" + obj.tag);
}
inline std::ostream &operator<<(std::ostream &output,
                                const closed_interval &obj) {
  return output << obj.lower << "-" << obj.upper;
}
inline std::ostream &operator<<(std::ostream &output, const element &obj) {
  return output << "{" << obj.key << ":" << obj.range << "}";
}

inline gtid_set to_model(const std::vector<element> &spec) {
  gtid_set result;
  for (const auto &item : spec) {
    result.add(item.key, item.range.lower, item.range.upper);
  }
  return result;
}

} // namespace pbt::gtid_model

#endif // TESTS_PROPERTY_COMMON_GTID_MODEL_HPP
