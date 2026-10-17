// SPDX-License-Identifier: MIT
// Native ARM64EC version of D2R's MD5 update and block transform, run by NativeRedirect.cpp in place of the game's
// emulated copies. The game's context layout and update logic are mirrored exactly:
//   +0x00 uint64_t bit count, advanced by 512 per full block only
//   +0x08 uint32_t state[4] (A, B, C, D)
//   +0x18 uint32_t bytes waiting in the buffer
//   +0x1c uint8_t  buffer[64]
// The transform is standard MD5; NativeRedirect.cpp checks the game's own round tables against the standard ones before
// switching this on.

#include "NativeStats.h"

#include <stdint.h>

struct FEXNativeCallStats FEXNativeMD5UpdateStats;
struct FEXNativeCallStats FEXNativeMD5TransformStats;

static inline uint64_t ReadTicks(void) {
  uint64_t Value = 0;
#ifdef FEX_NATIVE_ARM64
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(Value));
#endif
  return Value;
}

static inline void Count(struct FEXNativeCallStats* Stats, uint64_t Start, uint64_t Bytes) {
  const uint64_t End = ReadTicks();
  __atomic_fetch_add(&Stats->Calls, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&Stats->Ticks, End - Start, __ATOMIC_RELAXED);
  __atomic_fetch_add(&Stats->Bytes, Bytes, __ATOMIC_RELAXED);
}

static inline uint32_t Load32(const uint8_t* Ptr) {
  uint32_t Value;
  __builtin_memcpy(&Value, Ptr, sizeof(Value));
  return Value;
}

static inline void Store32(uint8_t* Ptr, uint32_t Value) {
  __builtin_memcpy(Ptr, &Value, sizeof(Value));
}

static inline uint64_t Load64(const uint8_t* Ptr) {
  uint64_t Value;
  __builtin_memcpy(&Value, Ptr, sizeof(Value));
  return Value;
}

static inline void Store64(uint8_t* Ptr, uint64_t Value) {
  __builtin_memcpy(Ptr, &Value, sizeof(Value));
}

#define MD5_F(x, y, z) ((((y) ^ (z)) & (x)) ^ (z))
#define MD5_G(x, y, z) ((((x) ^ (y)) & (z)) ^ (y))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | ~(z)))
#define MD5_STEP(f, a, b, c, d, x, t, s) \
  (a) += f((b), (c), (d)) + (x) + (uint32_t)(t); \
  (a) = ((a) << (s)) | ((a) >> (32 - (s))); \
  (a) += (b);

