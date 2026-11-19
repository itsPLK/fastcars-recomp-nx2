#pragma once
// Byte swaps of 16 bytes at a time (big-endian guest data to host order): SSSE3 on x86-64, NEON on
// arm64 (macOS, Android). unit = 4 (32-bit words) or 2 (16-bit halves).

#include <cstdint>

#include <arm_neon.h>
#define FASTCARS_SWAP16_TARGET
namespace fastcars::native {
inline void Swap16Bytes(uint8_t* out, const uint8_t* in, uint32_t unit) {
  uint8x16_t v = vld1q_u8(in);
  vst1q_u8(out, unit == 4 ? vrev32q_u8(v) : vrev16q_u8(v));
}
}  // namespace fastcars::native
