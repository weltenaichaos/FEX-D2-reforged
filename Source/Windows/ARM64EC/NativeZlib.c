// SPDX-License-Identifier: MIT
// Native ARM64EC builds of the zlib 1.3.1 routines (zlib/, zlib license) that NativeRedirect.cpp runs in place of a
// game's own statically linked, emulated copies. Each entry point also counts calls, time and bytes for the stats log,
// and inflate_fast notes every stream it sees start (what it is, how big, whether it repeats earlier output, and who
// asked for it).
//
// With the unpack cache on (NativeUnpackCache.h), whole streams are also served from disk: see the comment above
// FEXNativeInflateFast.

// Z_PREFIX renames every zlib symbol (inflate_fast included) to z_*, keeping them out of the way of anything else in the DLL.
#define Z_PREFIX
#include "zlib/inffast.c"
#include "zlib/adler32.c"

#include "NativeStats.h"
#include "NativeUnpackCache.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

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

static inline void Lock(void) {
  while (__atomic_exchange_n(&StreamLock, 1, __ATOMIC_ACQUIRE)) {
    while (__atomic_load_n(&StreamLock, __ATOMIC_RELAXED)) {
#ifdef FEX_NATIVE_ARM64
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
  // Only read what is surely mapped: up to the thread's stack base when on the thread's stack, otherwise (a stack the
  // game switched to itself) only to the end of the current page.
  uint64_t End = ((uint64_t)Stack | 0xfff) + 1;
#ifdef FEX_NATIVE_ARM64
  uint64_t Teb;
  __asm__ volatile("mov %0, x18" : "=r"(Teb));
  const uint64_t StackBase = *(const uint64_t*)(Teb + 8);   // NT_TIB.StackBase
  const uint64_t StackLimit = *(const uint64_t*)(Teb + 16); // NT_TIB.StackLimit
  if ((uint64_t)Stack >= StackLimit && (uint64_t)Stack < StackBase) {
    End = StackBase;
  }
#endif
  // In the case where ExitFunctionEC left the x64 return address on the stack, skip it: it's inflate_fast's caller.
  if (!InText(ReturnAddress)) {
    ++Stack;
  }
  int Found = 0;
  for (int i = 0; i < 256 && Found < 3; ++i) {
    if ((uint64_t)(Stack + i + 1) > End) {
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

static int Printable(uint32_t Magic) {
  for (int i = 0; i < 4; ++i) {
    const uint32_t c = (Magic >> (i * 8)) & 0xff;
    if (c < 0x20 || c >= 0x7f) {
      return 0;
    }
  }
  return 1;
}

static void NoteStream(const unsigned char* Out, uint32_t Produced, uint32_t BufferSize, const uint64_t* Stack, uint64_t ReturnAddress) {
  uint32_t Magic;
  __builtin_memcpy(&Magic, Out, 4);
  if (!Printable(Magic)) {
    Magic = 0;
  }
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
  if (Produced + 1024 >= BufferSize) {
    Streams.Oneshot++;
  }

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

// Unpack cache.
//
// D2R inflates each chunk of game data with a single inflate(Z_FINISH) call that has the whole compressed chunk as
// input and the whole output buffer. inflate() reads the zlib header and the first block's header, then calls
// inflate_fast for the bulk of each block, and when the last block is done it checks the adler32 of everything it
// wrote against the stream's 4-byte trailer (state CHECK) and returns Z_STREAM_END.
//
// Miss: the first inflate_fast call of a fresh stream remembers the stream (Pending). When inflate() later asks for the
// adler32 of the whole output, that value matching the last 4 input bytes proves the stream was decoded completely,
// that it ends exactly at the end of its input, and that the output is right; the output is then stored.
//
// Hit: the first inflate_fast call of a fresh stream finds the same input (hashed whole, together with everything else
// the decoding depends on) in the cache. It reads the stored output into the game's buffer, moves the input to the
// trailer and leaves the state at CHECK, so inflate() goes straight to verifying the trailer against the adler32 of the
// stored output and finishes the stream normally, skipping every remaining block.

#define UNPACK_CACHE_MIN_BYTES (16u * 1024u)
#define UNPACK_CACHE_KEY_VERSION 1u

struct KeySeed {
  uint32_t Version;
  uint32_t AvailIn;
  uint32_t AvailOut;
  uint32_t Hold;
  uint32_t Bits;
  uint32_t LenBits;
  uint32_t DistBits;
  uint32_t Wrap;
  uint64_t TablesHash;
};

static inline uint64_t CurrentThreadId(void) {
#ifdef FEX_NATIVE_ARM64
  uint64_t Teb;
  __asm__ volatile("mov %0, x18" : "=r"(Teb));
  return *(const uint64_t*)(Teb + 0x48); // TEB.ClientId.UniqueThread
#else
  return 0;
#endif
}

// A fresh zlib stream (not gzip, not raw deflate) with its check on, nothing written yet and an empty window, so the
// first call's decoding depends only on the state and the input.
static int CacheableStart(z_streamp strm, unsigned start, const struct inflate_state* state) {
  return strm->total_out == 0 && start == strm->avail_out && state->strm == strm && state->mode == LEN && state->wrap == 5 &&
         state->flags == 0 && state->whave == 0 && state->sane && state->bits < 8 && strm->avail_in >= 16 &&
         strm->avail_out >= UNPACK_CACHE_MIN_BYTES;
}

// The key covers everything decoding the rest of the stream depends on: the decoding tables built from the first
// block's header, the bits already taken from the input, the buffer sizes, and every input byte.
static int CacheKey(z_streamp strm, const struct inflate_state* state, uint64_t* Seed, uint64_t Key[2]) {
  uint64_t TablesHash;
  if (state->lencode == state->codes) {
    // Dynamic block: both tables (with their sub-tables) are in codes[], up to next.
    if (state->next < state->codes || state->next > state->codes + ENOUGH || state->distcode < state->codes ||
        state->distcode >= state->next) {
      return 0;
    }
    TablesHash = XXH3_64bits(state->codes, (size_t)(state->next - state->codes) * sizeof(code));
  } else {
    // Fixed block: the game's static tables, which have no sub-tables.
    if (state->lenbits > 10 || state->distbits > 10) {
      return 0;
    }
    TablesHash = XXH3_64bits_withSeed(state->distcode, ((size_t)1 << state->distbits) * sizeof(code),
                                      XXH3_64bits(state->lencode, ((size_t)1 << state->lenbits) * sizeof(code)));
  }

  struct KeySeed S;
  __builtin_memset(&S, 0, sizeof(S));
  S.Version = UNPACK_CACHE_KEY_VERSION;
  S.AvailIn = strm->avail_in;
  S.AvailOut = strm->avail_out;
  S.Hold = (uint32_t)state->hold;
  S.Bits = state->bits;
  S.LenBits = state->lenbits;
  S.DistBits = state->distbits;
  S.Wrap = (uint32_t)state->wrap;
  S.TablesHash = TablesHash;
  *Seed = XXH3_64bits(&S, sizeof(S));

  const XXH128_hash_t Hash = XXH3_128bits_withSeed(strm->next_in, strm->avail_in, *Seed);
  Key[0] = Hash.low64;
  Key[1] = Hash.high64;
  return 1;
}

static int ApplyCacheHit(z_streamp strm, struct inflate_state* state, const uint64_t Key[2]) {
  uint32_t Produced, Trailer;
  if (!FEXUnpackCacheGet(Key, strm->next_out, strm->avail_out, &Produced, &Trailer)) {
    return 0;
  }
  if (Produced > strm->avail_out || (uint64_t)Trailer + 4 > strm->avail_in) {
    return 0;
  }
  strm->next_in += Trailer;
  strm->avail_in -= Trailer;
  strm->next_out += Produced;
  strm->avail_out -= Produced;
  state->hold = 0;
  state->bits = 0;
  state->last = 1;
  state->mode = CHECK;
  return 1;
}

// Streams that missed, waiting for inflate() to check their output.
#define PENDING_SLOTS 64

struct PendingStream {
  const unsigned char* Out;
  const unsigned char* In;
  uint32_t AvailIn;
  uint32_t AvailOut;
  uint64_t Thread;
  uint64_t Seed;
  uint64_t Key[2];
};

static struct PendingStream Pending[PENDING_SLOTS];
static uint32_t PendingNext;
static uint32_t PendingLock;

static inline void PendingAcquire(void) {
  while (__atomic_exchange_n(&PendingLock, 1, __ATOMIC_ACQUIRE)) {
    while (__atomic_load_n(&PendingLock, __ATOMIC_RELAXED)) {
#ifdef FEX_NATIVE_ARM64
      __asm__ volatile("yield");
#endif
    }
  }
}

static inline void PendingRelease(void) {
  __atomic_store_n(&PendingLock, 0, __ATOMIC_RELEASE);
}

static void AddPending(const struct PendingStream* Stream) {
  PendingAcquire();
  uint32_t Slot = PendingNext;
  for (uint32_t i = 0; i < PENDING_SLOTS; ++i) {
    if (Pending[i].Out == Stream->Out && Pending[i].Thread == Stream->Thread) {
      Slot = i; // the same buffer started a new stream: the old one never finished
      break;
    }
  }
  if (Slot == PendingNext) {
    PendingNext = (PendingNext + 1) % PENDING_SLOTS;
  }
  Pending[Slot] = *Stream;
  PendingRelease();
}

static int TakePending(const unsigned char* Out, uint64_t Thread, struct PendingStream* Stream) {
  int Found = 0;
  PendingAcquire();
  for (uint32_t i = 0; i < PENDING_SLOTS; ++i) {
    if (Pending[i].Out == Out && Pending[i].Thread == Thread) {
      *Stream = Pending[i];
      Pending[i].Out = 0;
      Found = 1;
      break;
    }
  }
  PendingRelease();
  return Found;
}

// inflate() asked for the adler32 of len bytes at buf, starting from the initial value 1, and got Result.
static void CompletePending(const unsigned char* buf, uint64_t len, uint32_t Result) {
  struct PendingStream P;
  if (!TakePending(buf, CurrentThreadId(), &P)) {
    return;
  }
  if (len > P.AvailOut || P.AvailIn < 16) {
    return;
  }
  const unsigned char* T = P.In + P.AvailIn - 4;
  const uint32_t Trailer = ((uint32_t)T[0] << 24) | ((uint32_t)T[1] << 16) | ((uint32_t)T[2] << 8) | (uint32_t)T[3];
  if (Trailer != Result) {
    return; // not the whole stream, or more input follows the stream
  }
  // The input is still what the key was made from.
  const XXH128_hash_t Hash = XXH3_128bits_withSeed(P.In, P.AvailIn, P.Seed);
  if (Hash.low64 != P.Key[0] || Hash.high64 != P.Key[1]) {
    return;
  }
  FEXUnpackCachePut(P.Key, buf, (uint32_t)len, P.AvailIn - 4);
}

// Called from the entry thunk in Module.S with the guest's x64 arguments (RCX, RDX = x0, x1), plus the guest stack
// pointer above the return address and that return address, for NoteStream.
void FEXNativeInflateFast(z_streamp strm, unsigned start, const uint64_t* GuestStack, uint64_t ReturnAddress) {
  const uint64_t Start = ReadTicks();
  const uInt AvailOut = strm->avail_out;
  unsigned char* const Out = strm->next_out;
  // inflate() passes the avail_out it was entered with as start, and only updates total_out when it returns: so this is
  // the first output of a stream if neither inflate() nor an earlier call in this stream has written anything yet.
  const int Fresh = strm->total_out == 0 && start == AvailOut;
  const int StreamStart = FEXNativeZlibTrackStreams && Fresh;

  int Hit = 0;
  if (FEXUnpackCacheEnabled && Fresh) {
    struct inflate_state* state = (struct inflate_state*)strm->state;
    uint64_t Seed, Key[2];
    if (CacheableStart(strm, start, state) && CacheKey(strm, state, &Seed, Key)) {
      __atomic_fetch_add(&FEXUnpackCacheStats.KeyTicks, ReadTicks() - Start, __ATOMIC_RELAXED);
      Hit = ApplyCacheHit(strm, state, Key);
      if (!Hit) {
        const struct PendingStream Stream = {
          .Out = Out,
          .In = strm->next_in,
          .AvailIn = strm->avail_in,
          .AvailOut = AvailOut,
          .Thread = CurrentThreadId(),
          .Seed = Seed,
          .Key = {Key[0], Key[1]},
        };
        AddPending(&Stream);
      }
    }
  }

  if (!Hit) {
    inflate_fast(strm, start);
  }
  const uInt Produced = AvailOut - strm->avail_out;
  if (StreamStart && Produced >= 4) {
    NoteStream(Out, Produced, start, GuestStack, ReturnAddress);
  }
  if (!Hit) {
    Count(&FEXNativeInflateFastStats, Start, Produced);
  }
}

// The return value becomes the guest's RAX, so the 32-bit result is zero extended like an x64 32-bit register write.
uint64_t FEXNativeAdler32Z(uint32_t adler, const uint8_t* buf, uint64_t len) {
  const uint64_t Start = ReadTicks();
  const uint32_t Result = (uint32_t)adler32_z(adler, buf, len);
  Count(&FEXNativeAdler32ZStats, Start, len);
  if (FEXUnpackCacheEnabled && adler == 1 && buf && len >= UNPACK_CACHE_MIN_BYTES && len <= 0xffffffffu) {
    CompletePending(buf, len, Result);
  }
  return Result;
}
