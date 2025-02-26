/**
 *  @brief  Hardware-accelerated non-cryptographic string hashing and checksums.
 *  @file   hash.h
 *  @author Ash Vardanian
 *
 *  Includes core APIs:
 *
 *  - `sz_bytesum` - for byte-level 64-bit unsigned byte-level checksums.
 *  - `sz_hash` - for 64-bit single-shot hashing using AES instructions.
 *  - `sz_hash_state_init`, `sz_hash_state_stream`, `sz_hash_state_fold` - for incremental hashing.
 *  - `sz_generate` - for populating buffers with pseudo-random noise using AES instructions.
 *
 *  Why the hell do we need a yet another hashing library?!
 *  Turns out, most existing libraries have noticeable constraints. Try finding a library that:
 *
 *  - Outputs 64-bit or 128-bit hashes and passes the SMHasher test suite.
 *  - Is fast for both short and long strings.
 *  - Supports incremental @b (streaming) hashing, when the data arrives in chunks.
 *  - Supports custom seeds hashes and secret strings for security.
 *  - Provides dynamic dispatch for different architectures to simplify deployment.
 *  - Uses modern SIMD, including not just AVX2 and NEON, but also AVX-512 and SVE2.
 *  - Documents its logic and guarantees the same output across different platforms.
 *
 *  This includes projects like "MurmurHash", "CityHash", "SpookyHash", "FarmHash", "MetroHash", "HighwayHash", etc.
 *  There are 2 libraries that are close to meeting these requirements: "xxHash" in C++ and "aHash" in Rust:
 *
 *  - "aHash" is fast, but written in Rust, has no dynamic dispatch, and lacks AVX-512 and SVE2 support.
 *    It also does not adhere to a fixed output, and can't be used in applications like computing packet checksums
 *    in network traffic or implementing persistent data structures.
 *
 *  - "xxHash" is implemented in C, has an extremely wide set of third-party language bindings, and provides both
 *    32-, 64-, and 128-bit hashes. It is fast, but its dynamic dispatch is limited to x86 with `xxh_x86dispatch.c`.
 *
 *  StringZilla uses a scheme more similar to the "aHash" library, utilizing the AES extensions, that provide
 *  a remarkable level of "mixing per cycle" and are broadly available on modern CPUs. Similar to "aHash", they
 *  are combined with "shuffle & add" instructions to provide a high level of entropy in the output. That operation
 *  is practically free, as many modern CPUs will dispatch them on different ports. On x86, for example:
 *
 *  - `VAESENC` (ZMM, ZMM, ZMM)`:
 *    - on Intel Ice Lake: 5 cycles on port 0.
 *    - On AMD Zen4: 4 cycles on ports 0 or 1.
 *  - `VPSHUFB_Z (ZMM, K, ZMM, ZMM)`
 *    - on Intel Ice Lake: 3 cycles on port 5.
 *    - On AMD Zen4: 2 cycles on ports 1 or 2.
 *  - `VPADDQ (ZMM, ZMM, ZMM)`:
 *    - on Intel Ice Lake: 1 cycle on ports 0 or 5.
 *    - On AMD Zen4: 1 cycle on ports 0, 1, 2, 3.
 *
 *  Unlike "aHash", the length is not mixed into "AES" block at start to allow incremental construction.
 *  Unlike "aHash", on long inputs, we use a heavier procedure that is more vector-friendly on modern servers.
 *  Unlike "aHash", we don't load interleaved memory regions, making vectorized variant more similar to sequential.
 *  Unlike "aHash", on platforms like Intel Skylake-X or AWS Graviton 3, we use masked loads.
 *  Unlike "aHash", in final folding procedure, we use the same `VAESENC` instead of `VAESDEC`, which
 *  still provides the same level of mixing, but allows us to have a lighter serial fallback implementation.
 *
 *  @see Reini Urban's more active fork of SMHasher by Austin Appleby: https://github.com/rurban/smhasher
 *  @see The serial AES routines are based on Morten Jensen's "tiny-AES-c": https://github.com/kokke/tiny-AES-c
 *  @see The "xxHash" C implementation by Yann Collet: https://github.com/Cyan4973/xxHash
 *  @see The "aHash" Rust implementation by Tom Kaitchuck: https://github.com/tkaitchuck/aHash
 *  @see "Emulating x86 AES Intrinsics on ARMv8-A" by Michael Brase:
 *       https://blog.michaelbrase.com/2018/05/08/emulating-x86-aes-intrinsics-on-armv8-a/
 */
#ifndef STRINGZILLA_HASH_H_
#define STRINGZILLA_HASH_H_

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma region Core API

/**
 *  @brief  Computes the 64-bit check-sum of bytes in a string.
 *          Similar to `std::ranges::accumulate`.
 *
 *  @param[in] text String to aggregate.
 *  @param[in] length Number of bytes in the text.
 *  @return 64-bit unsigned value.
 *
 *  Example usage:
 *
 *  @code{.c}
 *      #include <stringzilla/hash.h>
 *      int main() {
 *          return sz_bytesum("hi", 2) == 209 ? 0 : 1;
 *      }
 *  @endcode
 *
 *  @note   Selects the fastest implementation at compile- or run-time based on `SZ_DYNAMIC_DISPATCH`.
 *  @sa     sz_bytesum_serial, sz_bytesum_haswell, sz_bytesum_skylake, sz_bytesum_ice, sz_bytesum_neon
 */
SZ_DYNAMIC sz_u64_t sz_bytesum(sz_cptr_t text, sz_size_t length);

/**
 *  @brief  Computes the 64-bit unsigned hash of a string similar to @b `std::hash` in C++.
 *          It's not cryptographically secure, but it's fast and provides a good distribution.
 *          It passes the SMHasher suite by Austin Appleby with no collisions, even with `--extra` flag.
 *  @see    HASH.md for a detailed explanation of the algorithm.
 *
 *  @param[in] text String to hash.
 *  @param[in] length Number of bytes in the text.
 *  @param[in] seed 64-bit unsigned seed for the hash.
 *  @return 64-bit hash value.
 *
 *  Example usage:
 *
 *  @code{.c}
 *      #include <stringzilla/hash.h>
 *      int main() {
 *          return sz_hash("hello", 5, 0) != sz_hash("world", 5, 0) ? 0 : 1;
 *      }
 *  @endcode
 *
 *  @note   Selects the fastest implementation at compile- or run-time based on `SZ_DYNAMIC_DISPATCH`.
 *  @sa     sz_hash_serial, sz_hash_haswell, sz_hash_skylake, sz_hash_ice, sz_hash_neon
 *
 *  @note   The algorithm must provide the same output on all platforms in both single-shot and incremental modes.
 *  @sa     sz_hash_state_init, sz_hash_state_stream, sz_hash_state_fold
 */
SZ_DYNAMIC sz_u64_t sz_hash(sz_cptr_t text, sz_size_t length, sz_u64_t seed);

/**
 *  @brief  A Pseudorandom Number Generator (PRNG), inspired the AES-CTR-128 algorithm,
 *          but using only one round of AES mixing as opposed to "NIST SP 800-90A".
 *
 *  CTR_DRBG (CounTeR mode Deterministic Random Bit Generator) appears secure and indistinguishable from a
 *  true random source when AES is used as the underlying block cipher and 112 bits are taken from this PRNG.
 *  When AES is used as the underlying block cipher and 128 bits are taken from each instantiation,
 *  the required security level is delivered with the caveat that a 128-bit cipher's output in
 *  counter mode can be distinguished from a true RNG.
 *
 *  In this case, it doesn't apply, as we only use one round of AES mixing. We also don't expose a separate "key",
 *  only a "nonce", to keep the API simple, but we mix it with 512 bits of Pi constants to increase randomness.
 *
 *  @param[out] text Output string buffer to be populated.
 *  @param[in] length Number of bytes in the string.
 *  @param[in] nonce "Number used ONCE" to ensure uniqueness of produced blocks.
 *
 *  Example usage:
 *
 *  @code{.c}
 *      #include <stringzilla/hash.h>
 *      int main() {
 *          char first_buffer[5], second_buffer[5];
 *          sz_generate(first_buffer, 5, 0);
 *          sz_generate(second_buffer, 5, 0); //? Same nonce must produce the same output
 *          return sz_bytesum(first_buffer, 5) == sz_bytesum(second_buffer, 5) ? 0 : 1;
 *      }
 *  @endcode
 *
 *  @note   Selects the fastest implementation at compile- or run-time based on `SZ_DYNAMIC_DISPATCH`.
 *  @sa     sz_generate_serial, sz_generate_haswell, sz_generate_skylake, sz_generate_ice, sz_generate_neon
 */
SZ_DYNAMIC void sz_generate(sz_ptr_t text, sz_size_t length, sz_u64_t nonce);

/**
 *  @brief  The state for incremental construction of a hash.
 *  @see    sz_hash_state_init, sz_hash_state_stream, sz_hash_state_fold.
 */
typedef struct sz_hash_state_t {
    sz_u512_vec_t aes;
    sz_u512_vec_t sum;
    sz_u512_vec_t ins;
    sz_u128_vec_t key;
    sz_size_t ins_length;
} sz_hash_state_t;

typedef struct _sz_hash_minimal_t {
    sz_u128_vec_t aes;
    sz_u128_vec_t sum;
    sz_u128_vec_t key;
} _sz_hash_minimal_t;

/**
 *  @brief  Initializes the state for incremental construction of a hash.
 *
 *  @param[out] state The state to initialize.
 *  @param[in] seed The 64-bit unsigned seed for the hash.
 */
SZ_DYNAMIC void sz_hash_state_init(sz_hash_state_t *state, sz_u64_t seed);

/**
 *  @brief  Updates the state with new data.
 *
 *  @param[inout] state The state to stream.
 *  @param[in] text The new data to include in the hash.
 *  @param[in] length The number of bytes in the new data.
 */
SZ_DYNAMIC void sz_hash_state_stream(sz_hash_state_t *state, sz_cptr_t text, sz_size_t length);

/**
 *  @brief  Finalizes the immutable state and returns the hash.
 *
 *  @param[in] state The state to fold.
 *  @return The 64-bit hash value.
 */
SZ_DYNAMIC sz_u64_t sz_hash_state_fold(sz_hash_state_t const *state);

/** @copydoc sz_bytesum */
SZ_PUBLIC sz_u64_t sz_bytesum_serial(sz_cptr_t text, sz_size_t length);

/** @copydoc sz_hash */
SZ_PUBLIC sz_u64_t sz_hash_serial(sz_cptr_t text, sz_size_t length, sz_u64_t seed);

/** @copydoc sz_generate */
SZ_PUBLIC void sz_generate_serial(sz_ptr_t text, sz_size_t length, sz_u64_t nonce);

/** @copydoc sz_hash_state_init */
SZ_PUBLIC void sz_hash_state_init_serial(sz_hash_state_t *state, sz_u64_t seed);

/** @copydoc sz_hash_state_stream */
SZ_PUBLIC void sz_hash_state_stream_serial(sz_hash_state_t *state, sz_cptr_t text, sz_size_t length);

/** @copydoc sz_hash_state_fold */
SZ_PUBLIC sz_u64_t sz_hash_state_fold_serial(sz_hash_state_t const *state);

/** @copydoc sz_bytesum */
SZ_PUBLIC sz_u64_t sz_bytesum_haswell(sz_cptr_t text, sz_size_t length);

/** @copydoc sz_hash */
SZ_PUBLIC sz_u64_t sz_hash_haswell(sz_cptr_t text, sz_size_t length, sz_u64_t seed);

/** @copydoc sz_generate */
SZ_PUBLIC void sz_generate_haswell(sz_ptr_t text, sz_size_t length, sz_u64_t nonce);

/** @copydoc sz_hash_state_init */
SZ_PUBLIC void sz_hash_state_init_haswell(sz_hash_state_t *state, sz_u64_t seed);

/** @copydoc sz_hash_state_stream */
SZ_PUBLIC void sz_hash_state_stream_haswell(sz_hash_state_t *state, sz_cptr_t text, sz_size_t length);

/** @copydoc sz_hash_state_fold */
SZ_PUBLIC sz_u64_t sz_hash_state_fold_haswell(sz_hash_state_t const *state);

/** @copydoc sz_bytesum */
SZ_PUBLIC sz_u64_t sz_bytesum_skylake(sz_cptr_t text, sz_size_t length);

/** @copydoc sz_hash */
SZ_PUBLIC sz_u64_t sz_hash_skylake(sz_cptr_t text, sz_size_t length, sz_u64_t seed);

/** @copydoc sz_generate */
SZ_PUBLIC void sz_generate_skylake(sz_ptr_t text, sz_size_t length, sz_u64_t nonce);

/** @copydoc sz_hash_state_init */
SZ_PUBLIC void sz_hash_state_init_skylake(sz_hash_state_t *state, sz_u64_t seed);

/** @copydoc sz_hash_state_stream */
SZ_PUBLIC void sz_hash_state_stream_skylake(sz_hash_state_t *state, sz_cptr_t text, sz_size_t length);

/** @copydoc sz_hash_state_fold */
SZ_PUBLIC sz_u64_t sz_hash_state_fold_skylake(sz_hash_state_t const *state);

/** @copydoc sz_bytesum */
SZ_PUBLIC sz_u64_t sz_bytesum_ice(sz_cptr_t text, sz_size_t length);

