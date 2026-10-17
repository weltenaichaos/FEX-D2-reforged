// SPDX-License-Identifier: MIT
// Native ARM64EC builds of the zlib 1.3.1 routines (zlib/, zlib license) that NativeRedirect.cpp runs in place of a
// game's own statically linked, emulated copies. Each entry point also counts calls, time and bytes for the stats log,
// and inflate_fast notes every stream it sees start (what it is, how big, whether it repeats earlier output, and who
// asked for it).

// Z_PREFIX renames every zlib symbol (inflate_fast included) to z_*, keeping them out of the way of anything else in the DLL.
#define Z_PREFIX
#include "zlib/inffast.c"
#include "zlib/adler32.c"

#include "NativeStats.h"

#include <stdint.h>

struct FEXNativeCallStats FEXNativeInflateFastStats;
struct FEXNativeCallStats FEXNativeAdler32ZStats;

// Set once by NativeRedirect.cpp before any guest code runs.
int FEXNativeZlibTrackStreams;
uint64_t FEXNativeImageBase;
uint64_t FEXNativeTextBegin;
uint64_t FEXNativeTextEnd;

static struct FEXNativeZlibStreams Streams;
static uint32_t StreamLock;

// Fingerprints of stream starts seen so far (output buffer size + up to the first 4 KiB of output), open addressing.
#define SEEN_SLOTS (1u << 18)
#define SEEN_PROBES 32
static uint64_t Seen[SEEN_SLOTS];