static void Transform(uint8_t* Ctx, const uint8_t* Block) {
  uint32_t X[16];
  for (int i = 0; i < 16; ++i) {
    X[i] = Load32(Block + i * 4);
  }

  uint32_t a = Load32(Ctx + 0x08);
  uint32_t b = Load32(Ctx + 0x0c);
  uint32_t c = Load32(Ctx + 0x10);
  uint32_t d = Load32(Ctx + 0x14);
  const uint32_t A = a, B = b, C = c, D = d;

  MD5_STEP(MD5_F, a, b, c, d, X[0], 0xd76aa478, 7)
  MD5_STEP(MD5_F, d, a, b, c, X[1], 0xe8c7b756, 12)
  MD5_STEP(MD5_F, c, d, a, b, X[2], 0x242070db, 17)
  MD5_STEP(MD5_F, b, c, d, a, X[3], 0xc1bdceee, 22)
  MD5_STEP(MD5_F, a, b, c, d, X[4], 0xf57c0faf, 7)
  MD5_STEP(MD5_F, d, a, b, c, X[5], 0x4787c62a, 12)
  MD5_STEP(MD5_F, c, d, a, b, X[6], 0xa8304613, 17)
  MD5_STEP(MD5_F, b, c, d, a, X[7], 0xfd469501, 22)
  MD5_STEP(MD5_F, a, b, c, d, X[8], 0x698098d8, 7)
  MD5_STEP(MD5_F, d, a, b, c, X[9], 0x8b44f7af, 12)
  MD5_STEP(MD5_F, c, d, a, b, X[10], 0xffff5bb1, 17)
  MD5_STEP(MD5_F, b, c, d, a, X[11], 0x895cd7be, 22)
  MD5_STEP(MD5_F, a, b, c, d, X[12], 0x6b901122, 7)
  MD5_STEP(MD5_F, d, a, b, c, X[13], 0xfd987193, 12)
  MD5_STEP(MD5_F, c, d, a, b, X[14], 0xa679438e, 17)
  MD5_STEP(MD5_F, b, c, d, a, X[15], 0x49b40821, 22)

  MD5_STEP(MD5_G, a, b, c, d, X[1], 0xf61e2562, 5)
  MD5_STEP(MD5_G, d, a, b, c, X[6], 0xc040b340, 9)
  MD5_STEP(MD5_G, c, d, a, b, X[11], 0x265e5a51, 14)
  MD5_STEP(MD5_G, b, c, d, a, X[0], 0xe9b6c7aa, 20)
  MD5_STEP(MD5_G, a, b, c, d, X[5], 0xd62f105d, 5)
  MD5_STEP(MD5_G, d, a, b, c, X[10], 0x02441453, 9)
  MD5_STEP(MD5_G, c, d, a, b, X[15], 0xd8a1e681, 14)
  MD5_STEP(MD5_G, b, c, d, a, X[4], 0xe7d3fbc8, 20)
  MD5_STEP(MD5_G, a, b, c, d, X[9], 0x21e1cde6, 5)
  MD5_STEP(MD5_G, d, a, b, c, X[14], 0xc33707d6, 9)
  MD5_STEP(MD5_G, c, d, a, b, X[3], 0xf4d50d87, 14)
  MD5_STEP(MD5_G, b, c, d, a, X[8], 0x455a14ed, 20)
  MD5_STEP(MD5_G, a, b, c, d, X[13], 0xa9e3e905, 5)
  MD5_STEP(MD5_G, d, a, b, c, X[2], 0xfcefa3f8, 9)
  MD5_STEP(MD5_G, c, d, a, b, X[7], 0x676f02d9, 14)
  MD5_STEP(MD5_G, b, c, d, a, X[12], 0x8d2a4c8a, 20)

  MD5_STEP(MD5_H, a, b, c, d, X[5], 0xfffa3942, 4)
  MD5_STEP(MD5_H, d, a, b, c, X[8], 0x8771f681, 11)
  MD5_STEP(MD5_H, c, d, a, b, X[11], 0x6d9d6122, 16)
  MD5_STEP(MD5_H, b, c, d, a, X[14], 0xfde5380c, 23)
  MD5_STEP(MD5_H, a, b, c, d, X[1], 0xa4beea44, 4)
  MD5_STEP(MD5_H, d, a, b, c, X[4], 0x4bdecfa9, 11)
  MD5_STEP(MD5_H, c, d, a, b, X[7], 0xf6bb4b60, 16)
  MD5_STEP(MD5_H, b, c, d, a, X[10], 0xbebfbc70, 23)
  MD5_STEP(MD5_H, a, b, c, d, X[13], 0x289b7ec6, 4)
  MD5_STEP(MD5_H, d, a, b, c, X[0], 0xeaa127fa, 11)
  MD5_STEP(MD5_H, c, d, a, b, X[3], 0xd4ef3085, 16)
  MD5_STEP(MD5_H, b, c, d, a, X[6], 0x04881d05, 23)
  MD5_STEP(MD5_H, a, b, c, d, X[9], 0xd9d4d039, 4)
  MD5_STEP(MD5_H, d, a, b, c, X[12], 0xe6db99e5, 11)
  MD5_STEP(MD5_H, c, d, a, b, X[15], 0x1fa27cf8, 16)
  MD5_STEP(MD5_H, b, c, d, a, X[2], 0xc4ac5665, 23)

  MD5_STEP(MD5_I, a, b, c, d, X[0], 0xf4292244, 6)
  MD5_STEP(MD5_I, d, a, b, c, X[7], 0x432aff97, 10)
  MD5_STEP(MD5_I, c, d, a, b, X[14], 0xab9423a7, 15)
  MD5_STEP(MD5_I, b, c, d, a, X[5], 0xfc93a039, 21)
  MD5_STEP(MD5_I, a, b, c, d, X[12], 0x655b59c3, 6)
  MD5_STEP(MD5_I, d, a, b, c, X[3], 0x8f0ccc92, 10)
  MD5_STEP(MD5_I, c, d, a, b, X[10], 0xffeff47d, 15)
  MD5_STEP(MD5_I, b, c, d, a, X[1], 0x85845dd1, 21)
  MD5_STEP(MD5_I, a, b, c, d, X[8], 0x6fa87e4f, 6)
  MD5_STEP(MD5_I, d, a, b, c, X[15], 0xfe2ce6e0, 10)
  MD5_STEP(MD5_I, c, d, a, b, X[6], 0xa3014314, 15)
  MD5_STEP(MD5_I, b, c, d, a, X[13], 0x4e0811a1, 21)
  MD5_STEP(MD5_I, a, b, c, d, X[4], 0xf7537e82, 6)
  MD5_STEP(MD5_I, d, a, b, c, X[11], 0xbd3af235, 10)
  MD5_STEP(MD5_I, c, d, a, b, X[2], 0x2ad7d2bb, 15)
  MD5_STEP(MD5_I, b, c, d, a, X[9], 0xeb86d391, 21)

  Store32(Ctx + 0x08, a + A);
  Store32(Ctx + 0x0c, b + B);
  Store32(Ctx + 0x10, c + C);
  Store32(Ctx + 0x14, d + D);
}

// Called from the entry thunks in Module.S with the guest's x64 arguments (RCX, RDX, R8 = x0, x1, x2).
void FEXNativeMD5Transform(uint8_t* Ctx, const uint8_t* Block) {
  const uint64_t Start = ReadTicks();
  Transform(Ctx, Block);
  Count(&FEXNativeMD5TransformStats, Start, 64);
}

void FEXNativeMD5Update(uint8_t* Ctx, const uint8_t* Data, uint64_t Length) {
  if (Length == 0) {
    return;
  }
  const uint64_t Start = ReadTicks();
  const uint64_t Total = Length;
  do {
    const uint32_t Fill = Load32(Ctx + 0x18);
    if (Fill == 0 && Length >= 64) {
      Transform(Ctx, Data);
      Store64(Ctx, Load64(Ctx) + 0x200);
      Data += 64;
      Length -= 64;
    } else {
      // Same arithmetic as the game's: the room left is computed in 32 bits, compared against the 64-bit length.
      const uint32_t Room = 64u - Fill;
      const uint32_t Chunk = (uint64_t)Room <= Length ? Room : (uint32_t)Length;
      __builtin_memcpy(Ctx + 0x1c + (uint64_t)Fill, Data, Chunk);
      Store32(Ctx + 0x18, Load32(Ctx + 0x18) + Chunk);
      Data += Chunk;
      Length -= Chunk;
      if (Load32(Ctx + 0x18) == 64) {
        Transform(Ctx, Ctx + 0x1c);
        Store64(Ctx, Load64(Ctx) + 0x200);
        Store32(Ctx + 0x18, 0);
      }
    }
  } while (Length != 0);
  Count(&FEXNativeMD5UpdateStats, Start, Total);
}