/** @copydoc sz_hash */
SZ_PUBLIC sz_u64_t sz_hash_ice(sz_cptr_t text, sz_size_t length, sz_u64_t seed);

/** @copydoc sz_generate */
SZ_PUBLIC void sz_generate_ice(sz_ptr_t text, sz_size_t length, sz_u64_t nonce);

/** @copydoc sz_hash_state_init */
SZ_PUBLIC void sz_hash_state_init_ice(sz_hash_state_t *state, sz_u64_t seed);

/** @copydoc sz_hash_state_stream */
SZ_PUBLIC void sz_hash_state_stream_ice(sz_hash_state_t *state, sz_cptr_t text, sz_size_t length);

/** @copydoc sz_hash_state_fold */
SZ_PUBLIC sz_u64_t sz_hash_state_fold_ice(sz_hash_state_t const *state);

/** @copydoc sz_bytesum */
SZ_PUBLIC sz_u64_t sz_bytesum_neon(sz_cptr_t text, sz_size_t length);

/** @copydoc sz_hash */
SZ_PUBLIC sz_u64_t sz_hash_neon(sz_cptr_t text, sz_size_t length, sz_u64_t seed);

/** @copydoc sz_generate */
SZ_PUBLIC void sz_generate_neon(sz_ptr_t text, sz_size_t length, sz_u64_t nonce);

/** @copydoc sz_hash_state_init */
SZ_PUBLIC void sz_hash_state_init_neon(sz_hash_state_t *state, sz_u64_t seed);

/** @copydoc sz_hash_state_stream */
SZ_PUBLIC void sz_hash_state_stream_neon(sz_hash_state_t *state, sz_cptr_t text, sz_size_t length);

/** @copydoc sz_hash_state_fold */
SZ_PUBLIC sz_u64_t sz_hash_state_fold_neon(sz_hash_state_t const *state);

#pragma endregion // Core API

#pragma region Helper Methods

/**
 *  @brief  Compares the state of two running hashes.
 *  @note   The current content of the `ins` buffer and its length is ignored.
 */
SZ_PUBLIC sz_bool_t sz_hash_state_equal(sz_hash_state_t const *lhs, sz_hash_state_t const *rhs) {
    int same_aes = //
        lhs->aes.u64s[0] == rhs->aes.u64s[0] && lhs->aes.u64s[1] == rhs->aes.u64s[1] &&
        lhs->aes.u64s[2] == rhs->aes.u64s[2] && lhs->aes.u64s[3] == rhs->aes.u64s[3];
    int same_sum = //
        lhs->sum.u64s[0] == rhs->sum.u64s[0] && lhs->sum.u64s[1] == rhs->sum.u64s[1] &&
        lhs->sum.u64s[2] == rhs->sum.u64s[2] && lhs->sum.u64s[3] == rhs->sum.u64s[3];
    int same_key = //
        lhs->key.u64s[0] == rhs->key.u64s[0] && lhs->key.u64s[1] == rhs->key.u64s[1];
    return same_aes && same_sum && same_key ? sz_true_k : sz_false_k;
}

#pragma endregion // Helper Methods

#pragma region Serial Implementation

SZ_PUBLIC sz_u64_t sz_bytesum_serial(sz_cptr_t text, sz_size_t length) {
    sz_u64_t bytesum = 0;
    sz_u8_t const *text_u8 = (sz_u8_t const *)text;
    sz_u8_t const *text_end = text_u8 + length;
    for (; text_u8 != text_end; ++text_u8) bytesum += *text_u8;
    return bytesum;
}

/**
 *  @brief  Emulates the behaviour of `_mm_aesenc_si128` for a single round.
 *          This function is used as a fallback when the hardware-accelerated version is not available.
 *  @return Result of `MixColumns(SubBytes(ShiftRows(state))) ^ round_key`.
 *  @see    Based on Jean-Philippe Aumasson's reference implementation: https://github.com/veorq/aesenc-noNI
 */
