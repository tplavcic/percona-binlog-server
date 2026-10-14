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

// Property-based tests for opensslpp::cipher_context, the wrapper used for
// binlog encryption at rest (src/binsrv/storage_core.cpp):
//  - the file key is encrypted with the KEK (ECB / CBC / CTR / GCM),
//  - binlog data is encrypted with a CTR cipher, and both appends
//    (put_event_data) and random-access reads (fetch_event_block) create
//    the cipher context with cipher_context::create_with_offset(), which
//    must be equivalent to processing the whole file from offset 0.
//
// Oracle: OpenSSL's EVP API called directly (one-shot, padding disabled,
// the same as the wrapper promises), plus the definition of CTR mode
// (keystream block i = E(K, IV + i), the IV being one 128-bit big-endian
// counter, as in OpenSSL's CRYPTO_ctr128_encrypt()).

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#define BOOST_TEST_MODULE CipherContextPropertyTests
// this include is needed as it provides the 'main()' function
// NOLINTNEXTLINE(misc-include-cleaner)
#include <boost/test/unit_test.hpp>

#include <openssl/evp.h>

#include "opensslpp/cipher_context.hpp"
#include "opensslpp/cipher_context_fwd.hpp"
#include "opensslpp/cipher_mode_type.hpp"
#include "opensslpp/core_error.hpp"

#include "util/byte_span_fwd.hpp"

#include "common/pbt_helpers.hpp"

namespace {

namespace gs = hegel::generators;
using opensslpp::cipher_context;
using opensslpp::cipher_context_operation_type;
using opensslpp::cipher_mode_type;

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

util::byte_span span(bytes &data) {
  return {// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
          reinterpret_cast<std::byte *>(std::data(data)), std::size(data)};
}

// ciphers of the modes the wrapper supports, filtered by what the linked
// OpenSSL provides
std::vector<std::string> available(std::vector<std::string> names) {
  std::erase_if(names, [](const std::string &name) {
    return !cipher_context::is_cipher_name_known(name);
  });
  return names;
}

const std::vector<std::string> &ctr_ciphers() {
  static const auto names{
      available({"AES-128-CTR", "AES-192-CTR", "AES-256-CTR", "ARIA-128-CTR",
                 "ARIA-192-CTR", "ARIA-256-CTR", "CAMELLIA-128-CTR",
                 "CAMELLIA-192-CTR", "CAMELLIA-256-CTR", "SM4-CTR"})};
  return names;
}

const std::vector<std::string> &all_supported_ciphers() {
  static const auto names{
      available({"AES-128-ECB", "AES-192-ECB", "AES-256-ECB", "AES-128-CBC",
                 "AES-192-CBC", "AES-256-CBC", "AES-128-CTR", "AES-192-CTR",
                 "AES-256-CTR", "AES-128-GCM", "AES-192-GCM", "AES-256-GCM",
                 "ARIA-128-CTR", "CAMELLIA-256-CTR", "SM4-CTR", "ARIA-256-CBC",
                 "CAMELLIA-128-ECB"})};
  return names;
}

// block size used for alignment requirements of update(): 16 for ECB / CBC,
// 1 for the stream-like CTR and GCM (OpenSSL reports 1 for them)
std::size_t update_granularity(const std::string &name) {
  return cipher_context::get_block_size_in_bytes(name);
}

struct material {
  std::string cipher;
  bytes key;
  bytes iv;
};

std::ostream &operator<<(std::ostream &output, const material &obj) {
  return output << "{cipher=" << obj.cipher << ", key=" << hex(obj.key)
                << ", iv=" << hex(obj.iv) << "}";
}

gs::Generator<material> materials(const std::vector<std::string> &names) {
  return gs::sampled_from(names).flat_map([](const std::string &name) {
    const auto key_size{cipher_context::get_key_size_in_bytes(name)};
    const auto iv_size{cipher_context::get_iv_size_in_bytes(name)};
    return gs::tuples(gs::binary({.min_size = key_size, .max_size = key_size}),
                      gs::binary({.min_size = iv_size, .max_size = iv_size}))
        .map([name](auto pair) {
          return material{name, std::get<0>(pair), std::get<1>(pair)};
        });
  });
}

// plaintext whose length is a multiple of the cipher's update granularity
gs::Generator<bytes> plaintexts(const std::string &cipher,
                                std::size_t max_blocks = 20U) {
  const auto granularity{update_granularity(cipher)};
  const auto max_size{granularity == 1U ? max_blocks * 16U + 15U
                                        : max_blocks * granularity};
  return gs::binary({.max_size = max_size}).map([granularity](bytes data) {
    data.resize(data.size() - data.size() % granularity);
    return data;
  });
}

struct oracle_result {
  bytes output;
  bytes tag;
};

// one-shot EVP encryption / decryption with padding disabled
oracle_result openssl_oracle(bool encrypt, const material &mat,
                             const bytes &input, const bytes &tag = {}) {
  const EVP_CIPHER *cipher{EVP_get_cipherbyname(mat.cipher.c_str())};
  if (cipher == nullptr) {
    throw std::runtime_error("oracle: unknown cipher");
  }
  EVP_CIPHER_CTX *ctx{EVP_CIPHER_CTX_new()};
  oracle_result result;
  result.output.resize(input.size() + 32U);
  int len{0};
  int total{0};
  bool ok{EVP_CipherInit_ex(ctx, cipher, nullptr, mat.key.data(),
                            mat.iv.empty() ? nullptr : mat.iv.data(),
                            encrypt ? 1 : 0) == 1 &&
          EVP_CIPHER_CTX_set_padding(ctx, 0) == 1};
  const bool gcm{EVP_CIPHER_get_mode(cipher) == EVP_CIPH_GCM_MODE};
  if (ok && gcm && !encrypt) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                             static_cast<int>(tag.size()),
                             const_cast<std::uint8_t *>(tag.data())) == 1;
  }
  if (ok && !input.empty()) {
    ok = EVP_CipherUpdate(ctx, result.output.data(), &len, input.data(),
                          static_cast<int>(input.size())) == 1;
    total = len;
  }
  if (ok) {
    ok = EVP_CipherFinal_ex(ctx, std::next(result.output.data(), total),
                            &len) == 1;
    total += len;
  }
  if (ok && gcm && encrypt) {
    result.tag.resize(16U);
    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16,
                             result.tag.data()) == 1;
  }
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) {
    throw std::runtime_error("oracle: OpenSSL operation failed");
  }
  result.output.resize(static_cast<std::size_t>(total));
  return result;
}

