// SPDX-License-Identifier: MIT
// Native ARM64EC builds of the zlib 1.3.1 routines (zlib/, zlib license) that NativeRedirect.cpp runs in place of a
// game's own statically linked, emulated copies. Each entry point also counts calls, time and bytes for the stats log.

// Z_PREFIX renames every zlib symbol (inflate_fast included) to z_*, keeping them out of the way of anything else in the DLL.
#define Z_PREFIX
#include "zlib/inffast.c"
#include "zlib/adler32.c"

#include <stdint.h>

// Mirrored in NativeRedirect.cpp.
struct FEXNativeCallStats {
  uint64_t Calls;
  uint64_t Ticks; // CNTVCT_EL0 ticks spent inside the function
  uint64_t Bytes; // inflate_fast: bytes written, adler32_z: bytes summed
};

struct FEXNativeCallStats FEXNativeInflateFastStats;
struct FEXNativeCallStats FEXNativeAdler32ZStats;

static inline uint64_t ReadTicks(void) {
  uint64_t Value;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(Value));
  return Value;
}

static inline void Count(struct FEXNativeCallStats* Stats, uint64_t Start, uint64_t Bytes) {
  const uint64_t End = ReadTicks();
  __atomic_fetch_add(&Stats->Calls, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&Stats->Ticks, End - Start, __ATOMIC_RELAXED);
  __atomic_fetch_add(&Stats->Bytes, Bytes, __ATOMIC_RELAXED);
}

// Called from the entry thunks in Module.S with the guest's x64 arguments (RCX, RDX, R8 = x0, x1, x2). The return value
// becomes the guest's RAX, so 32-bit results are zero extended like an x64 32-bit register write would.
void FEXNativeInflateFast(z_streamp strm, unsigned start) {
  const uint64_t Start = ReadTicks();
  const uInt AvailOut = strm->avail_out;
  inflate_fast(strm, start);
  Count(&FEXNativeInflateFastStats, Start, AvailOut - strm->avail_out);
}

uint64_t FEXNativeAdler32Z(uint32_t adler, const uint8_t* buf, uint64_t len) {
  const uint64_t Start = ReadTicks();
  const uint32_t Result = (uint32_t)adler32_z(adler, buf, len);
  Count(&FEXNativeAdler32ZStats, Start, len);
  return Result;
}