SZ_INTERNAL sz_u128_vec_t _sz_emulate_aesenc_si128_serial(sz_u128_vec_t state_vec, sz_u128_vec_t round_key_vec) {
    static sz_u8_t const sbox[256] = {
        // 0     1    2      3     4    5     6     7      8    9     A      B     C     D     E     F
        0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, //
        0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, //
        0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, //
        0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, //
        0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, //
        0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, //
        0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, //
        0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, //
        0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, //
        0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, //
        0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, //
        0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, //
        0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, //
        0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, //
        0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, //
        0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

    // Combine `ShiftRows` and `SubBytes`
    sz_u8_t state_2d[4][4];
    for (int i = 0; i < 16; ++i) state_2d[((i / 4) + 4 - (i % 4)) % 4][i % 4] = sbox[state_vec.u8s[i]];
#define _sz_gf2_double(x) (((x) << 1) ^ ((((x) >> 7) & 1) * 0x1b))
    // Perform `MixColumns` using GF2 multiplication by 2
    for (int i = 0; i < 4; ++i) {
        sz_u8_t t = state_2d[i][0];
        sz_u8_t u = state_2d[i][0] ^ state_2d[i][1] ^ state_2d[i][2] ^ state_2d[i][3];
        state_2d[i][0] ^= u ^ _sz_gf2_double(state_2d[i][0] ^ state_2d[i][1]);
        state_2d[i][1] ^= u ^ _sz_gf2_double(state_2d[i][1] ^ state_2d[i][2]);
        state_2d[i][2] ^= u ^ _sz_gf2_double(state_2d[i][2] ^ state_2d[i][3]);
        state_2d[i][3] ^= u ^ _sz_gf2_double(state_2d[i][3] ^ t);
    }
#undef _sz_gf2_double
    // Export `XOR`-ing with the round key
    sz_u128_vec_t result;
    for (int i = 0; i < 16; ++i) result.u8s[i] = state_2d[i / 4][i % 4] ^ round_key_vec.u8s[i];
    return result;
}

SZ_INTERNAL sz_u128_vec_t _sz_emulate_shuffle_epi8_serial(sz_u128_vec_t state_vec, sz_u8_t const order[16]) {
    sz_u128_vec_t result;
    for (int i = 0; i < 16; ++i) result.u8s[i] = state_vec.u8s[order[i]];
    return result;
}

/**
 *  @brief  Provides 1024 bits worth of precomputed Pi constants for the hash.
 *  @return Pointer aligned to 64 bytes on SIMD-capable platforms.
 *
 *  Bailey-Borwein-Plouffe @b (BBP) formula is used to compute the hexadecimal digits of Pi.
 *  It can be easily implemented in just 10 lines of Python and for 1024 bits requires 256 digits:
 *
 *  @code{.py}
 *      def pi(digits: int) -> str:
 *          n, d = 0, 1
 *          HEX = "0123456789ABCDEF"
 *          result = ["3."]
 *          for i in range(digits):
 *              xn = 120 * i**2 + 151 * i + 47
 *              xd = 512 * i**4 + 1024 * i**3 + 712 * i**2 + 194 * i + 15
 *              n = ((16 * n * xd) + (xn * d)) % (d * xd)
 *              d *= xd
 *              result.append(HEX[(16 * n) // d])
 *          return "".join(result)
 *  @endcode
 *
 *  For `pi(16)` the result is `3.243F6A8885A308D3` and you can find the digits after the dot in
 *  the first element of output array.
 *
 *  @see    Bailey-Borwein-Plouffe @b (BBP) formula explanation by Mosè Giordano:
 *          https://giordano.github.io/blog/2017-11-21-hexadecimal-pi/
 *
 */
SZ_INTERNAL sz_u64_t const *_sz_hash_pi_constants(void) {
    static _SZ_ALIGN64 sz_u64_t const pi[16] = {
        0x243F6A8885A308D3ull, 0x13198A2E03707344ull, 0xA4093822299F31D0ull, 0x082EFA98EC4E6C89ull,
        0x452821E638D01377ull, 0xBE5466CF34E90C6Cull, 0xC0AC29B7C97C50DDull, 0x3F84D5B5B5470917ull,
        0x9216D5D98979FB1Bull, 0xD1310BA698DFB5ACull, 0x2FFD72DBD01ADFB7ull, 0xB8E1AFED6A267E96ull,
        0xBA7C9045F12C7F99ull, 0x24A19947B3916CF7ull, 0x0801F2E2858EFC16ull, 0x636920D871574E69ull,
    };
    return &pi[0];
}

/**
 *  @brief  Provides a shuffle mask for the additive part, identical to "aHash" in a single lane.
 *  @return Pointer aligned to 64 bytes on SIMD-capable platforms.
 */
SZ_INTERNAL sz_u8_t const *_sz_hash_u8x16x4_shuffle(void) {
    static _SZ_ALIGN64 sz_u8_t const shuffle[64] = {
        0x04, 0x0b, 0x09, 0x06, 0x08, 0x0d, 0x0f, 0x05, //
        0x0e, 0x03, 0x01, 0x0c, 0x00, 0x07, 0x0a, 0x02, //
        0x04, 0x0b, 0x09, 0x06, 0x08, 0x0d, 0x0f, 0x05, //
        0x0e, 0x03, 0x01, 0x0c, 0x00, 0x07, 0x0a, 0x02, //
        0x04, 0x0b, 0x09, 0x06, 0x08, 0x0d, 0x0f, 0x05, //
        0x0e, 0x03, 0x01, 0x0c, 0x00, 0x07, 0x0a, 0x02, //
        0x04, 0x0b, 0x09, 0x06, 0x08, 0x0d, 0x0f, 0x05, //
        0x0e, 0x03, 0x01, 0x0c, 0x00, 0x07, 0x0a, 0x02  //
    };
    return &shuffle[0];
}

SZ_INTERNAL void _sz_hash_minimal_init_serial(_sz_hash_minimal_t *state, sz_u64_t seed) {

    // The key is made from the seed and half of it will be mixed with the length in the end
    state->key.u64s[1] = seed;
    state->key.u64s[0] = seed;

    // XOR the user-supplied keys with the two "pi" constants
    sz_u64_t const *pi = _sz_hash_pi_constants();
    state->aes.u64s[0] = seed ^ pi[0];
    state->aes.u64s[1] = seed ^ pi[1];
    state->sum.u64s[0] = seed ^ pi[8];
    state->sum.u64s[1] = seed ^ pi[9];
}

SZ_INTERNAL void _sz_hash_minimal_update_serial(_sz_hash_minimal_t *state, sz_u128_vec_t block) {
    sz_u8_t const *shuffle = _sz_hash_u8x16x4_shuffle();
    state->aes = _sz_emulate_aesenc_si128_serial(state->aes, block);
    state->sum = _sz_emulate_shuffle_epi8_serial(state->sum, shuffle);
    state->sum.u64s[0] += block.u64s[0], state->sum.u64s[1] += block.u64s[1];
}

SZ_INTERNAL sz_u64_t _sz_hash_minimal_finalize_serial(_sz_hash_minimal_t const *state, sz_size_t length) {
    // Mix the length into the key
    sz_u128_vec_t key_with_length = state->key;
    key_with_length.u64s[0] += length;
    // Combine the "sum" and the "AES" blocks
    sz_u128_vec_t mixed_registers = _sz_emulate_aesenc_si128_serial(state->sum, state->aes);
    // Make sure the "key" mixes enough with the state,
    // as with less than 2 rounds - SMHasher fails
    sz_u128_vec_t mixed_within_register = _sz_emulate_aesenc_si128_serial(
        _sz_emulate_aesenc_si128_serial(mixed_registers, key_with_length), mixed_registers);
    // Extract the low 64 bits
    return mixed_within_register.u64s[0];
}

SZ_INTERNAL void _sz_hash_shift_in_register_serial(sz_u128_vec_t *vec, int shift_bytes) {
    // One of the ridiculous things about x86, the `bsrli` instruction requires its operand to be an immediate.
    // On GCC and Clang, we could use the provided `__int128` type, but MSVC doesn't support it.
    // So we need to emulate it with 2x 64-bit shifts.
    if (shift_bytes >= 8) {
        vec->u64s[0] = (vec->u64s[1] >> (shift_bytes - 8) * 8);
        vec->u64s[1] = (0);
    }
    else if (shift_bytes) { //! If `shift_bytes == 0`, the shift would cause UB.
        vec->u64s[0] = (vec->u64s[0] >> shift_bytes * 8) | (vec->u64s[1] << (8 - shift_bytes) * 8);
        vec->u64s[1] = (vec->u64s[1] >> shift_bytes * 8);
    }
}

SZ_PUBLIC void sz_hash_state_init_serial(sz_hash_state_t *state, sz_u64_t seed) {

    // The key is made from the seed and half of it will be mixed with the length in the end
    state->key.u64s[0] = seed;
    state->key.u64s[1] = seed;

    // XOR the user-supplied keys with the two "pi" constants
    sz_u64_t const *pi = _sz_hash_pi_constants();
    for (int i = 0; i < 8; ++i) state->aes.u64s[i] = seed ^ pi[i];
    for (int i = 0; i < 8; ++i) state->sum.u64s[i] = seed ^ pi[i + 8];

    // The inputs are zeroed out at the beginning
    for (int i = 0; i < 8; ++i) state->ins.u64s[i] = 0;
    state->ins_length = 0;
}

SZ_INTERNAL void _sz_hash_state_update_serial(sz_hash_state_t *state) {
    sz_u8_t const *shuffle = _sz_hash_u8x16x4_shuffle();

    // To reuse the snippets above, let's cast to our familiar 128-bit vectors
    sz_u128_vec_t *aes_vecs = (sz_u128_vec_t *)&state->aes.u64s[0];
    sz_u128_vec_t *sum_vecs = (sz_u128_vec_t *)&state->sum.u64s[0];
    sz_u128_vec_t *ins_vecs = (sz_u128_vec_t *)&state->ins.u64s[0];

    // First 128-bit block
    aes_vecs[0] = _sz_emulate_aesenc_si128_serial(aes_vecs[0], ins_vecs[0]);
    sum_vecs[0] = _sz_emulate_shuffle_epi8_serial(sum_vecs[0], shuffle);
    sum_vecs[0].u64s[0] += ins_vecs[0].u64s[0], sum_vecs[0].u64s[1] += ins_vecs[0].u64s[1];

    // Second 128-bit block
    aes_vecs[1] = _sz_emulate_aesenc_si128_serial(aes_vecs[1], ins_vecs[1]);
    sum_vecs[1] = _sz_emulate_shuffle_epi8_serial(sum_vecs[1], shuffle);
    sum_vecs[1].u64s[0] += ins_vecs[1].u64s[0], sum_vecs[1].u64s[1] += ins_vecs[1].u64s[1];

    // Third 128-bit block
    aes_vecs[2] = _sz_emulate_aesenc_si128_serial(aes_vecs[2], ins_vecs[2]);
    sum_vecs[2] = _sz_emulate_shuffle_epi8_serial(sum_vecs[2], shuffle);
    sum_vecs[2].u64s[0] += ins_vecs[2].u64s[0], sum_vecs[2].u64s[1] += ins_vecs[2].u64s[1];

    // Fourth 128-bit block
    aes_vecs[3] = _sz_emulate_aesenc_si128_serial(aes_vecs[3], ins_vecs[3]);
    sum_vecs[3] = _sz_emulate_shuffle_epi8_serial(sum_vecs[3], shuffle);
    sum_vecs[3].u64s[0] += ins_vecs[3].u64s[0], sum_vecs[3].u64s[1] += ins_vecs[3].u64s[1];
}

SZ_INTERNAL sz_u64_t _sz_hash_state_finalize_serial(sz_hash_state_t const *state) {

    // Mix the length into the key
    sz_u128_vec_t key_with_length = state->key;
    key_with_length.u64s[0] += state->ins_length;

    // To reuse the snippets above, let's cast to our familiar 128-bit vectors
    sz_u128_vec_t *aes_vecs = (sz_u128_vec_t *)&state->aes.u64s[0];
    sz_u128_vec_t *sum_vecs = (sz_u128_vec_t *)&state->sum.u64s[0];

    // Combine the "sum" and the "AES" blocks
    sz_u128_vec_t mixed_registers0 = _sz_emulate_aesenc_si128_serial(sum_vecs[0], aes_vecs[0]);
    sz_u128_vec_t mixed_registers1 = _sz_emulate_aesenc_si128_serial(sum_vecs[1], aes_vecs[1]);
    sz_u128_vec_t mixed_registers2 = _sz_emulate_aesenc_si128_serial(sum_vecs[2], aes_vecs[2]);
    sz_u128_vec_t mixed_registers3 = _sz_emulate_aesenc_si128_serial(sum_vecs[3], aes_vecs[3]);

    // Combine the mixed registers
    sz_u128_vec_t mixed_registers01 = _sz_emulate_aesenc_si128_serial(mixed_registers0, mixed_registers1);
    sz_u128_vec_t mixed_registers23 = _sz_emulate_aesenc_si128_serial(mixed_registers2, mixed_registers3);
    sz_u128_vec_t mixed_registers = _sz_emulate_aesenc_si128_serial(mixed_registers01, mixed_registers23);

    // Make sure the "key" mixes enough with the state,
    // as with less than 2 rounds - SMHasher fails
    sz_u128_vec_t mixed_within_register = _sz_emulate_aesenc_si128_serial(
        _sz_emulate_aesenc_si128_serial(mixed_registers, key_with_length), mixed_registers);

    // Extract the low 64 bits
    return mixed_within_register.u64s[0];
}

SZ_PUBLIC sz_u64_t sz_hash_serial(sz_cptr_t start, sz_size_t length, sz_u64_t seed) {
    if (length <= 16) {
        // Initialize the AES block with a given seed
        _sz_hash_minimal_t state;
        _sz_hash_minimal_init_serial(&state, seed);
        // Load the data and update the state
        sz_u128_vec_t data_vec;
        data_vec.u64s[0] = data_vec.u64s[1] = 0;
        for (sz_size_t i = 0; i < length; ++i) data_vec.u8s[i] = start[i];
        _sz_hash_minimal_update_serial(&state, data_vec);
        return _sz_hash_minimal_finalize_serial(&state, length);
    }
    else if (length <= 32) {
        // Initialize the AES block with a given seed
        _sz_hash_minimal_t state;
        _sz_hash_minimal_init_serial(&state, seed);
        // Load the data and update the state
        sz_u128_vec_t data0_vec, data1_vec;
        data0_vec.u64s[0] = *(sz_u64_t const *)(start);
        data0_vec.u64s[1] = *(sz_u64_t const *)(start + 8);
        data1_vec.u64s[0] = *(sz_u64_t const *)(start + length - 16);
        data1_vec.u64s[1] = *(sz_u64_t const *)(start + length - 8);
        // Let's shift the data within the register to de-interleave the bytes.
        _sz_hash_shift_in_register_serial(&data1_vec, 32 - length);
        _sz_hash_minimal_update_serial(&state, data0_vec);
        _sz_hash_minimal_update_serial(&state, data1_vec);
        return _sz_hash_minimal_finalize_serial(&state, length);
    }
    else if (length <= 48) {
        // Initialize the AES block with a given seed
        _sz_hash_minimal_t state;
        _sz_hash_minimal_init_serial(&state, seed);
        // Load the data and update the state
        sz_u128_vec_t data0_vec, data1_vec, data2_vec;
        data0_vec.u64s[0] = *(sz_u64_t const *)(start);
        data0_vec.u64s[1] = *(sz_u64_t const *)(start + 8);
        data1_vec.u64s[0] = *(sz_u64_t const *)(start + 16);
        data1_vec.u64s[1] = *(sz_u64_t const *)(start + 24);
        data2_vec.u64s[0] = *(sz_u64_t const *)(start + length - 16);
        data2_vec.u64s[1] = *(sz_u64_t const *)(start + length - 8);
        // Let's shift the data within the register to de-interleave the bytes.
        _sz_hash_shift_in_register_serial(&data2_vec, 48 - length);
        _sz_hash_minimal_update_serial(&state, data0_vec);
        _sz_hash_minimal_update_serial(&state, data1_vec);
        _sz_hash_minimal_update_serial(&state, data2_vec);
        return _sz_hash_minimal_finalize_serial(&state, length);
    }
    else if (length <= 64) {
        // Initialize the AES block with a given seed
        _sz_hash_minimal_t state;
        _sz_hash_minimal_init_serial(&state, seed);
        // Load the data and update the state
        sz_u128_vec_t data0_vec, data1_vec, data2_vec, data3_vec;
        data0_vec.u64s[0] = *(sz_u64_t const *)(start);
        data0_vec.u64s[1] = *(sz_u64_t const *)(start + 8);
        data1_vec.u64s[0] = *(sz_u64_t const *)(start + 16);
        data1_vec.u64s[1] = *(sz_u64_t const *)(start + 24);
        data2_vec.u64s[0] = *(sz_u64_t const *)(start + 32);
        data2_vec.u64s[1] = *(sz_u64_t const *)(start + 40);
        data3_vec.u64s[0] = *(sz_u64_t const *)(start + length - 16);
        data3_vec.u64s[1] = *(sz_u64_t const *)(start + length - 8);
        // Let's shift the data within the register to de-interleave the bytes.
        _sz_hash_shift_in_register_serial(&data3_vec, 64 - length);
        _sz_hash_minimal_update_serial(&state, data0_vec);
        _sz_hash_minimal_update_serial(&state, data1_vec);
        _sz_hash_minimal_update_serial(&state, data2_vec);
        _sz_hash_minimal_update_serial(&state, data3_vec);
        return _sz_hash_minimal_finalize_serial(&state, length);
    }
    else {
        // Use a larger state to handle the main loop and add different offsets
        // to different lanes of the register
        sz_hash_state_t state;
        sz_hash_state_init_serial(&state, seed);

        for (; state.ins_length + 64 <= length; state.ins_length += 64) {
            state.ins.u64s[0] = *(sz_u64_t const *)(start + state.ins_length);
            state.ins.u64s[1] = *(sz_u64_t const *)(start + state.ins_length + 8);
            state.ins.u64s[2] = *(sz_u64_t const *)(start + state.ins_length + 16);
            state.ins.u64s[3] = *(sz_u64_t const *)(start + state.ins_length + 24);
            state.ins.u64s[4] = *(sz_u64_t const *)(start + state.ins_length + 32);
            state.ins.u64s[5] = *(sz_u64_t const *)(start + state.ins_length + 40);
            state.ins.u64s[6] = *(sz_u64_t const *)(start + state.ins_length + 48);
            state.ins.u64s[7] = *(sz_u64_t const *)(start + state.ins_length + 56);
            _sz_hash_state_update_serial(&state);
        }
        if (state.ins_length < length) {
            for (sz_size_t i = 0; i != 8; ++i) state.ins.u64s[i] = 0;
            for (sz_size_t i = 0; state.ins_length < length; ++i, ++state.ins_length)
                state.ins.u8s[i] = start[state.ins_length];
            _sz_hash_state_update_serial(&state);
            state.ins_length = length;
        }
        return _sz_hash_state_finalize_serial(&state);
    }
}

SZ_PUBLIC void sz_hash_state_stream_serial(sz_hash_state_t *state, sz_cptr_t text, sz_size_t length) {
    while (length) {
        sz_size_t progress_in_block = state->ins_length % 64;
        sz_size_t to_copy = sz_min_of_two(length, 64 - progress_in_block);
        int const will_fill_block = progress_in_block + to_copy == 64;
        // Update the metadata before we modify the `to_copy` variable
        state->ins_length += to_copy;
        length -= to_copy;
        // Append to the internal buffer until it's full
        while (to_copy--) state->ins.u8s[progress_in_block++] = *text++;
        // If we've reached the end of the buffer, update the state
        if (will_fill_block) {
            _sz_hash_state_update_serial(state);
            // Reset to zeros now, so we don't have to overwrite an immutable buffer in the folding state
            for (int i = 0; i < 8; ++i) state->ins.u64s[i] = 0;
        }
    }
}

SZ_PUBLIC sz_u64_t sz_hash_state_fold_serial(sz_hash_state_t const *state) {
    sz_size_t length = state->ins_length;
    if (length >= 64) return _sz_hash_state_finalize_serial(state);

    // Switch back to a smaller "minimal" state for small inputs
    _sz_hash_minimal_t minimal_state;
    minimal_state.key = state->key;
    minimal_state.aes = *(sz_u128_vec_t const *)&state->aes.u64s[0];
    minimal_state.sum = *(sz_u128_vec_t const *)&state->sum.u64s[0];

    // The logic is different depending on the length of the input
    sz_u128_vec_t const *ins_vecs = (sz_u128_vec_t const *)&state->ins.u64s[0];
    if (length <= 16) {
        _sz_hash_minimal_update_serial(&minimal_state, ins_vecs[0]);
        return _sz_hash_minimal_finalize_serial(&minimal_state, length);
    }
    else if (length <= 32) {
        _sz_hash_minimal_update_serial(&minimal_state, ins_vecs[0]);
        _sz_hash_minimal_update_serial(&minimal_state, ins_vecs[1]);
        return _sz_hash_minimal_finalize_serial(&minimal_state, length);
    }
    else if (length <= 48) {
        _sz_hash_minimal_update_serial(&minimal_state, ins_vecs[0]);
        _sz_hash_minimal_update_serial(&minimal_state, ins_vecs[1]);
        _sz_hash_minimal_update_serial(&minimal_state, ins_vecs[2]);
        return _sz_hash_minimal_finalize_serial(&minimal_state, length);
    }
    else {
        _sz_hash_minimal_update_serial(&minimal_state, ins_vecs[0]);
        _sz_hash_minimal_update_serial(&minimal_state, ins_vecs[1]);
        _sz_hash_minimal_update_serial(&minimal_state, ins_vecs[2]);
        _sz_hash_minimal_update_serial(&minimal_state, ins_vecs[3]);
        return _sz_hash_minimal_finalize_serial(&minimal_state, length);
    }
}

SZ_PUBLIC void sz_generate_serial(sz_ptr_t text, sz_size_t length, sz_u64_t nonce) {
    sz_u64_t const *pi_ptr = _sz_hash_pi_constants();
    sz_u128_vec_t input_vec, pi_vec, key_vec, generated_vec;
    for (sz_size_t lane_index = 0; length; ++lane_index) {
        // Each 128-bit block is initialized with the same nonce
        input_vec.u64s[0] = input_vec.u64s[1] = nonce + lane_index;
        // We rotate the first 512-bits of the Pi to mix with the nonce
        pi_vec = ((sz_u128_vec_t const *)pi_ptr)[lane_index % 4];
        key_vec.u64s[0] = nonce ^ pi_vec.u64s[0];
        key_vec.u64s[1] = nonce ^ pi_vec.u64s[1];
        generated_vec = _sz_emulate_aesenc_si128_serial(input_vec, key_vec);
        // Export back to the user-supplied buffer
        for (int i = 0; i < 16 && length; ++i, --length) *text++ = generated_vec.u8s[i];
    }
}

#pragma endregion // Serial Implementation

/*  AVX2 implementation of the string search algorithms for Haswell processors and newer.
 *  Very minimalistic (compared to AVX-512), but still faster than the serial implementation.
 */
#pragma region Haswell Implementation
#if SZ_USE_HASWELL
#pragma GCC push_options
#pragma GCC target("avx2")
#pragma clang attribute push(__attribute__((target("avx2"))), apply_to = function)

SZ_PUBLIC sz_u64_t sz_bytesum_haswell(sz_cptr_t text, sz_size_t length) {
    // The naive implementation of this function is very simple.
    // It assumes the CPU is great at handling unaligned "loads".
    //
    // A typical AWS Skylake instance can have 32 KB x 2 blocks of L1 data cache per core,
    // 1 MB x 2 blocks of L2 cache per core, and one shared L3 cache buffer.
    // For now, let's avoid the cases beyond the L2 size.
    int is_huge = length > 1ull * 1024ull * 1024ull;

    // When the buffer is small, there isn't much to innovate.
    if (length <= 32) { return sz_bytesum_serial(text, length); }
    else if (!is_huge) {
        sz_u256_vec_t text_vec, sums_vec;
        sums_vec.ymm = _mm256_setzero_si256();
        for (; length >= 32; text += 32, length -= 32) {
            text_vec.ymm = _mm256_lddqu_si256((__m256i const *)text);
            sums_vec.ymm = _mm256_add_epi64(sums_vec.ymm, _mm256_sad_epu8(text_vec.ymm, _mm256_setzero_si256()));
        }
        // We can also avoid the final serial loop by fetching 32 bytes from end, in reverse direction,
        // and shifting the data within the register to zero-out the duplicate bytes.

        // Accumulating 256 bits is harder, as we need to extract the 128-bit sums first.
        __m128i low_xmm = _mm256_castsi256_si128(sums_vec.ymm);
        __m128i high_xmm = _mm256_extracti128_si256(sums_vec.ymm, 1);
        __m128i sums_xmm = _mm_add_epi64(low_xmm, high_xmm);
        sz_u64_t low = (sz_u64_t)_mm_cvtsi128_si64(sums_xmm);
        sz_u64_t high = (sz_u64_t)_mm_extract_epi64(sums_xmm, 1);
        sz_u64_t result = low + high;
        if (length) result += sz_bytesum_serial(text, length);
        return result;
    }
    // For gigantic buffers, exceeding typical L1 cache sizes, there are other tricks we can use.
    // Most notably, we can avoid populating the cache with the entire buffer, and instead traverse it in 2 directions.
    else {
        sz_size_t head_length = (32 - ((sz_size_t)text % 32)) % 32; // 31 or less.
        sz_size_t tail_length = (sz_size_t)(text + length) % 32;    // 31 or less.
        sz_size_t body_length = length - head_length - tail_length; // Multiple of 32.
        sz_u64_t result = 0;

        // Handle the tail before we start updating the `text` pointer
        while (tail_length) result += text[length - (tail_length--)];
        // Handle the head
        while (head_length--) result += *text++;

        sz_u256_vec_t text_vec, sums_vec;
        sums_vec.ymm = _mm256_setzero_si256();
        // Fill the aligned body of the buffer.
        if (!is_huge) {
            for (; body_length >= 32; text += 32, body_length -= 32) {
                text_vec.ymm = _mm256_stream_load_si256((__m256i const *)text);
                sums_vec.ymm = _mm256_add_epi64(sums_vec.ymm, _mm256_sad_epu8(text_vec.ymm, _mm256_setzero_si256()));
            }
        }
        // When the buffer is huge, we can traverse it in 2 directions.
        else {
            sz_u256_vec_t text_reversed_vec, sums_reversed_vec;
            sums_reversed_vec.ymm = _mm256_setzero_si256();
            for (; body_length >= 64; text += 32, body_length -= 64) {
                text_vec.ymm = _mm256_stream_load_si256((__m256i *)(text));
                sums_vec.ymm = _mm256_add_epi64(sums_vec.ymm, _mm256_sad_epu8(text_vec.ymm, _mm256_setzero_si256()));
                text_reversed_vec.ymm = _mm256_stream_load_si256((__m256i *)(text + body_length - 32));
                sums_reversed_vec.ymm = _mm256_add_epi64(
                    sums_reversed_vec.ymm, _mm256_sad_epu8(text_reversed_vec.ymm, _mm256_setzero_si256()));
            }
            if (body_length >= 32) {
                _sz_assert(body_length == 32);
                text_vec.ymm = _mm256_stream_load_si256((__m256i *)(text));
                sums_vec.ymm = _mm256_add_epi64(sums_vec.ymm, _mm256_sad_epu8(text_vec.ymm, _mm256_setzero_si256()));
                text += 32;
            }
            sums_vec.ymm = _mm256_add_epi64(sums_vec.ymm, sums_reversed_vec.ymm);
        }

        // Accumulating 256 bits is harder, as we need to extract the 128-bit sums first.
        __m128i low_xmm = _mm256_castsi256_si128(sums_vec.ymm);
        __m128i high_xmm = _mm256_extracti128_si256(sums_vec.ymm, 1);
        __m128i sums_xmm = _mm_add_epi64(low_xmm, high_xmm);
        sz_u64_t low = (sz_u64_t)_mm_cvtsi128_si64(sums_xmm);
        sz_u64_t high = (sz_u64_t)_mm_extract_epi64(sums_xmm, 1);
        result += low + high;
        return result;
    }
}

SZ_INTERNAL void _sz_hash_minimal_init_haswell(_sz_hash_minimal_t *state, sz_u64_t seed) {

    // The key is made from the seed and half of it will be mixed with the length in the end
    __m128i seed_vec = _mm_set1_epi64x(seed);
    state->key.xmm = seed_vec;

    // XOR the user-supplied keys with the two "pi" constants
    sz_u64_t const *pi = _sz_hash_pi_constants();
    __m128i const pi0 = _mm_load_si128((__m128i const *)(pi));
    __m128i const pi1 = _mm_load_si128((__m128i const *)(pi + 8));
    __m128i k1 = _mm_xor_si128(seed_vec, pi0);
    __m128i k2 = _mm_xor_si128(seed_vec, pi1);

    // The first 128 bits of the "sum" and "AES" blocks are the same
    state->aes.xmm = k1;
    state->sum.xmm = k2;
}

SZ_INTERNAL sz_u64_t _sz_hash_minimal_finalize_haswell(_sz_hash_minimal_t const *state, sz_size_t length) {
    // Mix the length into the key
    __m128i key_with_length = _mm_add_epi64(state->key.xmm, _mm_set_epi64x(0, length));
    // Combine the "sum" and the "AES" blocks
    __m128i mixed_registers = _mm_aesenc_si128(state->sum.xmm, state->aes.xmm);
    // Make sure the "key" mixes enough with the state,
    // as with less than 2 rounds - SMHasher fails
    __m128i mixed_within_register =
        _mm_aesenc_si128(_mm_aesenc_si128(mixed_registers, key_with_length), mixed_registers);
    // Extract the low 64 bits
    return _mm_cvtsi128_si64(mixed_within_register);
}

SZ_INTERNAL void _sz_hash_minimal_update_haswell(_sz_hash_minimal_t *state, __m128i block) {
    __m128i const shuffle_mask = _mm_load_si128((__m128i const *)_sz_hash_u8x16x4_shuffle());
    state->aes.xmm = _mm_aesenc_si128(state->aes.xmm, block);
    state->sum.xmm = _mm_add_epi64(_mm_shuffle_epi8(state->sum.xmm, shuffle_mask), block);
}

SZ_PUBLIC void sz_hash_state_init_haswell(sz_hash_state_t *state, sz_u64_t seed) {
    // The key is made from the seed and half of it will be mixed with the length in the end
    __m128i seed_vec = _mm_set1_epi64x(seed);
    state->key.xmm = seed_vec;

    // XOR the user-supplied keys with the two "pi" constants
    sz_u64_t const *pi = _sz_hash_pi_constants();
    for (int i = 0; i < 4; ++i)
        state->aes.xmms[i] = _mm_xor_si128(seed_vec, _mm_load_si128((__m128i const *)(pi + i * 2)));
    for (int i = 0; i < 4; ++i)
        state->sum.xmms[i] = _mm_xor_si128(seed_vec, _mm_load_si128((__m128i const *)(pi + i * 2 + 8)));

    // The inputs are zeroed out at the beginning
    state->ins.xmms[0] = state->ins.xmms[1] = state->ins.xmms[2] = state->ins.xmms[3] = _mm_setzero_si128();
    state->ins_length = 0;
}

SZ_INTERNAL void _sz_hash_state_update_haswell(sz_hash_state_t *state) {
    __m128i const shuffle_mask = _mm_load_si128((__m128i const *)_sz_hash_u8x16x4_shuffle());
    state->aes.xmms[0] = _mm_aesenc_si128(state->aes.xmms[0], state->ins.xmms[0]);
    state->sum.xmms[0] = _mm_add_epi64(_mm_shuffle_epi8(state->sum.xmms[0], shuffle_mask), state->ins.xmms[0]);
    state->aes.xmms[1] = _mm_aesenc_si128(state->aes.xmms[1], state->ins.xmms[1]);
    state->sum.xmms[1] = _mm_add_epi64(_mm_shuffle_epi8(state->sum.xmms[1], shuffle_mask), state->ins.xmms[1]);
    state->aes.xmms[2] = _mm_aesenc_si128(state->aes.xmms[2], state->ins.xmms[2]);
    state->sum.xmms[2] = _mm_add_epi64(_mm_shuffle_epi8(state->sum.xmms[2], shuffle_mask), state->ins.xmms[2]);
    state->aes.xmms[3] = _mm_aesenc_si128(state->aes.xmms[3], state->ins.xmms[3]);
    state->sum.xmms[3] = _mm_add_epi64(_mm_shuffle_epi8(state->sum.xmms[3], shuffle_mask), state->ins.xmms[3]);
}

SZ_INTERNAL sz_u64_t _sz_hash_state_finalize_haswell(sz_hash_state_t const *state) {
    // Mix the length into the key
    __m128i key_with_length = _mm_add_epi64(state->key.xmm, _mm_set_epi64x(0, state->ins_length));
    // Combine the "sum" and the "AES" blocks
    __m128i mixed_registers0 = _mm_aesenc_si128(state->sum.xmms[0], state->aes.xmms[0]);
    __m128i mixed_registers1 = _mm_aesenc_si128(state->sum.xmms[1], state->aes.xmms[1]);
    __m128i mixed_registers2 = _mm_aesenc_si128(state->sum.xmms[2], state->aes.xmms[2]);
    __m128i mixed_registers3 = _mm_aesenc_si128(state->sum.xmms[3], state->aes.xmms[3]);
    // Combine the mixed registers
    __m128i mixed_registers01 = _mm_aesenc_si128(mixed_registers0, mixed_registers1);
    __m128i mixed_registers23 = _mm_aesenc_si128(mixed_registers2, mixed_registers3);
    __m128i mixed_registers = _mm_aesenc_si128(mixed_registers01, mixed_registers23);
    // Make sure the "key" mixes enough with the state,
    // as with less than 2 rounds - SMHasher fails
    __m128i mixed_within_register =
        _mm_aesenc_si128(_mm_aesenc_si128(mixed_registers, key_with_length), mixed_registers);
    // Extract the low 64 bits
    return _mm_cvtsi128_si64(mixed_within_register);
}

SZ_PUBLIC sz_u64_t sz_hash_haswell(sz_cptr_t start, sz_size_t length, sz_u64_t seed) {

    if (length <= 16) {
        // Initialize the AES block with a given seed
        _sz_hash_minimal_t state;
        _sz_hash_minimal_init_haswell(&state, seed);
        // Load the data and update the state
        sz_u128_vec_t data_vec;
        data_vec.xmm = _mm_setzero_si128();
        for (sz_size_t i = 0; i < length; ++i) data_vec.u8s[i] = start[i];
        _sz_hash_minimal_update_haswell(&state, data_vec.xmm);
        return _sz_hash_minimal_finalize_haswell(&state, length);
    }
    else if (length <= 32) {
        // Initialize the AES block with a given seed
        _sz_hash_minimal_t state;
        _sz_hash_minimal_init_haswell(&state, seed);
        // Load the data and update the state
        sz_u128_vec_t data0_vec, data1_vec;
        data0_vec.xmm = _mm_lddqu_si128((__m128i const *)(start));
        data1_vec.xmm = _mm_lddqu_si128((__m128i const *)(start + length - 16));
        // Let's shift the data within the register to de-interleave the bytes.
        _sz_hash_shift_in_register_serial(&data1_vec, 32 - length);
        _sz_hash_minimal_update_haswell(&state, data0_vec.xmm);
        _sz_hash_minimal_update_haswell(&state, data1_vec.xmm);
        return _sz_hash_minimal_finalize_haswell(&state, length);
    }
    else if (length <= 48) {
        // Initialize the AES block with a given seed
        _sz_hash_minimal_t state;
        _sz_hash_minimal_init_haswell(&state, seed);
        // Load the data and update the state
        sz_u128_vec_t data0_vec, data1_vec, data2_vec;
        data0_vec.xmm = _mm_lddqu_si128((__m128i const *)(start));
        data1_vec.xmm = _mm_lddqu_si128((__m128i const *)(start + 16));
        data2_vec.xmm = _mm_lddqu_si128((__m128i const *)(start + length - 16));
        // Let's shift the data within the register to de-interleave the bytes.
        _sz_hash_shift_in_register_serial(&data2_vec, 48 - length);
        _sz_hash_minimal_update_haswell(&state, data0_vec.xmm);
        _sz_hash_minimal_update_haswell(&state, data1_vec.xmm);
        _sz_hash_minimal_update_haswell(&state, data2_vec.xmm);
        return _sz_hash_minimal_finalize_haswell(&state, length);
    }
    else if (length <= 64) {
        // Initialize the AES block with a given seed
        _sz_hash_minimal_t state;
        _sz_hash_minimal_init_haswell(&state, seed);
        // Load the data and update the state
        sz_u128_vec_t data0_vec, data1_vec, data2_vec, data3_vec;
        data0_vec.xmm = _mm_lddqu_si128((__m128i const *)(start));
        data1_vec.xmm = _mm_lddqu_si128((__m128i const *)(start + 16));
        data2_vec.xmm = _mm_lddqu_si128((__m128i const *)(start + 32));
        data3_vec.xmm = _mm_lddqu_si128((__m128i const *)(start + length - 16));
        // Let's shift the data within the register to de-interleave the bytes.
        _sz_hash_shift_in_register_serial(&data3_vec, 64 - length);
        _sz_hash_minimal_update_haswell(&state, data0_vec.xmm);
        _sz_hash_minimal_update_haswell(&state, data1_vec.xmm);
        _sz_hash_minimal_update_haswell(&state, data2_vec.xmm);
        _sz_hash_minimal_update_haswell(&state, data3_vec.xmm);
        return _sz_hash_minimal_finalize_haswell(&state, length);
    }
    else {
        // Use a larger state to handle the main loop and add different offsets
        // to different lanes of the register
        sz_hash_state_t state;
        sz_hash_state_init_haswell(&state, seed);
        for (; state.ins_length + 64 <= length; state.ins_length += 64) {
            state.ins.xmms[0] = _mm_lddqu_si128((__m128i const *)(start + state.ins_length));
            state.ins.xmms[1] = _mm_lddqu_si128((__m128i const *)(start + state.ins_length + 16));
            state.ins.xmms[2] = _mm_lddqu_si128((__m128i const *)(start + state.ins_length + 32));
            state.ins.xmms[3] = _mm_lddqu_si128((__m128i const *)(start + state.ins_length + 48));
            _sz_hash_state_update_haswell(&state);
        }
        // Handle the tail, resetting the registers to zero first
        if (state.ins_length < length) {
            state.ins.xmms[0] = _mm_setzero_si128();
            state.ins.xmms[1] = _mm_setzero_si128();
            state.ins.xmms[2] = _mm_setzero_si128();
            state.ins.xmms[3] = _mm_setzero_si128();
            for (sz_size_t i = 0; state.ins_length < length; ++i, ++state.ins_length)
                state.ins.u8s[i] = start[state.ins_length];
            _sz_hash_state_update_haswell(&state);
            state.ins_length = length;
        }
        return _sz_hash_state_finalize_haswell(&state);
    }
}

SZ_PUBLIC void sz_hash_state_stream_haswell(sz_hash_state_t *state, sz_cptr_t text, sz_size_t length) {
    while (length) {
        // Append to the internal buffer until it's full
        if (state->ins_length % 64 == 0 && length >= 64) {
            state->ins.xmms[0] = _mm_lddqu_si128((__m128i const *)text);
            state->ins.xmms[1] = _mm_lddqu_si128((__m128i const *)(text + 16));
            state->ins.xmms[2] = _mm_lddqu_si128((__m128i const *)(text + 32));
            state->ins.xmms[3] = _mm_lddqu_si128((__m128i const *)(text + 48));
            _sz_hash_state_update_haswell(state);
            state->ins_length += 64;
            text += 64;
            length -= 64;
        }
        // If vectorization isn't that trivial - fall back to the serial implementation
        else {
            sz_size_t progress_in_block = state->ins_length % 64;
            sz_size_t to_copy = sz_min_of_two(length, 64 - progress_in_block);
            int const will_fill_block = progress_in_block + to_copy == 64;
            // Update the metadata before we modify the `to_copy` variable
            state->ins_length += to_copy;
            length -= to_copy;
            // Append to the internal buffer until it's full
            while (to_copy--) state->ins.u8s[progress_in_block++] = *text++;
            // If we've reached the end of the buffer, update the state
            if (will_fill_block) {
                _sz_hash_state_update_haswell(state);
                // Reset to zeros now, so we don't have to overwrite an immutable buffer in the folding state
                for (int i = 0; i < 4; ++i) state->ins.xmms[i] = _mm_setzero_si128();
            }
        }
    }
}

SZ_PUBLIC sz_u64_t sz_hash_state_fold_haswell(sz_hash_state_t const *state) {
    sz_size_t length = state->ins_length;
    if (length >= 64) return _sz_hash_state_finalize_haswell(state);

    // Switch back to a smaller "minimal" state for small inputs
    _sz_hash_minimal_t minimal_state;
    minimal_state.key.xmm = state->key.xmm;
    minimal_state.aes.xmm = state->aes.xmms[0];
    minimal_state.sum.xmm = state->sum.xmms[0];

    // The logic is different depending on the length of the input
    __m128i const *ins_vecs = (__m128i const *)&state->ins.xmms[0];
    if (length <= 16) {
        _sz_hash_minimal_update_haswell(&minimal_state, ins_vecs[0]);
        return _sz_hash_minimal_finalize_haswell(&minimal_state, length);
    }
    else if (length <= 32) {
        _sz_hash_minimal_update_haswell(&minimal_state, ins_vecs[0]);
        _sz_hash_minimal_update_haswell(&minimal_state, ins_vecs[1]);
        return _sz_hash_minimal_finalize_haswell(&minimal_state, length);
    }
    else if (length <= 48) {
        _sz_hash_minimal_update_haswell(&minimal_state, ins_vecs[0]);
        _sz_hash_minimal_update_haswell(&minimal_state, ins_vecs[1]);
        _sz_hash_minimal_update_haswell(&minimal_state, ins_vecs[2]);
        return _sz_hash_minimal_finalize_haswell(&minimal_state, length);
    }
    else {
        _sz_hash_minimal_update_haswell(&minimal_state, ins_vecs[0]);
        _sz_hash_minimal_update_haswell(&minimal_state, ins_vecs[1]);
        _sz_hash_minimal_update_haswell(&minimal_state, ins_vecs[2]);
        _sz_hash_minimal_update_haswell(&minimal_state, ins_vecs[3]);
        return _sz_hash_minimal_finalize_haswell(&minimal_state, length);
    }
}

SZ_PUBLIC void sz_generate_haswell(sz_ptr_t text, sz_size_t length, sz_u64_t nonce) {
    sz_u64_t const *pi_ptr = _sz_hash_pi_constants();
    if (length <= 16) {
        __m128i input = _mm_set1_epi64x(nonce);
        __m128i pi = _mm_load_si128((__m128i const *)pi_ptr);
        __m128i key = _mm_xor_si128(_mm_set1_epi64x(nonce), pi);
        __m128i generated = _mm_aesenc_si128(input, key);
        // Now the tricky part is outputting this data to the user-supplied buffer
        // without masked writes, like in AVX-512.
        for (sz_size_t i = 0; i < length; ++i) text[i] = ((sz_u8_t *)&generated)[i];
    }
    // Assuming the YMM register contains two 128-bit blocks, the input to the generator
    // will be more complex, containing the sum of the nonce and the block number.
    else if (length <= 32) {
        __m128i inputs[2], pis[2], keys[2], generated[2];
        inputs[0] = _mm_set1_epi64x(nonce);
        inputs[1] = _mm_set1_epi64x(nonce + 1);
        pis[0] = _mm_load_si128((__m128i const *)(pi_ptr));
        pis[1] = _mm_load_si128((__m128i const *)(pi_ptr + 2));
        keys[0] = _mm_xor_si128(_mm_set1_epi64x(nonce), pis[0]);
        keys[1] = _mm_xor_si128(_mm_set1_epi64x(nonce), pis[1]);
        generated[0] = _mm_aesenc_si128(inputs[0], keys[0]);
        generated[1] = _mm_aesenc_si128(inputs[1], keys[1]);
        // The first store can easily be vectorized, but the second can be serial for now
        _mm_storeu_si128((__m128i *)text, generated[0]);
        for (sz_size_t i = 16; i < length; ++i) text[i] = ((sz_u8_t *)&generated[1])[i - 16];
    }
    // The last special case we handle outside of the primary loop is for buffers up to 64 bytes long.
    else if (length <= 48) {
        __m128i inputs[3], pis[3], keys[3], generated[3];
        inputs[0] = _mm_set1_epi64x(nonce);
        inputs[1] = _mm_set1_epi64x(nonce + 1);
        inputs[2] = _mm_set1_epi64x(nonce + 2);
        pis[0] = _mm_load_si128((__m128i const *)(pi_ptr));
        pis[1] = _mm_load_si128((__m128i const *)(pi_ptr + 2));
        pis[2] = _mm_load_si128((__m128i const *)(pi_ptr + 4));
        keys[0] = _mm_xor_si128(_mm_set1_epi64x(nonce), pis[0]);
        keys[1] = _mm_xor_si128(_mm_set1_epi64x(nonce), pis[1]);
        keys[2] = _mm_xor_si128(_mm_set1_epi64x(nonce), pis[2]);
        generated[0] = _mm_aesenc_si128(inputs[0], keys[0]);
        generated[1] = _mm_aesenc_si128(inputs[1], keys[1]);
        generated[2] = _mm_aesenc_si128(inputs[2], keys[2]);
        // The first store can easily be vectorized, but the second can be serial for now
        _mm_storeu_si128((__m128i *)text, generated[0]);
        _mm_storeu_si128((__m128i *)(text + 16), generated[1]);
        for (sz_size_t i = 32; i < length; ++i) text[i] = ((sz_u8_t *)generated)[i];
    }
    // The final part of the function is the primary loop, which processes the buffer in 64-byte chunks.
    else {
        __m128i inputs[4], pis[4], keys[4], generated[4];
        inputs[0] = _mm_set1_epi64x(nonce);
        inputs[1] = _mm_set1_epi64x(nonce + 1);
        inputs[2] = _mm_set1_epi64x(nonce + 2);
        inputs[3] = _mm_set1_epi64x(nonce + 3);
        // Load parts of PI into the registers
        pis[0] = _mm_load_si128((__m128i const *)(pi_ptr));
        pis[1] = _mm_load_si128((__m128i const *)(pi_ptr + 2));
        pis[2] = _mm_load_si128((__m128i const *)(pi_ptr + 4));
        pis[3] = _mm_load_si128((__m128i const *)(pi_ptr + 6));
        // XOR the nonce with the PI constants
        keys[0] = _mm_xor_si128(_mm_set1_epi64x(nonce), pis[0]);
        keys[1] = _mm_xor_si128(_mm_set1_epi64x(nonce), pis[1]);
        keys[2] = _mm_xor_si128(_mm_set1_epi64x(nonce), pis[2]);
        keys[3] = _mm_xor_si128(_mm_set1_epi64x(nonce), pis[3]);

        // Produce the output, fixing the key and enumerating input chunks.
        sz_size_t i = 0;
        __m128i const increment = _mm_set1_epi64x(4);
        for (; i + 64 <= length; i += 64) {
            generated[0] = _mm_aesenc_si128(inputs[0], keys[0]);
            generated[1] = _mm_aesenc_si128(inputs[1], keys[1]);
            generated[2] = _mm_aesenc_si128(inputs[2], keys[2]);
            generated[3] = _mm_aesenc_si128(inputs[3], keys[3]);
            _mm_storeu_si128((__m128i *)(text + i), generated[0]);
            _mm_storeu_si128((__m128i *)(text + i + 16), generated[1]);
            _mm_storeu_si128((__m128i *)(text + i + 32), generated[2]);
            _mm_storeu_si128((__m128i *)(text + i + 48), generated[3]);
            inputs[0] = _mm_add_epi64(inputs[0], increment);
            inputs[1] = _mm_add_epi64(inputs[1], increment);
            inputs[2] = _mm_add_epi64(inputs[2], increment);
            inputs[3] = _mm_add_epi64(inputs[3], increment);
        }

        // Handle the tail of the buffer.
        {
            generated[0] = _mm_aesenc_si128(inputs[0], keys[0]);
            generated[1] = _mm_aesenc_si128(inputs[1], keys[1]);
            generated[2] = _mm_aesenc_si128(inputs[2], keys[2]);
            generated[3] = _mm_aesenc_si128(inputs[3], keys[3]);
            for (sz_size_t j = 0; i < length; ++i, ++j) text[i] = ((sz_u8_t *)generated)[j];
        }
    }
}

#pragma clang attribute pop
#pragma GCC pop_options
#endif            // SZ_USE_HASWELL
#pragma endregion // Haswell Implementation

/*  AVX512 implementation of the string hashing algorithms for Skylake and newer CPUs.
 *  Includes extensions: F, CD, ER, PF, VL, DQ, BW.
 *
 *  This is the "starting level" for the advanced algorithms using K-mask registers on x86.
 */
#pragma region Skylake Implementation
#if SZ_USE_SKYLAKE
#pragma GCC push_options
#pragma GCC target("avx", "avx512f", "avx512vl", "avx512bw", "bmi", "bmi2")
#pragma clang attribute push(__attribute__((target("avx,avx512f,avx512vl,avx512bw,bmi,bmi2"))), apply_to = function)

SZ_PUBLIC sz_u64_t sz_bytesum_skylake(sz_cptr_t text, sz_size_t length) {
    // The naive implementation of this function is very simple.
    // It assumes the CPU is great at handling unaligned "loads".
    //
    // A typical AWS Sapphire Rapids instance can have 48 KB x 2 blocks of L1 data cache per core,
    // 2 MB x 2 blocks of L2 cache per core, and one shared 60 MB buffer of L3 cache.
    // With two strings, we may consider the overall workload huge, if each exceeds 1 MB in length.
    int const is_huge = length >= 1ull * 1024ull * 1024ull;
    sz_u512_vec_t text_vec, sums_vec;

    // When the buffer is small, there isn't much to innovate.
    // Separately handling even smaller payloads doesn't increase performance even on synthetic benchmarks.
    if (length <= 16) {
        __mmask16 mask = _sz_u16_mask_until(length);
        text_vec.xmms[0] = _mm_maskz_loadu_epi8(mask, text);
        sums_vec.xmms[0] = _mm_sad_epu8(text_vec.xmms[0], _mm_setzero_si128());
        sz_u64_t low = (sz_u64_t)_mm_cvtsi128_si64(sums_vec.xmms[0]);
        sz_u64_t high = (sz_u64_t)_mm_extract_epi64(sums_vec.xmms[0], 1);
        return low + high;
    }
    else if (length <= 32) {
        __mmask32 mask = _sz_u32_mask_until(length);
        text_vec.ymms[0] = _mm256_maskz_loadu_epi8(mask, text);
        sums_vec.ymms[0] = _mm256_sad_epu8(text_vec.ymms[0], _mm256_setzero_si256());
        // Accumulating 256 bits is harder, as we need to extract the 128-bit sums first.
        __m128i low_xmm = _mm256_castsi256_si128(sums_vec.ymms[0]);
        __m128i high_xmm = _mm256_extracti128_si256(sums_vec.ymms[0], 1);
        __m128i sums_xmm = _mm_add_epi64(low_xmm, high_xmm);
        sz_u64_t low = (sz_u64_t)_mm_cvtsi128_si64(sums_xmm);
        sz_u64_t high = (sz_u64_t)_mm_extract_epi64(sums_xmm, 1);
        return low + high;
    }
    else if (length <= 64) {
        __mmask64 mask = _sz_u64_mask_until(length);
        text_vec.zmm = _mm512_maskz_loadu_epi8(mask, text);
        sums_vec.zmm = _mm512_sad_epu8(text_vec.zmm, _mm512_setzero_si512());
        return _mm512_reduce_add_epi64(sums_vec.zmm);
    }
    // For large buffers, fitting into L1 cache sizes, there are other tricks we can use.
    //
    // 1. Moving in both directions to maximize the throughput, when fetching from multiple
    //    memory pages. Also helps with cache set-associativity issues, as we won't always
    //    be fetching the same buckets in the lookup table.
    //
    // Bidirectional traversal generally adds about 10% to such algorithms.
    else if (!is_huge) {
        sz_size_t head_length = (64 - ((sz_size_t)text % 64)) % 64; // 63 or less.
        sz_size_t tail_length = (sz_size_t)(text + length) % 64;    // 63 or less.
        sz_size_t body_length = length - head_length - tail_length; // Multiple of 64.
        _sz_assert(body_length % 64 == 0 && head_length < 64 && tail_length < 64);
        __mmask64 head_mask = _sz_u64_mask_until(head_length);
        __mmask64 tail_mask = _sz_u64_mask_until(tail_length);

        text_vec.zmm = _mm512_maskz_loadu_epi8(head_mask, text);
        sums_vec.zmm = _mm512_sad_epu8(text_vec.zmm, _mm512_setzero_si512());
        for (text += head_length; body_length >= 64; text += 64, body_length -= 64) {
            text_vec.zmm = _mm512_load_si512((__m512i const *)text);
            sums_vec.zmm = _mm512_add_epi64(sums_vec.zmm, _mm512_sad_epu8(text_vec.zmm, _mm512_setzero_si512()));
        }
        text_vec.zmm = _mm512_maskz_loadu_epi8(tail_mask, text);
        sums_vec.zmm = _mm512_add_epi64(sums_vec.zmm, _mm512_sad_epu8(text_vec.zmm, _mm512_setzero_si512()));
        return _mm512_reduce_add_epi64(sums_vec.zmm);
    }
    // For gigantic buffers, exceeding typical L1 cache sizes, there are other tricks we can use.
    //
    // 1. Using non-temporal loads to avoid polluting the cache.
    // 2. Prefetching the next cache line, to avoid stalling the CPU. This generally useless
    //    for predictable patterns, so disregard this advice.
    //
    // Bidirectional traversal generally adds about 10% to such algorithms.
    else {
        sz_u512_vec_t text_reversed_vec, sums_reversed_vec;
        sz_size_t head_length = (64 - ((sz_size_t)text % 64)) % 64;
        sz_size_t tail_length = (sz_size_t)(text + length) % 64;
        sz_size_t body_length = length - head_length - tail_length;
        __mmask64 head_mask = _sz_u64_mask_until(head_length);
        __mmask64 tail_mask = _sz_u64_mask_until(tail_length);

        text_vec.zmm = _mm512_maskz_loadu_epi8(head_mask, text);
        sums_vec.zmm = _mm512_sad_epu8(text_vec.zmm, _mm512_setzero_si512());
        text_reversed_vec.zmm = _mm512_maskz_loadu_epi8(tail_mask, text + head_length + body_length);
        sums_reversed_vec.zmm = _mm512_sad_epu8(text_reversed_vec.zmm, _mm512_setzero_si512());

        // Now in the main loop, we can use non-temporal loads, performing the operation in both directions.
        for (text += head_length; body_length >= 128; text += 64, body_length -= 128) {
            text_vec.zmm = _mm512_stream_load_si512((__m512i *)(text));
            sums_vec.zmm = _mm512_add_epi64(sums_vec.zmm, _mm512_sad_epu8(text_vec.zmm, _mm512_setzero_si512()));
            text_reversed_vec.zmm = _mm512_stream_load_si512((__m512i *)(text + body_length - 64));
            sums_reversed_vec.zmm =
                _mm512_add_epi64(sums_reversed_vec.zmm, _mm512_sad_epu8(text_reversed_vec.zmm, _mm512_setzero_si512()));
        }
        if (body_length >= 64) {
            text_vec.zmm = _mm512_stream_load_si512((__m512i *)(text));
            sums_vec.zmm = _mm512_add_epi64(sums_vec.zmm, _mm512_sad_epu8(text_vec.zmm, _mm512_setzero_si512()));
        }

        return _mm512_reduce_add_epi64(_mm512_add_epi64(sums_vec.zmm, sums_reversed_vec.zmm));
    }
}

SZ_PUBLIC void sz_hash_state_init_skylake(sz_hash_state_t *state, sz_u64_t seed) {
    // The key is made from the seed and half of it will be mixed with the length in the end
    __m512i seed_vec = _mm512_set1_epi64(seed);
    state->key.xmm = _mm512_castsi512_si128(seed_vec);

    // XOR the user-supplied keys with the two "pi" constants
    sz_u64_t const *pi = _sz_hash_pi_constants();
    __m512i const pi0 = _mm512_load_epi64((__m512i const *)(pi));
    __m512i const pi1 = _mm512_load_epi64((__m512i const *)(pi + 8));
    state->aes.zmm = _mm512_xor_si512(seed_vec, pi0);
    state->sum.zmm = _mm512_xor_si512(seed_vec, pi1);

    // The inputs are zeroed out at the beginning
    state->ins.zmm = _mm512_setzero_si512();
    state->ins_length = 0;
}

SZ_PUBLIC sz_u64_t sz_hash_skylake(sz_cptr_t start, sz_size_t length, sz_u64_t seed) {

    if (length <= 16) {
        // Initialize the AES block with a given seed
        _sz_hash_minimal_t state;
        _sz_hash_minimal_init_haswell(&state, seed);
        // Load the data and update the state
        sz_u128_vec_t data_vec;
        data_vec.xmm = _mm_maskz_loadu_epi8(_sz_u16_mask_until(length), start);
        _sz_hash_minimal_update_haswell(&state, data_vec.xmm);
        return _sz_hash_minimal_finalize_haswell(&state, length);
    }
    else if (length <= 32) {
        // Initialize the AES block with a given seed
        _sz_hash_minimal_t state;
        _sz_hash_minimal_init_haswell(&state, seed);
        // Load the data and update the state
        sz_u128_vec_t data0_vec, data1_vec;
        data0_vec.xmm = _mm_lddqu_si128((__m128i const *)(start));
        data1_vec.xmm = _mm_maskz_loadu_epi8(_sz_u16_mask_until(length - 16), start + 16);
        _sz_hash_minimal_update_haswell(&state, data0_vec.xmm);
        _sz_hash_minimal_update_haswell(&state, data1_vec.xmm);
        return _sz_hash_minimal_finalize_haswell(&state, length);
    }
    else if (length <= 48) {
        // Initialize the AES block with a given seed
        _sz_hash_minimal_t state;
        _sz_hash_minimal_init_haswell(&state, seed);
        // Load the data and update the state
        sz_u128_vec_t data0_vec, data1_vec, data2_vec;
        data0_vec.xmm = _mm_lddqu_si128((__m128i const *)(start));
        data1_vec.xmm = _mm_lddqu_si128((__m128i const *)(start + 16));
        data2_vec.xmm = _mm_maskz_loadu_epi8(_sz_u16_mask_until(length - 32), start + 32);
        _sz_hash_minimal_update_haswell(&state, data0_vec.xmm);
        _sz_hash_minimal_update_haswell(&state, data1_vec.xmm);
        _sz_hash_minimal_update_haswell(&state, data2_vec.xmm);
        return _sz_hash_minimal_finalize_haswell(&state, length);
    }
    else if (length <= 64) {
        // Initialize the AES block with a given seed
        _sz_hash_minimal_t state;
        _sz_hash_minimal_init_haswell(&state, seed);
        // Load the data and update the state
        sz_u128_vec_t data0_vec, data1_vec, data2_vec, data3_vec;
        data0_vec.xmm = _mm_lddqu_si128((__m128i const *)(start));
        data1_vec.xmm = _mm_lddqu_si128((__m128i const *)(start + 16));
        data2_vec.xmm = _mm_lddqu_si128((__m128i const *)(start + 32));
        data3_vec.xmm = _mm_maskz_loadu_epi8(_sz_u16_mask_until(length - 48), start + 48);
        _sz_hash_minimal_update_haswell(&state, data0_vec.xmm);
        _sz_hash_minimal_update_haswell(&state, data1_vec.xmm);
        _sz_hash_minimal_update_haswell(&state, data2_vec.xmm);
        _sz_hash_minimal_update_haswell(&state, data3_vec.xmm);
        return _sz_hash_minimal_finalize_haswell(&state, length);
    }
    else {
        // Use a larger state to handle the main loop and add different offsets
        // to different lanes of the register
        sz_hash_state_t state;
        sz_hash_state_init_skylake(&state, seed);

        for (; state.ins_length + 64 <= length; state.ins_length += 64) {
            state.ins.zmm = _mm512_loadu_epi8(start + state.ins_length);
            _sz_hash_state_update_haswell(&state);
        }
        if (state.ins_length < length) {
            state.ins.zmm = _mm512_maskz_loadu_epi8( //
                _sz_u64_mask_until(length - state.ins_length), start + state.ins_length);
            _sz_hash_state_update_haswell(&state);
            state.ins_length = length;
        }
        return _sz_hash_state_finalize_haswell(&state);
    }
}

SZ_PUBLIC void sz_hash_state_stream_skylake(sz_hash_state_t *state, sz_cptr_t text, sz_size_t length) {
    while (length) {
        sz_size_t const progress_in_block = state->ins_length % 64;
        sz_size_t const to_copy = sz_min_of_two(length, 64 - progress_in_block);
        int const will_fill_block = progress_in_block + to_copy == 64;
        // Update the metadata before we modify the `to_copy` variable
        state->ins_length += to_copy;
        length -= to_copy;
        // Append to the internal buffer until it's full
        __mmask64 to_copy_mask = _sz_u64_mask_until(to_copy);
        _mm512_mask_storeu_epi8(&state->ins.u8s[0] + progress_in_block, to_copy_mask,
                                _mm512_maskz_loadu_epi8(to_copy_mask, text));
        text += to_copy;
        // If we've reached the end of the buffer, update the state
        if (will_fill_block) {
            _sz_hash_state_update_haswell(state);
            // Reset to zeros now, so we don't have to overwrite an immutable buffer in the folding state
            state->ins.zmm = _mm512_setzero_si512();
        }
    }
}

SZ_PUBLIC sz_u64_t sz_hash_state_fold_skylake(sz_hash_state_t const *state) {
    //? We don't know a better way to fold the state on Ice Lake, than to use the Haswell implementation.
    return sz_hash_state_fold_haswell(state);
}

SZ_PUBLIC void sz_generate_skylake(sz_ptr_t text, sz_size_t length, sz_u64_t nonce) {
    sz_generate_serial(text, length, nonce);
}

#pragma clang attribute pop
#pragma GCC pop_options
#endif            // SZ_USE_SKYLAKE
#pragma endregion // Skylake Implementation

/*  AVX512 implementation of the string search algorithms for Ice Lake and newer CPUs.
 *  Includes extensions:
 *  - 2017 Skylake: F, CD, ER, PF, VL, DQ, BW,
 *  - 2018 CannonLake: IFMA, VBMI,
 *  - 2019 Ice Lake: VPOPCNTDQ, VNNI, VBMI2, BITALG, GFNI, VPCLMULQDQ, VAES.
 */
#pragma region Ice Lake Implementation
#if SZ_USE_ICE
#pragma GCC push_options
#pragma GCC target("avx", "avx512f", "avx512vl", "avx512bw", "avx512dq", "avx512vbmi", "avx512vnni", "bmi", "bmi2", \
                   "aes", "vaes")