// Runs the wrapper over 'input', split into the given chunk sizes (rounded
// to the update granularity, remainder in the last chunk).
oracle_result run_wrapper(cipher_context &ctx, const bytes &input,
                          const std::vector<std::size_t> &chunks = {}) {
  oracle_result result;
  result.output.resize(input.size());
  const auto granularity{ctx.get_block_size_in_bytes()};
  std::size_t position{0U};
  auto chunk_it{std::begin(chunks)};
  while (position < input.size()) {
    std::size_t size{input.size() - position};
    if (chunk_it != std::end(chunks)) {
      size = std::min(size, *chunk_it - *chunk_it % granularity);
      ++chunk_it;
    }
    if (size == 0U) {
      continue;
    }
    const auto in{cspan(input).subspan(position, size)};
    const auto out{span(result.output).subspan(position, size)};
    ctx.update(in, out);
    position += size;
  }
  if (ctx.get_tag_size_in_bytes() != 0U &&
      ctx.get_operation() == cipher_context_operation_type::encryption) {
    result.tag.resize(ctx.get_tag_size_in_bytes());
    ctx.finalize(span(result.tag));
  } else {
    ctx.finalize();
  }
  return result;
}

} // namespace

// ===========================================================================
// ACCEPT direction
// ===========================================================================

// Encryption through the wrapper, with any split of the input into update()
// calls, is byte-for-byte OpenSSL's one-shot encryption (and the same GCM
// tag).
BOOST_AUTO_TEST_CASE(EncryptMatchesOpenSSL) {
  pbt::check([](hegel::TestCase &tc) {
    const auto mat{tc.draw("material", materials(all_supported_ciphers()))};
    const auto plaintext{tc.draw("plaintext", plaintexts(mat.cipher))};
    const auto chunks{
        tc.draw("chunks", gs::vectors(gs::integers<std::size_t>(
                                          {.min_value = 1U, .max_value = 64U}),
                                      {.max_size = 8U}))};
    cipher_context ctx{cipher_context_operation_type::encryption, mat.cipher,
                       cspan(mat.key), cspan(mat.iv)};
    const auto actual{run_wrapper(ctx, plaintext, chunks)};
    const auto expected{openssl_oracle(true, mat, plaintext)};
    pbt::require_equal(hex(actual.output), hex(expected.output),
                       "ciphertext differs from OpenSSL one-shot encryption");
    pbt::require_equal(hex(actual.tag), hex(expected.tag),
                       "GCM tag differs from OpenSSL");
  });
}