static inline uint64_t ReadTicks(void) {
  uint64_t Value = 0;
#ifdef __aarch64__
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

static inline void Lock(void) {
  while (__atomic_exchange_n(&StreamLock, 1, __ATOMIC_ACQUIRE)) {
    while (__atomic_load_n(&StreamLock, __ATOMIC_RELAXED)) {
#ifdef __aarch64__
      __asm__ volatile("yield");
#endif
    }
  }
}

static inline void Unlock(void) {
  __atomic_store_n(&StreamLock, 0, __ATOMIC_RELEASE);
}

void FEXNativeZlibStreamSnapshot(struct FEXNativeZlibStreams* Out) {
  Lock();
  *Out = Streams;
  Unlock();
}

static uint64_t Fingerprint(const unsigned char* Data, uint32_t Size, uint32_t BufferSize) {
  uint64_t Hash = 0x9E3779B97F4A7C15ull ^ BufferSize;
  uint32_t i = 0;
  for (; i + 8 <= Size; i += 8) {
    uint64_t Word;
    __builtin_memcpy(&Word, Data + i, 8);
    Hash = (Hash ^ Word) * 0xff51afd7ed558ccdull;
    Hash ^= Hash >> 32;
  }
  for (; i < Size; ++i) {
    Hash = (Hash ^ Data[i]) * 0x100000001b3ull;
  }
  return Hash ? Hash : 1;
}

// Returns 1 if this fingerprint was seen before.
static int SeenBefore(uint64_t Hash) {
  uint32_t Index = (uint32_t)Hash & (SEEN_SLOTS - 1);
  for (int Probe = 0; Probe < SEEN_PROBES; ++Probe) {
    uint64_t Current = __atomic_load_n(&Seen[Index], __ATOMIC_RELAXED);
    if (Current == Hash) {
      return 1;
    }
    if (Current == 0) {
      uint64_t Expected = 0;
      if (__atomic_compare_exchange_n(&Seen[Index], &Expected, Hash, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        return 0;
      }
      if (Expected == Hash) {
        return 1;
      }
    }
    Index = (Index + 1) & (SEEN_SLOTS - 1);
  }
  return 0;
}

static inline int InText(uint64_t Value) {
  return Value >= FEXNativeTextBegin && Value < FEXNativeTextEnd;
}

// The first three values on the guest stack above inflate_fast's frame that point into the executable's code: with
// frame pointers absent these are, almost always, the return addresses of inflate() and the two functions above it.
static void FindCallers(const uint64_t* Stack, uint64_t ReturnAddress, uint32_t Out[3]) {
  uint64_t StackBase = 0;
#ifdef __aarch64__
  uint64_t Teb;
  __asm__ volatile("mov %0, x18" : "=r"(Teb));
  StackBase = *(const uint64_t*)(Teb + 8); // NT_TIB.StackBase
#endif
  // In the case where ExitFunctionEC left the x64 return address on the stack, skip it: it's inflate_fast's caller.
  if (!InText(ReturnAddress)) {
    ++Stack;
  }
  int Found = 0;
  for (int i = 0; i < 256 && Found < 3; ++i) {
    if ((uint64_t)(Stack + i + 1) > StackBase) {
      break;
    }
    const uint64_t Value = Stack[i];
    if (InText(Value)) {
      Out[Found++] = (uint32_t)(Value - FEXNativeImageBase);
    }
  }
}

static uint32_t SizeBucket(uint32_t Size) {
  uint32_t Bucket = 0;
  for (uint32_t Limit = 1024; Bucket < FEX_NATIVE_ZLIB_SIZE_BUCKETS - 1 && Size >= Limit; Limit *= 4) {
    ++Bucket;
  }
  return Bucket;
}

static void NoteStream(const unsigned char* Out, uint32_t Produced, uint32_t BufferSize, const uint64_t* Stack, uint64_t ReturnAddress) {
  uint32_t Magic;
  __builtin_memcpy(&Magic, Out, 4);
  const int Repeat = SeenBefore(Fingerprint(Out, Produced < 4096 ? Produced : 4096, BufferSize));
  uint32_t Callers[3] = {0, 0, 0};
  FindCallers(Stack, ReturnAddress, Callers);

  Lock();
  Streams.Streams++;
  Streams.Bytes += BufferSize;
  if (Repeat) {
    Streams.Repeats++;
    Streams.RepeatBytes += BufferSize;
  }
  Streams.SizeBuckets[SizeBucket(BufferSize)]++;

  struct FEXNativeZlibMagic* Slot = 0;
  for (int i = 0; i < FEX_NATIVE_ZLIB_MAGIC_SLOTS; ++i) {
    if (!Streams.Magics[i].Used) {
      Streams.Magics[i].Used = 1;
      Streams.Magics[i].Magic = Magic;
      Slot = &Streams.Magics[i];
      break;
    }
    if (Streams.Magics[i].Magic == Magic) {
      Slot = &Streams.Magics[i];
      break;
    }
  }
  if (Slot) {
    Slot->Streams++;
    Slot->Repeats += Repeat;
    Slot->Bytes += BufferSize;
  } else {
    Streams.Unlisted++;
  }

  struct FEXNativeZlibCaller* Caller = 0;
  for (int i = 0; i < FEX_NATIVE_ZLIB_CALLER_SLOTS; ++i) {
    struct FEXNativeZlibCaller* C = &Streams.Callers[i];
    if (!C->Used) {
      C->Used = 1;
      C->RVA[0] = Callers[0];
      C->RVA[1] = Callers[1];
      C->RVA[2] = Callers[2];
      Caller = C;
      break;
    }
    if (C->RVA[0] == Callers[0] && C->RVA[1] == Callers[1] && C->RVA[2] == Callers[2]) {
      Caller = C;
      break;
    }
  }
  if (Caller) {
    Caller->Streams++;
    Caller->Repeats += Repeat;
    Caller->Bytes += BufferSize;
  }
  Unlock();
}

// Called from the entry thunk in Module.S with the guest's x64 arguments (RCX, RDX = x0, x1), plus the guest stack
// pointer above the return address and that return address, for NoteStream.
void FEXNativeInflateFast(z_streamp strm, unsigned start, const uint64_t* GuestStack, uint64_t ReturnAddress) {
  const uint64_t Start = ReadTicks();
  const uInt AvailOut = strm->avail_out;
  unsigned char* const Out = strm->next_out;
  // inflate() passes the avail_out it was entered with as start, and only updates total_out when it returns: so this is
  // the first output of a stream if neither inflate() nor an earlier call in this stream has written anything yet.
  const int StreamStart = FEXNativeZlibTrackStreams && strm->total_out == 0 && start == AvailOut;
  inflate_fast(strm, start);
  const uInt Produced = AvailOut - strm->avail_out;
  if (StreamStart && Produced >= 4) {
    NoteStream(Out, Produced, start, GuestStack, ReturnAddress);
  }
  Count(&FEXNativeInflateFastStats, Start, Produced);
}

// The return value becomes the guest's RAX, so the 32-bit result is zero extended like an x64 32-bit register write.
uint64_t FEXNativeAdler32Z(uint32_t adler, const uint8_t* buf, uint64_t len) {
  const uint64_t Start = ReadTicks();
  const uint32_t Result = (uint32_t)adler32_z(adler, buf, len);
  Count(&FEXNativeAdler32ZStats, Start, len);
  return Result;
}