#pragma clang attribute push(                                                                                  \
    __attribute__((target("avx,avx512f,avx512vl,avx512bw,avx512dq,avx512vbmi,avx512vnni,bmi,bmi2,aes,vaes"))), \
    apply_to = function)

SZ_PUBLIC sz_u64_t sz_bytesum_ice(sz_cptr_t text, sz_size_t length) {
    // The naive implementation of this function is very simple.
    // It assumes the CPU is great at handling unaligned "loads".
    //
    // A typical AWS Sapphire Rapids instance can have 48 KB x 2 blocks of L1 data cache per core,
    // 2 MB x 2 blocks of L2 cache per core, and one shared 60 MB buffer of L3 cache.
    // With two strings, we may consider the overall workload huge, if each exceeds 1 MB in length.
    int const is_huge = length >= 1ull * 1024ull * 1024ull;
    sz_u512_vec_t text_vec, sums_vec;

    // When the buffer is small, there isn't much to innovate.
    // Separately handling even smaller payloads doesn't increase performance even on synthetic benchmarks.
    if (length <= 16) {
        __mmask16 mask = _sz_u16_mask_until(length);
        text_vec.xmms[0] = _mm_maskz_loadu_epi8(mask, text);
        sums_vec.xmms[0] = _mm_sad_epu8(text_vec.xmms[0], _mm_setzero_si128());
        sz_u64_t low = (sz_u64_t)_mm_cvtsi128_si64(sums_vec.xmms[0]);
        sz_u64_t high = (sz_u64_t)_mm_extract_epi64(sums_vec.xmms[0], 1);
        return low + high;
    }
    else if (length <= 32) {
        __mmask32 mask = _sz_u32_mask_until(length);
        text_vec.ymms[0] = _mm256_maskz_loadu_epi8(mask, text);
        sums_vec.ymms[0] = _mm256_sad_epu8(text_vec.ymms[0], _mm256_setzero_si256());
        // Accumulating 256 bits is harder, as we need to extract the 128-bit sums first.
        __m128i low_xmm = _mm256_castsi256_si128(sums_vec.ymms[0]);
        __m128i high_xmm = _mm256_extracti128_si256(sums_vec.ymms[0], 1);
        __m128i sums_xmm = _mm_add_epi64(low_xmm, high_xmm);
        sz_u64_t low = (sz_u64_t)_mm_cvtsi128_si64(sums_xmm);
        sz_u64_t high = (sz_u64_t)_mm_extract_epi64(sums_xmm, 1);
        return low + high;
    }
    else if (length <= 64) {
        __mmask64 mask = _sz_u64_mask_until(length);
        text_vec.zmm = _mm512_maskz_loadu_epi8(mask, text);
        sums_vec.zmm = _mm512_sad_epu8(text_vec.zmm, _mm512_setzero_si512());
        return _mm512_reduce_add_epi64(sums_vec.zmm);
    }
    // For large buffers, fitting into L1 cache sizes, there are other tricks we can use.
    //
    // 1. Moving in both directions to maximize the throughput, when fetching from multiple
    //    memory pages. Also helps with cache set-associativity issues, as we won't always
    //    be fetching the same buckets in the lookup table.
    // 2. Port-level parallelism, can be used to hide the latency of expensive SIMD instructions.
    //    - `VPSADBW (ZMM, ZMM, ZMM)` combination with `VPADDQ (ZMM, ZMM, ZMM)`:
    //        - On Ice Lake, the `VPSADBW` is 3 cycles on port 5; the `VPADDQ` is 1 cycle on ports 0/5.
    //        - On Zen 4, the `VPSADBW` is 3 cycles on ports 0/1; the `VPADDQ` is 1 cycle on ports 0/1/2/3.
    //    - `VPDPBUSDS (ZMM, ZMM, ZMM)`:
    //        - On Ice Lake, the `VPDPBUSDS` is 5 cycles on port 0.
    //        - On Zen 4, the `VPDPBUSDS` is 4 cycles on ports 0/1.
    //
    // Bidirectional traversal generally adds about 10% to such algorithms.
    // Port level parallelism can yield more, but remember that one of the instructions accumulates
    // with 32-bit integers and the other one will be using 64-bit integers.
    else if (!is_huge) {
        sz_size_t head_length = (64 - ((sz_size_t)text % 64)) % 64; // 63 or less.
        sz_size_t tail_length = (sz_size_t)(text + length) % 64;    // 63 or less.
        sz_size_t body_length = length - head_length - tail_length; // Multiple of 64.
        _sz_assert(body_length % 64 == 0 && head_length < 64 && tail_length < 64);
        __mmask64 head_mask = _sz_u64_mask_until(head_length);
        __mmask64 tail_mask = _sz_u64_mask_until(tail_length);

        sz_u512_vec_t zeros_vec, ones_vec;
        zeros_vec.zmm = _mm512_setzero_si512();
        ones_vec.zmm = _mm512_set1_epi8(1);

        // Take care of the unaligned head and tail!
        sz_u512_vec_t text_reversed_vec, sums_reversed_vec;
        text_vec.zmm = _mm512_maskz_loadu_epi8(head_mask, text);
        sums_vec.zmm = _mm512_sad_epu8(text_vec.zmm, zeros_vec.zmm);
        text_reversed_vec.zmm = _mm512_maskz_loadu_epi8(tail_mask, text + head_length + body_length);
        sums_reversed_vec.zmm = _mm512_dpbusds_epi32(zeros_vec.zmm, text_reversed_vec.zmm, ones_vec.zmm);

        // Now in the main loop, we can use aligned loads, performing the operation in both directions.
        for (text += head_length; body_length >= 128; text += 64, body_length -= 128) {
            text_reversed_vec.zmm = _mm512_load_si512((__m512i *)(text + body_length - 64));
            sums_reversed_vec.zmm = _mm512_dpbusds_epi32(sums_reversed_vec.zmm, text_reversed_vec.zmm, ones_vec.zmm);
            text_vec.zmm = _mm512_load_si512((__m512i *)(text));
            sums_vec.zmm = _mm512_add_epi64(sums_vec.zmm, _mm512_sad_epu8(text_vec.zmm, zeros_vec.zmm));
        }
        // There may be an aligned chunk of 64 bytes left.
        if (body_length >= 64) {
            _sz_assert(body_length == 64);
            text_vec.zmm = _mm512_load_si512((__m512i *)(text));
            sums_vec.zmm = _mm512_add_epi64(sums_vec.zmm, _mm512_sad_epu8(text_vec.zmm, zeros_vec.zmm));
        }

        return _mm512_reduce_add_epi64(sums_vec.zmm) + _mm512_reduce_add_epi32(sums_reversed_vec.zmm);
    }
    // For gigantic buffers, exceeding typical L1 cache sizes, there are other tricks we can use.
    //
    // 1. Using non-temporal loads to avoid polluting the cache.
    // 2. Prefetching the next cache line, to avoid stalling the CPU. This generally useless
    //    for predictable patterns, so disregard this advice.
    //
    // Bidirectional traversal generally adds about 10% to such algorithms.
    else {
        sz_u512_vec_t text_reversed_vec, sums_reversed_vec;
        sz_size_t head_length = (64 - ((sz_size_t)text % 64)) % 64;
        sz_size_t tail_length = (sz_size_t)(text + length) % 64;
        sz_size_t body_length = length - head_length - tail_length;
        __mmask64 head_mask = _sz_u64_mask_until(head_length);
        __mmask64 tail_mask = _sz_u64_mask_until(tail_length);

        text_vec.zmm = _mm512_maskz_loadu_epi8(head_mask, text);
        sums_vec.zmm = _mm512_sad_epu8(text_vec.zmm, _mm512_setzero_si512());
        text_reversed_vec.zmm = _mm512_maskz_loadu_epi8(tail_mask, text + head_length + body_length);
        sums_reversed_vec.zmm = _mm512_sad_epu8(text_reversed_vec.zmm, _mm512_setzero_si512());

        // Now in the main loop, we can use non-temporal loads, performing the operation in both directions.
        for (text += head_length; body_length >= 128; text += 64, body_length -= 128) {
            text_vec.zmm = _mm512_stream_load_si512((__m512i *)(text));
            sums_vec.zmm = _mm512_add_epi64(sums_vec.zmm, _mm512_sad_epu8(text_vec.zmm, _mm512_setzero_si512()));
            text_reversed_vec.zmm = _mm512_stream_load_si512((__m512i *)(text + body_length - 64));
            sums_reversed_vec.zmm =
                _mm512_add_epi64(sums_reversed_vec.zmm, _mm512_sad_epu8(text_reversed_vec.zmm, _mm512_setzero_si512()));
        }
        if (body_length >= 64) {
            text_vec.zmm = _mm512_stream_load_si512((__m512i *)(text));
            sums_vec.zmm = _mm512_add_epi64(sums_vec.zmm, _mm512_sad_epu8(text_vec.zmm, _mm512_setzero_si512()));
        }

        return _mm512_reduce_add_epi64(_mm512_add_epi64(sums_vec.zmm, sums_reversed_vec.zmm));
    }
}