// decrypt(encrypt(p)) == p for every supported cipher (GCM with its tag).
BOOST_AUTO_TEST_CASE(DecryptEncryptRoundTrip) {
  pbt::check([](hegel::TestCase &tc) {
    const auto mat{tc.draw("material", materials(all_supported_ciphers()))};
    const auto plaintext{tc.draw("plaintext", plaintexts(mat.cipher))};
    cipher_context enc{cipher_context_operation_type::encryption, mat.cipher,
                       cspan(mat.key), cspan(mat.iv)};
    const auto encrypted{run_wrapper(enc, plaintext)};
    cipher_context dec{cipher_context_operation_type::decryption, mat.cipher,
                       cspan(mat.key), cspan(mat.iv), cspan(encrypted.tag)};
    const auto decrypted{run_wrapper(dec, encrypted.output)};
    pbt::require_equal(hex(decrypted.output), hex(plaintext),
                       "decrypt(encrypt(p)) != p");
  });
}

namespace {

// CTR random access as used by storage_core: a context created with
// create_with_offset(offset) and fed bytes [offset, offset + length) of
// the stream produces exactly bytes [offset, offset + length) of the
// full-stream result (encryption and decryption alike).
// With 'near_wrap' the low 64 bits of the IV are set so that the counter
// wraps around (2^64 - 1 -> 0) at or before the end of the window.
void check_ctr_offset(hegel::TestCase &tc, material mat, bool near_wrap) {
  const auto stream{tc.draw("stream", gs::binary({.max_size = 300U}))};
  const auto offset{tc.draw(
      "offset", gs::integers<std::size_t>({.max_value = stream.size()}))};
  const auto length{tc.draw(
      "length",
      gs::integers<std::size_t>({.max_value = stream.size() - offset}))};
  const auto encrypt{tc.draw("encrypt", gs::booleans())};
  if (near_wrap) {
    const auto window_end_block{(offset + length) / 16U};
    const auto distance{
        tc.draw("blocks_before_wrap",
                gs::integers<std::uint64_t>(
                    {.min_value = 0U, .max_value = window_end_block}))};
    // low 64 bits (big-endian) = 2^64 - 1 - distance
    const std::uint64_t low{~distance};
    for (std::size_t i{0U}; i < 8U; ++i) {
      mat.iv[mat.iv.size() - 1U - i] =
          static_cast<std::uint8_t>(low >> (8U * i));
    }
  }
  const auto operation{encrypt ? cipher_context_operation_type::encryption
                               : cipher_context_operation_type::decryption};

  const auto full{openssl_oracle(encrypt, mat, stream).output};
  const bytes window(
      std::next(stream.begin(), static_cast<long>(offset)),
      std::next(stream.begin(), static_cast<long>(offset + length)));
  bytes actual;
  try {
    auto ctx{cipher_context::create_with_offset(offset, operation, mat.cipher,
                                                cspan(mat.key), cspan(mat.iv))};
    actual = run_wrapper(ctx, window).output;
  } catch (const std::exception &e) {
    pbt::fail(std::string{"create_with_offset() rejected a valid CTR "
                          "cipher/offset: "} +
              e.what());
  }
  const bytes expected(
      std::next(full.begin(), static_cast<long>(offset)),
      std::next(full.begin(), static_cast<long>(offset + length)));
  pbt::require_equal(hex(actual), hex(expected),
                     "offset context output differs from the full stream");
}

} // namespace

// IVs whose low 64-bit counter cannot overflow within the stream (random
// IVs: overflow is astronomically unlikely in practice).
BOOST_AUTO_TEST_CASE(CtrOffsetMatchesFullStream) {
  pbt::check(
      [](hegel::TestCase &tc) {
        auto mat{tc.draw("material", materials(ctr_ciphers()))};
        // keep the top bit of the low 64-bit half clear -> no carry
        mat.iv[mat.iv.size() - 8U] &= 0x7FU;
        check_ctr_offset(tc, mat, false);
      },
      {.test_cases = 5000U});
}

