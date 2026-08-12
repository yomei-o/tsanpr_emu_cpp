// The host's own AES unit, on whichever host this is.
//
// x86-64 fuses a whole AES round into one instruction: AESDEC is InvShiftRows +
// InvSubBytes + InvMixColumns + AddRoundKey.  AArch64's ARMv8 Crypto extension
// splits the same work in two - AESD is AddRoundKey + InvSubBytes + InvShiftRows,
// and AESIMC is InvMixColumns on its own - so a round is AESD against a zero key,
// then AESIMC, then the real round key XORed in.  Both compute FIPS-197, so both
// give the same answer as the emulator's byte-at-a-time S-box loop; the only
// difference is how long it takes.  tools_aesvec.c checks that against the
// standard's own vectors, through the emulator, on whatever host it is built for.
//
// Where neither unit exists - wasm - nothing is defined here and the callers keep
// their software path.  X86EMU_NO_HOST_AES forces that path on a host that has
// one, which is how the two are compared.
#pragma once

#include <cstdint>

#if defined(X86EMU_NO_HOST_AES) || defined(__wasm__) || defined(__EMSCRIPTEN__)
// no host AES
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define X86EMU_HOST_AES 1
#define X86EMU_HOST_AES_X86 1
#include <wmmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#define X86EMU_HOST_AES 1
#define X86EMU_HOST_AES_ARM 1
#include <arm_neon.h>
#endif

#if defined(X86EMU_HOST_AES)

namespace x86emu {
namespace hostaes {

#if defined(X86EMU_HOST_AES_X86)

using Block = __m128i;
inline Block load(const uint8_t* p) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)); }
inline void store(uint8_t* p, Block v) { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), v); }
inline Block zero() { return _mm_setzero_si128(); }
inline Block xorb(Block a, Block b) { return _mm_xor_si128(a, b); }
inline Block enc(Block s, Block k) { return _mm_aesenc_si128(s, k); }
inline Block enclast(Block s, Block k) { return _mm_aesenclast_si128(s, k); }
inline Block dec(Block s, Block k) { return _mm_aesdec_si128(s, k); }
inline Block declast(Block s, Block k) { return _mm_aesdeclast_si128(s, k); }
inline Block imc(Block s) { return _mm_aesimc_si128(s); }

#else  // X86EMU_HOST_AES_ARM

using Block = uint8x16_t;
inline Block load(const uint8_t* p) { return vld1q_u8(p); }
inline void store(uint8_t* p, Block v) { vst1q_u8(p, v); }
inline Block zero() { return vdupq_n_u8(0); }
inline Block xorb(Block a, Block b) { return veorq_u8(a, b); }
// AESE with a zero key is SubBytes+ShiftRows on its own; AESMC is MixColumns.
inline Block enc(Block s, Block k) { return veorq_u8(vaesmcq_u8(vaeseq_u8(s, zero())), k); }
inline Block enclast(Block s, Block k) { return veorq_u8(vaeseq_u8(s, zero()), k); }
inline Block dec(Block s, Block k) { return veorq_u8(vaesimcq_u8(vaesdq_u8(s, zero())), k); }
inline Block declast(Block s, Block k) { return veorq_u8(vaesdq_u8(s, zero()), k); }
inline Block imc(Block s) { return vaesimcq_u8(s); }

#endif

}  // namespace hostaes
}  // namespace x86emu

#endif  // X86EMU_HOST_AES