SZ_INTERNAL void _sz_hash_state_update_ice(sz_hash_state_t *state) {
    __m512i const shuffle_mask = _mm512_load_si512((__m512i const *)_sz_hash_u8x16x4_shuffle());
    state->aes.zmm = _mm512_aesenc_epi128(state->aes.zmm, state->ins.zmm);
    state->sum.zmm = _mm512_add_epi64(_mm512_shuffle_epi8(state->sum.zmm, shuffle_mask), state->ins.zmm);
}

SZ_PUBLIC sz_u64_t sz_hash_ice(sz_cptr_t start, sz_size_t length, sz_u64_t seed) {

    if (length <= 16) {
        // Initialize the AES block with a given seed
        _sz_hash_minimal_t state;
        _sz_hash_minimal_init_haswell(&state, seed);
        // Load the data and update the state
        sz_u128_vec_t data_vec;
        data_vec.xmm = _mm_maskz_loadu_epi8(_sz_u16_mask_until(length), start);
        _sz_hash_minimal_update_haswell(&state, data_vec.xmm);
        return _sz_hash_minimal_finalize_haswell(&state, length);
    }
    else if (length <= 32) {
        // Initialize the AES block with a given seed
        _sz_hash_minimal_t state;
        _sz_hash_minimal_init_haswell(&state, seed);
        // Load the data and update the state
        sz_u128_vec_t data0_vec, data1_vec;
        data0_vec.xmm = _mm_lddqu_si128((__m128i const *)(start));
        data1_vec.xmm = _mm_maskz_loadu_epi8(_sz_u16_mask_until(length - 16), start + 16);
        _sz_hash_minimal_update_haswell(&state, data0_vec.xmm);
        _sz_hash_minimal_update_haswell(&state, data1_vec.xmm);
        return _sz_hash_minimal_finalize_haswell(&state, length);
    }
    else if (length <= 48) {
        // Initialize the AES block with a given seed
        _sz_hash_minimal_t state;
        _sz_hash_minimal_init_haswell(&state, seed);
        // Load the data and update the state
        sz_u128_vec_t data0_vec, data1_vec, data2_vec;
        data0_vec.xmm = _mm_lddqu_si128((__m128i const *)(start));
        data1_vec.xmm = _mm_lddqu_si128((__m128i const *)(start + 16));
        data2_vec.xmm = _mm_maskz_loadu_epi8(_sz_u16_mask_until(length - 32), start + 32);
        _sz_hash_minimal_update_haswell(&state, data0_vec.xmm);
        _sz_hash_minimal_update_haswell(&state, data1_vec.xmm);
        _sz_hash_minimal_update_haswell(&state, data2_vec.xmm);
        return _sz_hash_minimal_finalize_haswell(&state, length);
    }
    else if (length <= 64) {
        // Initialize the AES block with a given seed
        _sz_hash_minimal_t state;
        _sz_hash_minimal_init_haswell(&state, seed);
        // Load the data and update the state
        sz_u128_vec_t data0_vec, data1_vec, data2_vec, data3_vec;
        data0_vec.xmm = _mm_lddqu_si128((__m128i const *)(start));
        data1_vec.xmm = _mm_lddqu_si128((__m128i const *)(start + 16));
        data2_vec.xmm = _mm_lddqu_si128((__m128i const *)(start + 32));
        data3_vec.xmm = _mm_maskz_loadu_epi8(_sz_u16_mask_until(length - 48), start + 48);
        _sz_hash_minimal_update_haswell(&state, data0_vec.xmm);
        _sz_hash_minimal_update_haswell(&state, data1_vec.xmm);
        _sz_hash_minimal_update_haswell(&state, data2_vec.xmm);
        _sz_hash_minimal_update_haswell(&state, data3_vec.xmm);
        return _sz_hash_minimal_finalize_haswell(&state, length);
    }
    else {
        // Use a larger state to handle the main loop and add different offsets
        // to different lanes of the register
        sz_hash_state_t state;
        sz_hash_state_init_skylake(&state, seed);

        for (; state.ins_length + 64 <= length; state.ins_length += 64) {
            state.ins.zmm = _mm512_loadu_epi8(start + state.ins_length);
            _sz_hash_state_update_ice(&state);
        }
        if (state.ins_length < length) {
            state.ins.zmm = _mm512_maskz_loadu_epi8( //
                _sz_u64_mask_until(length - state.ins_length), start + state.ins_length);
            _sz_hash_state_update_ice(&state);
            state.ins_length = length;
        }
        return _sz_hash_state_finalize_haswell(&state);
    }
}