// The CTR counter is the whole 128-bit IV (OpenSSL CRYPTO_ctr128_encrypt
// carries into the upper 64 bits), so the offset context must produce the
// same bytes when the low 64 bits of the IV are about to wrap around.
BOOST_AUTO_TEST_CASE(CtrOffsetHandlesCounterCarry) {
  pbt::check(
      [](hegel::TestCase &tc) {
        const auto mat{tc.draw("material", materials(ctr_ciphers()))};
        check_ctr_offset(tc, mat, true);
      },
      {.test_cases = 2000U});
}

// create_with_offset() must accept every spelling of a CTR cipher name the
// plain constructor (and the storage configuration validation, which uses
// the same OpenSSL lookup) accepts: OpenSSL cipher names are
// case-insensitive.
BOOST_AUTO_TEST_CASE(CtrOffsetAcceptsSameCipherNamesAsConstructor) {
  pbt::check([](hegel::TestCase &tc) {
    const auto base{tc.draw("cipher", gs::sampled_from(ctr_ciphers()))};
    const auto case_mask{tc.draw("case_mask", gs::integers<std::uint32_t>())};
    std::string name{base};
    for (std::size_t i{0U}; i < name.size(); ++i) {
      if (((case_mask >> (i % 32U)) & 1U) != 0U) {
        name[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(name[i])));
      }
    }
    tc.note("cipher name spelling = '" + name + "'");
    tc.assume(cipher_context::is_cipher_name_supported(name));
    const auto offset{tc.draw(
        "offset",
        gs::integers<std::uint64_t>({.min_value = 1U, .max_value = 1000000U}))};
    const bytes key(cipher_context::get_key_size_in_bytes(name), 0x11U);
    const bytes iv(cipher_context::get_iv_size_in_bytes(name), 0x22U);
    try {
      [[maybe_unused]] auto ctx{cipher_context::create_with_offset(
          offset, cipher_context_operation_type::decryption, name, cspan(key),
          cspan(iv))};
    } catch (const std::exception &e) {
      pbt::fail(std::string{"create_with_offset() rejects a cipher name the "
                            "constructor accepts: "} +
                e.what());
    }
  });
}

// create_with_offset(0, ...) behaves exactly like the plain constructor
// for every supported cipher.
BOOST_AUTO_TEST_CASE(OffsetZeroEqualsPlainContext) {
  pbt::check([](hegel::TestCase &tc) {
    const auto mat{tc.draw("material", materials(all_supported_ciphers()))};
    const auto plaintext{tc.draw("plaintext", plaintexts(mat.cipher, 8U))};
    auto with_offset{cipher_context::create_with_offset(
        0U, cipher_context_operation_type::encryption, mat.cipher,
        cspan(mat.key), cspan(mat.iv))};
    cipher_context plain{cipher_context_operation_type::encryption, mat.cipher,
                         cspan(mat.key), cspan(mat.iv)};
    pbt::require_equal(hex(run_wrapper(with_offset, plaintext).output),
                       hex(run_wrapper(plain, plaintext).output),
                       "offset 0 context differs from the plain context");
  });
}

// Instance getters agree with the static getters for the same cipher name.
BOOST_AUTO_TEST_CASE(InstanceAndStaticGettersAgree) {
  pbt::check([](hegel::TestCase &tc) {
    const auto mat{tc.draw("material", materials(all_supported_ciphers()))};
    const cipher_context ctx{cipher_context_operation_type::encryption,
                             mat.cipher, cspan(mat.key), cspan(mat.iv)};
    pbt::require_equal(ctx.get_key_size_in_bytes(),
                       cipher_context::get_key_size_in_bytes(mat.cipher),
                       "key size");
    pbt::require_equal(ctx.get_iv_size_in_bytes(),
                       cipher_context::get_iv_size_in_bytes(mat.cipher),
                       "iv size");
    pbt::require_equal(ctx.get_block_size_in_bytes(),
                       cipher_context::get_block_size_in_bytes(mat.cipher),
                       "block size");
    pbt::require(ctx.get_mode() == cipher_context::get_mode(mat.cipher),
                 "mode");
  });
}