SZ_PUBLIC void sz_hash_state_init_ice(sz_hash_state_t *state, sz_u64_t seed) {
    sz_hash_state_init_skylake(state, seed);
}

SZ_PUBLIC void sz_hash_state_stream_ice(sz_hash_state_t *state, sz_cptr_t text, sz_size_t length) {
    while (length) {
        sz_size_t progress_in_block = state->ins_length % 64;
        sz_size_t to_copy = sz_min_of_two(length, 64 - progress_in_block);
        int const will_fill_block = progress_in_block + to_copy == 64;
        // Update the metadata before we modify the `to_copy` variable
        state->ins_length += to_copy;
        length -= to_copy;
        // Append to the internal buffer until it's full
        __mmask64 to_copy_mask = _sz_u64_mask_until(to_copy);
        _mm512_mask_storeu_epi8(state->ins.u8s + progress_in_block, to_copy_mask,
                                _mm512_maskz_loadu_epi8(to_copy_mask, text));
        text += to_copy;
        // If we've reached the end of the buffer, update the state
        if (will_fill_block) {
            _sz_hash_state_update_ice(state);
            // Reset to zeros now, so we don't have to overwrite an immutable buffer in the folding state
            state->ins.zmm = _mm512_setzero_si512();
        }
    }
}

SZ_PUBLIC sz_u64_t sz_hash_state_fold_ice(sz_hash_state_t const *state) {
    //? We don't know a better way to fold the state on Ice Lake, than to use the Haswell implementation.
    return sz_hash_state_fold_haswell(state);
}

SZ_PUBLIC void sz_generate_ice(sz_ptr_t output, sz_size_t length, sz_u64_t nonce) {
    if (length <= 16) {
        __m128i input = _mm_set1_epi64x(nonce);
        __m128i pi = _mm_load_si128((__m128i const *)_sz_hash_pi_constants());
        __m128i key = _mm_xor_si128(_mm_set1_epi64x(nonce), pi);
        __m128i generated = _mm_aesenc_si128(input, key);
        __mmask16 store_mask = _sz_u16_mask_until(length);
        _mm_mask_storeu_epi8((void *)output, store_mask, generated);
    }
    // Assuming the YMM register contains two 128-bit blocks, the input to the generator
    // will be more complex, containing the sum of the nonce and the block number.
    else if (length <= 32) {
        __m256i input = _mm256_set_epi64x(nonce + 1, nonce + 1, nonce, nonce);
        __m256i pi = _mm256_load_si256((__m256i const *)_sz_hash_pi_constants());
        __m256i key = _mm256_xor_si256(_mm256_set1_epi64x(nonce), pi);
        __m256i generated = _mm256_aesenc_epi128(input, key);
        __mmask32 store_mask = _sz_u32_mask_until(length);
        _mm256_mask_storeu_epi8((void *)output, store_mask, generated);
    }
    // The last special case we handle outside of the primary loop is for buffers up to 64 bytes long.
    else if (length <= 64) {
        __m512i input = _mm512_set_epi64(               //
            nonce + 3, nonce + 3, nonce + 2, nonce + 2, //
            nonce + 1, nonce + 1, nonce, nonce);
        __m512i pi = _mm512_load_si512((__m512i const *)_sz_hash_pi_constants());
        __m512i key = _mm512_xor_si512(_mm512_set1_epi64(nonce), pi);
        __m512i generated = _mm512_aesenc_epi128(input, key);
        __mmask64 store_mask = _sz_u64_mask_until(length);
        _mm512_mask_storeu_epi8((void *)output, store_mask, generated);
    }
    // The final part of the function is the primary loop, which processes the buffer in 64-byte chunks.
    else {
        __m512i const increment = _mm512_set1_epi64(4);
        __m512i input = _mm512_set_epi64(               //
            nonce + 3, nonce + 3, nonce + 2, nonce + 2, //
            nonce + 1, nonce + 1, nonce, nonce);
        __m512i const pi = _mm512_load_si512((__m512i const *)_sz_hash_pi_constants());
        __m512i const key = _mm512_xor_si512(_mm512_set1_epi64(nonce), pi);

        // Produce the output, fixing the key and enumerating input chunks.
        sz_size_t i = 0;
        for (; i + 64 <= length; i += 64) {
            __m512i generated = _mm512_aesenc_epi128(input, key);
            _mm512_storeu_epi8((void *)(output + i), generated);
            input = _mm512_add_epi64(input, increment);
        }

        // Handle the tail of the buffer.
        __m512i generated = _mm512_aesenc_epi128(input, key);
        __mmask64 store_mask = _sz_u64_mask_until(length - i);
        _mm512_mask_storeu_epi8((void *)(output + i), store_mask, generated);
    }
}