// update() with the output aliasing the input (storage_core decrypts
// fetched blocks in place) gives the same result as separate buffers.
BOOST_AUTO_TEST_CASE(InPlaceUpdateMatchesSeparateBuffers) {
  pbt::check([](hegel::TestCase &tc) {
    const auto mat{tc.draw("material", materials(all_supported_ciphers()))};
    const auto input{tc.draw("input", plaintexts(mat.cipher))};
    const auto encrypt{tc.draw("encrypt", gs::booleans())};
    tc.assume(encrypt ||
              cipher_context::get_mode(mat.cipher) != cipher_mode_type::gcm);
    const auto operation{encrypt ? cipher_context_operation_type::encryption
                                 : cipher_context_operation_type::decryption};
    cipher_context separate{operation, mat.cipher, cspan(mat.key),
                            cspan(mat.iv)};
    bytes expected(input.size());
    separate.update(cspan(input), span(expected));
    cipher_context in_place{operation, mat.cipher, cspan(mat.key),
                            cspan(mat.iv)};
    bytes buffer{input};
    in_place.update(cspan(buffer), span(buffer));
    pbt::require_equal(hex(buffer), hex(expected),
                       "in-place update differs from out-of-place update");
  });
}

// An update() rejected by validation (wrong output size) does not consume
// keystream: continuing with a valid update gives the uninterrupted result.
BOOST_AUTO_TEST_CASE(RejectedUpdateDoesNotAdvanceState) {
  pbt::check([](hegel::TestCase &tc) {
    const auto mat{tc.draw("material", materials(ctr_ciphers()))};
    const auto input{
        tc.draw("input", gs::binary({.min_size = 1U, .max_size = 100U}))};
    cipher_context ctx{cipher_context_operation_type::encryption, mat.cipher,
                       cspan(mat.key), cspan(mat.iv)};
    bytes wrong(input.size() + 1U);
    pbt::require_throws<opensslpp::core_error>(
        [&] { ctx.update(cspan(input), span(wrong)); },
        "update() with a mismatching output size must be rejected");
    const auto actual{run_wrapper(ctx, input).output};
    const auto expected{openssl_oracle(true, mat, input).output};
    pbt::require_equal(hex(actual), hex(expected),
                       "a rejected update() changed the cipher state");
  });
}

// Everything is_cipher_name_supported() accepts (the keyring validates KEK
// cipher names with it) can actually be used: a context with key / IV of
// the sizes the static getters report must be constructible.
BOOST_AUTO_TEST_CASE(SupportedCipherNamesAreUsable) {
  pbt::check([](hegel::TestCase &tc) {
    const auto name{tc.draw(
        "cipher", gs::sampled_from<std::string>({"AES-128-ECB",
                                                 "AES-256-CBC",
                                                 "AES-128-CTR",
                                                 "AES-256-GCM",
                                                 "ARIA-128-CBC",
                                                 "CAMELLIA-256-ECB",
                                                 "SM4-CBC",
                                                 "DES-EDE3-CBC",
                                                 "DES-EDE3-ECB",
                                                 "DES-CBC",
                                                 "DES-ECB",
                                                 "BF-CBC",
                                                 "BF-ECB",
                                                 "CAST5-CBC",
                                                 "CAST5-ECB",
                                                 "SEED-CBC",
                                                 "SEED-ECB",
                                                 "RC2-CBC",
                                                 "RC2-ECB",
                                                 "IDEA-CBC",
                                                 "AES-128-CBC-HMAC-SHA1",
                                                 "AES-256-CBC-HMAC-SHA256"}))};
    tc.assume(cipher_context::is_cipher_name_supported(name));
    const bytes key(cipher_context::get_key_size_in_bytes(name), 0x11U);
    const bytes iv(cipher_context::get_iv_size_in_bytes(name), 0x22U);
    try {
      [[maybe_unused]] const cipher_context ctx{
          cipher_context_operation_type::encryption, name, cspan(key),
          cspan(iv)};
    } catch (const std::exception &e) {
      pbt::fail("cipher reported as supported cannot be used: " +
                std::string{e.what()});
    }
  });
}

// ===========================================================================
// REJECT direction
// ===========================================================================

// A modified GCM ciphertext or tag must make decryption fail in finalize()
// with core_error (authentication).
BOOST_AUTO_TEST_CASE(GcmRejectsTamperedCiphertextOrTag) {
  pbt::check([](hegel::TestCase &tc) {
    const auto mat{tc.draw(
        "material",
        materials(available({"AES-128-GCM", "AES-192-GCM", "AES-256-GCM"})))};
    const auto plaintext{
        tc.draw("plaintext", gs::binary({.min_size = 1U, .max_size = 100U}))};
    cipher_context enc{cipher_context_operation_type::encryption, mat.cipher,
                       cspan(mat.key), cspan(mat.iv)};
    auto encrypted{run_wrapper(enc, plaintext)};
    const auto in_tag{tc.draw("tamper_tag", gs::booleans())};
    auto &target{in_tag ? encrypted.tag : encrypted.output};
    const auto position{
        tc.draw("position",
                gs::integers<std::size_t>({.max_value = target.size() - 1U}))};
    const auto flip{
        tc.draw("flip", gs::integers<std::uint8_t>({.min_value = 1U}))};
    target[position] ^= flip;
    pbt::require_throws<opensslpp::core_error>(
        [&] {
          cipher_context dec{cipher_context_operation_type::decryption,
                             mat.cipher, cspan(mat.key), cspan(mat.iv),
                             cspan(encrypted.tag)};
          [[maybe_unused]] const auto result{
              run_wrapper(dec, encrypted.output)};
        },
        "tampered GCM data must fail authentication");
  });
}

// Wrong key or IV sizes are rejected with core_error (never accepted, never
// a crash).
BOOST_AUTO_TEST_CASE(RejectsInvalidKeyOrIvSize) {
  pbt::check([](hegel::TestCase &tc) {
    const auto mat{tc.draw("material", materials(all_supported_ciphers()))};
    auto key{mat.key};
    auto iv{mat.iv};
    const auto break_key{tc.draw("break_key", gs::booleans())};
    const auto delta{
        tc.draw("delta", gs::integers<int>({.min_value = -8, .max_value = 8}))};
    tc.assume(delta != 0);
    auto &target{break_key ? key : iv};
    if (delta < 0) {
      tc.assume(target.size() >= static_cast<std::size_t>(-delta));
      target.resize(target.size() - static_cast<std::size_t>(-delta));
    } else {
      target.resize(target.size() + static_cast<std::size_t>(delta), 0x33U);
    }
    pbt::require_throws<opensslpp::core_error>(
        [&] {
          [[maybe_unused]] const cipher_context ctx{
              cipher_context_operation_type::encryption, mat.cipher, cspan(key),
              cspan(iv)};
        },
        "invalid key / iv size must be rejected");
  });
}

// ECB and CBC (padding disabled) reject update() input that is not a
// multiple of the block size.
BOOST_AUTO_TEST_CASE(BlockModesRejectPartialBlocks) {
  pbt::check([](hegel::TestCase &tc) {
    const auto mat{tc.draw(
        "material", materials(available({"AES-128-ECB", "AES-256-ECB",
                                         "AES-128-CBC", "AES-256-CBC"})))};
    const auto input{
        tc.draw("input", gs::binary({.min_size = 1U, .max_size = 100U}))};
    tc.assume(input.size() % 16U != 0U);
    cipher_context ctx{cipher_context_operation_type::encryption, mat.cipher,
                       cspan(mat.key), cspan(mat.iv)};
    bytes output(input.size());
    pbt::require_throws<opensslpp::core_error>(
        [&] { ctx.update(cspan(input), span(output)); },
        "partial block must be rejected in ECB/CBC");
  });
}

// create_with_offset() only supports CTR ciphers, whatever the offset
// (storage_core uses it for reads at any offset, including 0, so accepting
// a cipher only at offset 0 would make a file readable only from its
// start).
BOOST_AUTO_TEST_CASE(OffsetRejectsNonCtrCiphers) {
  pbt::check([](hegel::TestCase &tc) {
    const auto mat{tc.draw(
        "material",
        materials(available({"AES-128-ECB", "AES-256-CBC", "AES-128-GCM",
                             "AES-256-GCM", "CAMELLIA-128-ECB"})))};
    const auto offset{tc.draw(
        "offset", gs::one_of<std::uint64_t>(
                      {gs::just(std::uint64_t{0U}),
                       gs::integers<std::uint64_t>({.min_value = 1U})}))};
    pbt::require_throws<opensslpp::core_error>(
        [&] {
          [[maybe_unused]] auto ctx{cipher_context::create_with_offset(
              offset, cipher_context_operation_type::encryption, mat.cipher,
              cspan(mat.key), cspan(mat.iv))};
        },
        "create_with_offset() with a non-CTR cipher must be rejected");
  });
}