#pragma clang attribute pop
#pragma GCC pop_options
#endif            // SZ_USE_ICE
#pragma endregion // Ice Lake Implementation

/*  Implementation of the string hashing algorithms using the Arm NEON instruction set, available on 64-bit
 *  Arm processors. Covers billions of mobile CPUs worldwide, including Apple's A-series, and Qualcomm's Snapdragon.
 */
#pragma region NEON Implementation
#if SZ_USE_NEON
#pragma GCC push_options
#pragma GCC target("arch=armv8.2-a+simd")
#pragma clang attribute push(__attribute__((target("arch=armv8.2-a+simd"))), apply_to = function)

SZ_PUBLIC sz_u64_t sz_bytesum_neon(sz_cptr_t text, sz_size_t length) {
    uint64x2_t sum_vec = vdupq_n_u64(0);

    // Process 16 bytes (128 bits) at a time
    for (; length >= 16; text += 16, length -= 16) {
        uint8x16_t vec = vld1q_u8((sz_u8_t const *)text);      // Load 16 bytes
        uint16x8_t pairwise_sum1 = vpaddlq_u8(vec);            // Pairwise add lower and upper 8 bits
        uint32x4_t pairwise_sum2 = vpaddlq_u16(pairwise_sum1); // Pairwise add 16-bit results
        uint64x2_t pairwise_sum3 = vpaddlq_u32(pairwise_sum2); // Pairwise add 32-bit results
        sum_vec = vaddq_u64(sum_vec, pairwise_sum3);           // Accumulate the sum
    }

    // Final reduction of `sum_vec` to a single scalar
    sz_u64_t sum = vgetq_lane_u64(sum_vec, 0) + vgetq_lane_u64(sum_vec, 1);
    if (length) sum += sz_bytesum_serial(text, length);
    return sum;
}

SZ_PUBLIC void sz_hash_state_init_neon(sz_hash_state_t *state, sz_u64_t seed) {
    sz_hash_state_init_serial(state, seed);
}

SZ_PUBLIC void sz_hash_state_stream_neon(sz_hash_state_t *state, sz_cptr_t text, sz_size_t length) {
    sz_hash_state_stream_serial(state, text, length);
}

SZ_PUBLIC sz_u64_t sz_hash_state_fold_neon(sz_hash_state_t const *state) { return sz_hash_state_fold_serial(state); }

#pragma clang attribute pop
#pragma GCC pop_options
#endif            // SZ_USE_NEON
#pragma endregion // NEON Implementation

/*  Implementation of the string search algorithms using the Arm SVE variable-length registers,
 *  available in Arm v9 processors, like in Apple M4+ and Graviton 3+ CPUs.
 */
#pragma region SVE Implementation
#if SZ_USE_SVE
#pragma GCC push_options
#pragma GCC target("arch=armv8.2-a+sve")
#pragma clang attribute push(__attribute__((target("arch=armv8.2-a+sve"))), apply_to = function)

#pragma clang attribute pop
#pragma GCC pop_options
#endif            // SZ_USE_SVE
#pragma endregion // SVE Implementation

/*  Pick the right implementation for the string search algorithms.
 *  To override this behavior and precompile all backends - set `SZ_DYNAMIC_DISPATCH` to 1.
 */
#pragma region Compile Time Dispatching
#if !SZ_DYNAMIC_DISPATCH

SZ_DYNAMIC sz_u64_t sz_bytesum(sz_cptr_t text, sz_size_t length) {
#if SZ_USE_ICE
    return sz_bytesum_ice(text, length);
#elif SZ_USE_SKYLAKE
    return sz_bytesum_skylake(text, length);
#elif SZ_USE_HASWELL
    return sz_bytesum_haswell(text, length);
#elif SZ_USE_NEON
    return sz_bytesum_neon(text, length);
#else
    return sz_bytesum_serial(text, length);
#endif
}

SZ_DYNAMIC sz_u64_t sz_hash(sz_cptr_t text, sz_size_t length, sz_u64_t seed) {
#if SZ_USE_ICE
    return sz_hash_ice(text, length, seed);
#elif SZ_USE_SKYLAKE
    return sz_hash_skylake(text, length, seed);
#elif SZ_USE_HASWELL
    return sz_hash_haswell(text, length, seed);
#elif SZ_USE_NEON
    return sz_hash_neon(text, length, seed);
#else
    return sz_hash_serial(text, length, seed);
#endif
}

SZ_DYNAMIC void sz_generate(sz_ptr_t text, sz_size_t length, sz_u64_t nonce) {
#if SZ_USE_ICE
    sz_generate_ice(text, length, nonce);
#elif SZ_USE_SKYLAKE
    sz_generate_skylake(text, length, nonce);
#elif SZ_USE_HASWELL
    sz_generate_haswell(text, length, nonce);
#elif SZ_USE_NEON
    sz_generate_neon(text, length, nonce);
#else
    sz_generate_serial(text, length, nonce);
#endif
}

SZ_DYNAMIC void sz_hash_state_init(sz_hash_state_t *state, sz_u64_t seed) {
#if SZ_USE_ICE
    sz_hash_state_init_ice(state, seed);
#elif SZ_USE_SKYLAKE
    sz_hash_state_init_skylake(state, seed);
#elif SZ_USE_HASWELL
    sz_hash_state_init_haswell(state, seed);
#elif SZ_USE_NEON
    sz_hash_state_init_neon(state, seed);
#else
    sz_hash_state_init_serial(state, seed);
#endif
}

SZ_DYNAMIC void sz_hash_state_stream(sz_hash_state_t *state, sz_cptr_t text, sz_size_t length) {
#if SZ_USE_ICE
    sz_hash_state_stream_ice(state, text, length);
#elif SZ_USE_SKYLAKE
    sz_hash_state_stream_skylake(state, text, length);
#elif SZ_USE_HASWELL
    sz_hash_state_stream_haswell(state, text, length);
#elif SZ_USE_NEON
    sz_hash_state_stream_neon(state, text, length);
#else
    sz_hash_state_stream_serial(state, text, length);
#endif
}

SZ_DYNAMIC sz_u64_t sz_hash_state_fold(sz_hash_state_t const *state) {
#if SZ_USE_ICE
    return sz_hash_state_fold_ice(state);
#elif SZ_USE_SKYLAKE
    return sz_hash_state_fold_skylake(state);
#elif SZ_USE_HASWELL
    return sz_hash_state_fold_haswell(state);
#elif SZ_USE_NEON
    return sz_hash_state_fold_neon(state);
#else
    return sz_hash_state_fold_serial(state);
#endif
}

#endif            // !SZ_DYNAMIC_DISPATCH
#pragma endregion // Compile Time Dispatching

#ifdef __cplusplus
}
#endif // __cplusplus
#endif // STRINGZILLA_HASH_H_
