// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Runs ntdll's wait lists (RtlWaitOnAddress, RtlWakeAddress*) with upstream Wine's race fix (see WineWaitFix.h)
$end_info$
*/

#include "WineWaitFix.h"
#include "Windows/Common/StatsLog.h"
#include "Windows/Common/Switches.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <ntstatus.h>
#include <windef.h>
#include <winternl.h>
#include <winnt.h>
#include <winbase.h>

extern "C" {
extern uintptr_t NtDllBase;           // Module.cpp
extern uint32_t* NtDllRedirectionLUT; // Module.cpp: an export's x64 fast forward sequence RVA -> its native code RVA
extern uint32_t NtDllRedirectionLUTSize;
NTSTATUS NtProtectVirtualMemoryNative(HANDLE, PVOID*, SIZE_T*, ULONG, ULONG*); // Module.S, skips FEX's own notifications
// ntdll imports (Defs/ntdll.def). Called by name, never through a pointer to their native code: FEX's call checker
// (CheckCall, Module.S) maps every address inside ntdll's fast forward range, native code included, through its table.
NTSTATUS WINAPI NtWaitForAlertByThreadId(const void* Address, const LARGE_INTEGER* Timeout);
NTSTATUS WINAPI NtAlertThreadByThreadId(HANDLE ThreadId);
}

namespace WineWaitFix {
namespace {
  enum class Mode : uint8_t { Off, Kernel, Spin };
  Mode Active {Mode::Off};
  bool Counting {};
  fextl::string Outcome; // the note's text, kept until the stats log takes notes

  // One waiting thread, on its stack while it waits (as in Wine).
  struct Entry {
    Entry* Next;
    Entry* Prev;
    const void* Addr; // cleared (release) by the waker once it has unlinked the entry and read Tid; null in a list head
    DWORD Tid;
  };

  struct alignas(64) Queue {
    Entry Head;                  // Next/Prev point to Head while empty (set in Init)
    std::atomic<uint32_t> Lock;  // 0 free, 1 held, 2 held with sleepers (Kernel mode); 0/1 (Spin mode)
  };
  constexpr size_t QueueCount = 1024;
  Queue Queues[QueueCount];

  Queue& QueueFor(const void* Addr) {
    return Queues[(reinterpret_cast<uintptr_t>(Addr) >> 4) % QueueCount];
  }

  struct Counters {
    std::atomic<uint64_t> Waits;      // RtlWaitOnAddress calls
    std::atomic<uint64_t> Changed;    // of them, the value had changed already (no sleep)
    std::atomic<uint64_t> TimedOut;   // slept and timed out
    std::atomic<uint64_t> Early;      // came back alerted with the entry still queued (an alert left over from before):
                                      // in Wine's 11.0 code such a return racing a waker is what broke a list
    std::atomic<uint64_t> Wakes;      // RtlWakeAddressSingle + RtlWakeAddressAll calls
    std::atomic<uint64_t> Woken;      // threads they took off a list and alerted
    std::atomic<uint64_t> LockSleeps; // list lock waits that went to sleep in the kernel
  };
  Counters Stats {};

  void Count(std::atomic<uint64_t>& Counter, uint64_t N = 1) {
    if (Counting) {
      Counter.fetch_add(N, std::memory_order_relaxed);
    }
  }

  // Linux arm64 futex call, straight to the kernel (as KernelFutexCall in WritePriorityMutex.h).
  int64_t Futex(void* Address, int64_t Op, uint32_t Value) {
    register int64_t X8 asm("x8") = 98; // __NR_futex
    register int64_t X0 asm("x0") = reinterpret_cast<int64_t>(Address);
    register int64_t X1 asm("x1") = Op;
    register int64_t X2 asm("x2") = Value;
    register int64_t X3 asm("x3") = 0;
    register int64_t X4 asm("x4") = 0;
    register int64_t X5 asm("x5") = 0;
    asm volatile("svc #0" : "+r"(X0) : "r"(X8), "r"(X1), "r"(X2), "r"(X3), "r"(X4), "r"(X5) : "memory", "cc");
    return X0;
  }
  constexpr int64_t FutexWaitPrivate = 0 | 128;
  constexpr int64_t FutexWakePrivate = 1 | 128;
  constexpr int SpinRounds = 64; // ~a few microseconds of yields before sleeping

  void Lock(Queue& Q) {
    uint32_t Current = 0;
    if (Q.Lock.compare_exchange_strong(Current, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
      return;
    }
    if (Active == Mode::Spin) {
      // Wine's spin lock: yield until free.
      do {
        asm volatile("yield" ::: "memory");
        Current = 0;
      } while (!Q.Lock.compare_exchange_weak(Current, 1, std::memory_order_acquire, std::memory_order_relaxed));
      return;
    }
    for (int i = 0; i < SpinRounds; ++i) {
      asm volatile("yield" ::: "memory");
      Current = Q.Lock.load(std::memory_order_relaxed);
      if (Current == 0 && Q.Lock.compare_exchange_weak(Current, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
        return;
      }
    }
    // U. Drepper, "Futexes Are Tricky", mutex 3.
    if (Current != 2) {
      Current = Q.Lock.exchange(2, std::memory_order_acquire);
    }
    if (Current != 0) {
      Count(Stats.LockSleeps);
    }
    while (Current != 0) {
      Futex(&Q.Lock, FutexWaitPrivate, 2);
      Current = Q.Lock.exchange(2, std::memory_order_acquire);
    }
  }

  void Unlock(Queue& Q) {
    if (Q.Lock.exchange(0, std::memory_order_release) == 2) {
      Futex(&Q.Lock, FutexWakePrivate, 1);
    }
  }

  void AddTail(Entry* Head, Entry* E) {
    E->Next = Head;
    E->Prev = Head->Prev;
    Head->Prev->Next = E;
    Head->Prev = E;
  }

  void Unlink(Entry* E) {
    E->Next->Prev = E->Prev;
    E->Prev->Next = E->Next;
  }

  bool SameValue(const void* Addr, const void* Cmp, SIZE_T Size) {
    switch (Size) {
    case 1: return *static_cast<const volatile uint8_t*>(Addr) == *static_cast<const uint8_t*>(Cmp);
    case 2: return *static_cast<const volatile uint16_t*>(Addr) == *static_cast<const uint16_t*>(Cmp);
    case 4: return *static_cast<const volatile uint32_t*>(Addr) == *static_cast<const uint32_t*>(Cmp);
    case 8: return *static_cast<const volatile uint64_t*>(Addr) == *static_cast<const uint64_t*>(Cmp);
    }
    return false;
  }

  // The replacements. They run on any thread, from ntdll's own callers (SRW locks, condition variables, kernelbase's
  // WaitOnAddress) and from x64 code; they take no lock but a list's and call nothing that waits but the alert wait.
  NTSTATUS FixedWaitOnAddress(const void* Addr, const void* Cmp, SIZE_T Size, const LARGE_INTEGER* Timeout) {
    if (Size != 1 && Size != 2 && Size != 4 && Size != 8) {
      return STATUS_INVALID_PARAMETER;
    }
    Count(Stats.Waits);
    Queue& Q = QueueFor(Addr);
    Entry E;
    E.Addr = Addr;
    E.Tid = GetCurrentThreadId();

    Lock(Q);
    // Compared under the lock, so a wake after the change can't be missed (as in Wine).
    if (!SameValue(Addr, Cmp, Size)) {
      Unlock(Q);
      Count(Stats.Changed);
      return STATUS_SUCCESS;
    }
    AddTail(&Q.Head, &E);
    Unlock(Q);

    NTSTATUS Status = NtWaitForAlertByThreadId(nullptr, Timeout);

    // A waker unlinks the entry and reads its thread id before it clears Addr (release order), so once Addr reads null
    // here (acquire order) nobody touches the entry again and this stack can be reused. Otherwise take it off the list
    // under the lock (a timeout, or an alert left over from an earlier wake).
    if (__atomic_load_n(&E.Addr, __ATOMIC_ACQUIRE)) {
      Lock(Q);
      if (E.Addr) {
        Unlink(&E);
        if (Status == STATUS_ALERTED) {
          Count(Stats.Early);
        }
      }
      Unlock(Q);
    }
    if (Status == STATUS_TIMEOUT) {
      Count(Stats.TimedOut);
    }
    return Status == STATUS_ALERTED ? STATUS_SUCCESS : Status;
  }

  void FixedWakeAddressSingle(const void* Addr) {
    if (!Addr) {
      return;
    }
    Count(Stats.Wakes);
    Queue& Q = QueueFor(Addr);
    DWORD Tid = 0;
    Lock(Q);
    for (Entry* E = Q.Head.Next; E != &Q.Head; E = E->Next) {
      if (E->Addr == Addr) {
        // Taken off the list so that a second wake at the same time wakes a second waiter (as in Wine).
        Tid = E->Tid;
        Unlink(E);
        __atomic_store_n(&E->Addr, static_cast<const void*>(nullptr), __ATOMIC_RELEASE);
        break;
      }
    }
    Unlock(Q);
    if (Tid) {
      Count(Stats.Woken);
      NtAlertThreadByThreadId(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(Tid)));
    }
  }

  void FixedWakeAddressAll(const void* Addr) {
    if (!Addr) {
      return;
    }
    Count(Stats.Wakes);
    Queue& Q = QueueFor(Addr);
    DWORD Tids[256];
    size_t Waiting = 0;
    uint64_t All = 0;
    Lock(Q);
    for (Entry *E = Q.Head.Next, *Next; E != &Q.Head; E = Next) {
      Next = E->Next;
      if (E->Addr == Addr) {
        const DWORD Tid = E->Tid;
        Unlink(E);
        __atomic_store_n(&E->Addr, static_cast<const void*>(nullptr), __ATOMIC_RELEASE);
        if (Waiting == std::size(Tids)) {
          // Wine also alerts a full batch while holding the list.
          for (size_t i = 0; i < Waiting; ++i) {
            NtAlertThreadByThreadId(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(Tids[i])));
          }
          Waiting = 0;
        }
        Tids[Waiting++] = Tid;
        ++All;
      }
    }
    Unlock(Q);
    for (size_t i = 0; i < Waiting; ++i) {
      NtAlertThreadByThreadId(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(Tids[i])));
    }
    Count(Stats.Woken, All);
  }

  // Seconds in a row each list's lock was seen held at the once-a-second look (held for microseconds at most normally).
  uint8_t HeldSeconds[QueueCount] {};
  bool HeldNoted[QueueCount] {};

  fextl::string StatsLine() {
    // A lock seen held at five looks in a row means a thread is stuck inside a list (the build 139 freeze in Wine's own
    // lists): say so once per list. Only the lock word is read, never the list.
    for (size_t i = 0; i < QueueCount; ++i) {
      if (Queues[i].Lock.load(std::memory_order_relaxed)) {
        if (HeldSeconds[i] < 255) {
          ++HeldSeconds[i];
        }
        if (HeldSeconds[i] >= 5 && !HeldNoted[i]) {
          HeldNoted[i] = true;
          FEX::Windows::StatsLogNote(fextl::fmt::format("wine waits: list {}'s lock was held at {} looks in a row a second apart (lock word {}): "
                                                        "a thread is stuck inside FEX's wait list code",
                                                        i, HeldSeconds[i], Queues[i].Lock.load(std::memory_order_relaxed)));
        }
      } else {
        HeldSeconds[i] = 0;
      }
    }
    const uint64_t Waits = Stats.Waits.exchange(0, std::memory_order_relaxed);
    const uint64_t Changed = Stats.Changed.exchange(0, std::memory_order_relaxed);
    const uint64_t TimedOut = Stats.TimedOut.exchange(0, std::memory_order_relaxed);
    const uint64_t Early = Stats.Early.exchange(0, std::memory_order_relaxed);
    const uint64_t Wakes = Stats.Wakes.exchange(0, std::memory_order_relaxed);
    const uint64_t Woken = Stats.Woken.exchange(0, std::memory_order_relaxed);
    const uint64_t LockSleeps = Stats.LockSleeps.exchange(0, std::memory_order_relaxed);
    if (!Waits && !Wakes) {
      return {};
    }
    return fextl::fmt::format("  wine waits (FEX's fixed lists): waits {} (value changed already {}, timed out {}, back early with an old "
                              "alert {}) | wakes {} woke {} threads | list lock sleeps {}\n",
                              Waits, Changed, TimedOut, Early, Wakes, Woken, LockSleeps);
  }

  // ---- Checking ntdll's code before replacing it ----

  struct Section {
    uint64_t Begin, End;
    uint32_t Flags;
  };
  constexpr size_t MaxSections = 32;
  Section Sections[MaxSections];
  size_t SectionCount {};

  const Section* SectionAt(uint64_t Address) {
    for (size_t i = 0; i < SectionCount; ++i) {
      if (Address >= Sections[i].Begin && Address < Sections[i].End) {
        return &Sections[i];
      }
    }
    return nullptr;
  }

  // The native code of an ntdll export: exports point to x64 fast forward sequences, the redirection table to the code.
  // An export without one is taken as it is (the function table check below tells whether native code starts there).
  uint64_t NativeExport(HMODULE NtDll, const char* Name) {
    const uint64_t Export = reinterpret_cast<uint64_t>(GetProcAddress(NtDll, Name));
    if (!Export || Export < NtDllBase) {
      return 0;
    }
    const uint64_t RVA = Export - NtDllBase;
    if (NtDllRedirectionLUT && RVA < NtDllRedirectionLUTSize && NtDllRedirectionLUT[RVA]) {
      return NtDllBase + NtDllRedirectionLUT[RVA];
    }
    return Export;
  }

  // ARM64 function table entry containing Address: its start and length, from the packed or full unwind data. Entries is
  // the table's size (0 without one). In an ARM64EC process ntdll is seen through its x64 view: the exception directory
  // then holds the x64 code's table (12-byte entries), and the ARM64/ARM64EC code's 8-byte entries are the ARM64EC
  // metadata's extra table (checked on stock Proton 11.0-1's ntdll: 0x8bd0 bytes there, the directory 0xc48).
  bool FunctionAt(uint64_t Address, uint64_t& Begin, uint64_t& Length, size_t& Entries) {
    ULONG Size {};
    const uint32_t* Table = nullptr;
    ULONG ConfigSize {};
    const auto* LoadConfig = static_cast<const IMAGE_LOAD_CONFIG_DIRECTORY64*>(
      RtlImageDirectoryEntryToData(reinterpret_cast<HMODULE>(NtDllBase), TRUE, IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG, &ConfigSize));
    constexpr size_t CHPEFieldEnd =
      offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, CHPEMetadataPointer) + sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64::CHPEMetadataPointer);
    if (LoadConfig && ConfigSize >= CHPEFieldEnd && LoadConfig->Size >= CHPEFieldEnd && LoadConfig->CHPEMetadataPointer) {
      const auto* Metadata = reinterpret_cast<const IMAGE_ARM64EC_METADATA*>(LoadConfig->CHPEMetadataPointer);
      if (Metadata->Version >= 2 && Metadata->ExtraRFETable && Metadata->ExtraRFETableSize) {
        Table = reinterpret_cast<const uint32_t*>(NtDllBase + Metadata->ExtraRFETable);
        Size = Metadata->ExtraRFETableSize;
      }
    } else {
      Table = static_cast<const uint32_t*>(
        RtlImageDirectoryEntryToData(reinterpret_cast<HMODULE>(NtDllBase), TRUE, IMAGE_DIRECTORY_ENTRY_EXCEPTION, &Size));
    }
    Entries = Table ? Size / 8 : 0;
    if (!Table || Size < 8) {
      return false;
    }
    const size_t Count = Size / 8;
    for (size_t i = 0; i < Count; ++i) {
      const uint64_t Start = NtDllBase + Table[i * 2];
      const uint32_t Unwind = Table[i * 2 + 1];
      uint64_t Len;
      if (Unwind & 3) {
        Len = static_cast<uint64_t>((Unwind >> 2) & 0x7ff) * 4;
      } else {
        const uint64_t XData = NtDllBase + Unwind;
        const Section* S = SectionAt(XData);
        if (!S || XData + 4 > S->End) {
          continue;
        }
        Len = static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(XData) & 0x3ffff) * 4;
      }
      if (Address >= Start && Address < Start + Len) {
        Begin = Start;
        Length = Len;
        return true;
      }
    }
    return false;
  }

  int64_t SignExtend(uint64_t Value, int Bits) {
    const uint64_t Sign = 1ull << (Bits - 1);
    return static_cast<int64_t>((Value ^ Sign) - Sign);
  }

  // The address an ADR, or an ADRP and the first ADD/LDR/STR (unsigned immediate) within Window instructions after it
  // based on its register, forms. 0 when the instruction at Code isn't one; just the page, with Resolved false, when
  // nothing after an ADRP uses its register within Window.
  uint64_t FormedAddress(const uint32_t* Code, const uint32_t* End, bool& Resolved, int Window = 8) {
    const uint32_t I = Code[0];
    const uint64_t PC = reinterpret_cast<uint64_t>(Code);
    const uint64_t Imm21 = (static_cast<uint64_t>((I >> 5) & 0x7ffff) << 2) | ((I >> 29) & 3);
    Resolved = true;
    if ((I & 0x9f000000) == 0x10000000) { // ADR
      return PC + SignExtend(Imm21, 21);
    }
    if ((I & 0x9f000000) != 0x90000000) { // ADRP
      return 0;
    }
    const uint64_t Page = (PC & ~0xfffull) + (SignExtend(Imm21, 21) << 12);
    const uint32_t Rd = I & 31;
    for (const uint32_t* Next = Code + 1; Next < End && Next <= Code + Window; ++Next) {
      const uint32_t J = *Next;
      if ((J & 0xff800000) == 0x91000000 && ((J >> 5) & 31) == Rd) { // ADD Xd, Xn, #imm{, lsl #12}
        return Page + (static_cast<uint64_t>((J >> 10) & 0xfff) << (((J >> 22) & 1) ? 12 : 0));
      }
      if ((J & 0x3b000000) == 0x39000000 && ((J >> 5) & 31) == Rd) { // LDR/STR (unsigned immediate)
        return Page + (static_cast<uint64_t>((J >> 10) & 0xfff) << (J >> 30));
      }
    }
    Resolved = false;
    return Page;
  }

  // Addresses a function forms in full (ADR, or ADRP + ADD/LDR/STR), sorted, into Out; returns how many (at most Max).
  size_t FormedIn(uint64_t Begin, uint64_t Length, uint64_t* Out, size_t Max) {
    size_t N = 0;
    const auto* Code = reinterpret_cast<const uint32_t*>(Begin);
    const auto* End = reinterpret_cast<const uint32_t*>(Begin + Length);
    for (const uint32_t* C = Code; C < End && N < Max; ++C) {
      bool Resolved {};
      const uint64_t Formed = FormedAddress(C, End, Resolved);
      if (Formed && Resolved && std::find(Out, Out + N, Formed) == Out + N) {
        Out[N++] = Formed;
      }
    }
    std::sort(Out, Out + N);
    return N;
  }

  // Wine's struct futex_queue { struct list queue; LONG lock; }: 24 bytes, 256 of them.
  constexpr uint64_t WineQueueSize = 24;
  constexpr uint64_t WineQueueCount = 256;
  constexpr uint64_t WineQueuesBytes = WineQueueSize * WineQueueCount;

  // All 256 of Wine's lists at Base look unused or empty (both links 0 or both pointing to the list itself, lock 0).
  bool WineQueuesEmpty(uint64_t Base) {
    if (Base & 7) {
      return false;
    }
    const Section* S = SectionAt(Base);
    if (!S || Base + WineQueuesBytes > S->End || (S->Flags & IMAGE_SCN_MEM_EXECUTE) || !(S->Flags & IMAGE_SCN_MEM_WRITE)) {
      return false;
    }
    for (uint64_t i = 0; i < WineQueueCount; ++i) {
      const uint64_t Q = Base + i * WineQueueSize;
      const uint64_t Next = *reinterpret_cast<const uint64_t*>(Q);
      const uint64_t Prev = *reinterpret_cast<const uint64_t*>(Q + 8);
      const uint32_t LockWord = *reinterpret_cast<const uint32_t*>(Q + 16);
      if (LockWord || !((Next == 0 && Prev == 0) || (Next == Q && Prev == Q))) {
        return false;
      }
    }
    return true;
  }

  struct Target {
    const char* Name;
    void* Replacement;
    uint64_t Code;  // native code
    uint64_t Begin; // function table entry
    uint64_t Length;
    uint32_t Uses;  // uses of Wine's lists inside it
  };

  // Acquire loads and release stores (LDAR, LDAPR, LDAPUR, STLR, STLUR, any size) in a function. Stock Proton 11.0's three
  // functions have none (their list lock uses exclusive or LSE atomics, other encodings); upstream Wine's fix reads and
  // clears the woken mark with them, so a Proton that has the fix (our own build) is left alone.
  uint32_t OrderedAccesses(const Target& T) {
    uint32_t Count = 0;
    const auto* C = reinterpret_cast<const uint32_t*>(T.Begin);
    for (uint64_t i = 0; i < T.Length / 4; ++i) {
      const uint32_t I = C[i];
      if ((I & 0x3ffffc00) == 0x08dffc00 || (I & 0x3ffffc00) == 0x089ffc00 || (I & 0x3ffffc00) == 0x38bfc000 ||
          (I & 0x3f200c00) == 0x19000000) {
        ++Count;
      }
    }
    return Count;
  }

  // FNV-1a over the three functions' code, for the note: names the exact ntdll build the checks ran on.
  uint64_t CodeHash {};

  // ldr x16, #8; br x16; .quad To: x16 is a scratch register at a function's entry.
  void MakeStub(uint32_t (&Stub)[4], uint64_t To) {
    Stub[0] = 0x58000050;
    Stub[1] = 0xd61f0200;
    Stub[2] = static_cast<uint32_t>(To);
    Stub[3] = static_cast<uint32_t>(To >> 32);
  }

  // All three entries are made writable first and only then written, so either all of them switch or none does (a half
  // switch would split waiters and wakers between two sets of lists).
  template<size_t N>
  fextl::string PatchAll(const uint64_t (&Code)[N], const uint64_t (&To)[N]) {
    ULONG Old[N] {};
    size_t Writable = 0;
    for (; Writable < N; ++Writable) {
      void* ProtAddress = reinterpret_cast<void*>(Code[Writable]);
      SIZE_T ProtSize = 16;
      if (NtProtectVirtualMemoryNative(NtCurrentProcess(), &ProtAddress, &ProtSize, PAGE_EXECUTE_READWRITE, &Old[Writable])) {
        break;
      }
    }
    if (Writable == N) {
      for (size_t i = 0; i < N; ++i) {
        uint32_t Stub[4];
        MakeStub(Stub, To[i]);
        memcpy(reinterpret_cast<void*>(Code[i]), Stub, sizeof(Stub));
      }
    }
    // Put the protections back in reverse, so a page shared by two entries ends as it was.
    for (size_t i = Writable; i-- > 0;) {
      void* ProtAddress = reinterpret_cast<void*>(Code[i]);
      SIZE_T ProtSize = 16;
      ULONG Ignored {};
      NtProtectVirtualMemoryNative(NtCurrentProcess(), &ProtAddress, &ProtSize, Old[i], &Ignored);
    }
    if (Writable != N) {
      return "making ntdll's code writable failed, left alone";
    }
    for (size_t i = 0; i < N; ++i) {
      NtFlushInstructionCache(NtCurrentProcess(), reinterpret_cast<void*>(Code[i]), 16);
    }
    return {};
  }

  // Checks ntdll's code and, when it is what this replaces, patches it. Returns the note's text.
  fextl::string CheckAndPatch() {
    const auto NtDll = reinterpret_cast<HMODULE>(NtDllBase);
    if (!NtDll || !GetProcAddress(NtDll, "wine_get_version")) {
      return "not under Wine, left alone";
    }
    const auto* Nt = RtlImageNtHeader(NtDll);
    if (!Nt) {
      return "ntdll has no PE header, left alone";
    }
    const auto* SectionHeader = IMAGE_FIRST_SECTION(Nt);
    SectionCount = 0;
    for (WORD i = 0; i < Nt->FileHeader.NumberOfSections && SectionCount < MaxSections; ++i) {
      const auto& H = SectionHeader[i];
      const uint64_t Size = H.Misc.VirtualSize ? H.Misc.VirtualSize : H.SizeOfRawData;
      Sections[SectionCount++] = {NtDllBase + H.VirtualAddress, NtDllBase + H.VirtualAddress + Size, H.Characteristics};
    }

    if (!GetProcAddress(NtDll, "NtWaitForAlertByThreadId") || !GetProcAddress(NtDll, "NtAlertThreadByThreadId")) {
      return "ntdll's thread alert calls not found, left alone";
    }

    Target Targets[] = {
      {"RtlWaitOnAddress", reinterpret_cast<void*>(&FixedWaitOnAddress), 0, 0, 0, 0},
      {"RtlWakeAddressAll", reinterpret_cast<void*>(&FixedWakeAddressAll), 0, 0, 0, 0},
      {"RtlWakeAddressSingle", reinterpret_cast<void*>(&FixedWakeAddressSingle), 0, 0, 0, 0},
    };
    for (auto& T : Targets) {
      T.Code = NativeExport(NtDll, T.Name);
      if (!T.Code) {
        return fextl::fmt::format("{}'s native code not found, left alone", T.Name);
      }
      size_t Entries {};
      if (!FunctionAt(T.Code, T.Begin, T.Length, Entries) || T.Begin != T.Code || T.Length < 16) {
        return fextl::fmt::format("{} (ntdll+{:x}) not found as a whole function in ntdll's function table ({} entries{}), left alone",
                                  T.Name, T.Code - NtDllBase, Entries,
                                  T.Begin ? fextl::fmt::format(", the one there starts at ntdll+{:x}", T.Begin - NtDllBase) : fextl::string {});
      }
    }

    uint64_t Hash = 0xcbf29ce484222325ull;
    uint32_t Ordered = 0;
    for (const auto& T : Targets) {
      Ordered += OrderedAccesses(T);
      for (uint64_t i = 0; i < T.Length; ++i) {
        Hash = (Hash ^ reinterpret_cast<const uint8_t*>(T.Begin)[i]) * 0x100000001b3ull;
      }
    }
    CodeHash = Hash;
    if (Ordered) {
      return fextl::fmt::format("Proton's wait code already has upstream Wine's fix ({} acquire/release accesses in the three functions): "
                                "not stock 11.0-1, left alone",
                                Ordered);
    }

    // Wine's list array: an address all three functions form in full (each computes &futex_queues[(addr >> 4) % 256] from
    // it) whose 256 lists all look unused or empty. On stock 11.0-1 two addresses are formed in all three (the lists and
    // the debug channel); only the lists pass.
    uint64_t Formed[3][128];
    size_t FormedCount[3];
    for (size_t t = 0; t < 3; ++t) {
      FormedCount[t] = FormedIn(Targets[t].Begin, Targets[t].Length, Formed[t], std::size(Formed[t]));
    }
    uint64_t Found[4] {};
    size_t FoundCount = 0;
    for (size_t i = 0; i < FormedCount[2]; ++i) {
      const uint64_t Base = Formed[2][i];
      if (std::binary_search(Formed[0], Formed[0] + FormedCount[0], Base) && std::binary_search(Formed[1], Formed[1] + FormedCount[1], Base) &&
          WineQueuesEmpty(Base) && FoundCount < std::size(Found)) {
        Found[FoundCount++] = Base;
      }
    }
    if (FoundCount != 1) {
      return FoundCount ? fextl::fmt::format("{} candidates for Wine's wait lists in the three functions, left alone", FoundCount) :
                          fextl::string("Wine's wait lists not found in the three functions (or one wasn't empty), left alone");
    }
    const uint64_t Lists = Found[0];

    // Every use of the lists in ntdll's code must be inside the three functions (no copy of them inlined elsewhere).
    uint32_t Outside = 0;
    uint64_t FirstOutside = 0;
    for (size_t s = 0; s < SectionCount; ++s) {
      if (!(Sections[s].Flags & IMAGE_SCN_MEM_EXECUTE)) {
        continue;
      }
      const auto* Code = reinterpret_cast<const uint32_t*>(Sections[s].Begin);
      const auto* End = reinterpret_cast<const uint32_t*>(Sections[s].End & ~3ull);
      for (const uint32_t* C = Code; C < End; ++C) {
        bool Resolved {};
        uint64_t Use = FormedAddress(C, End, Resolved);
        if (Use && !Resolved && Use + 0xfff >= Lists && Use < Lists + WineQueuesBytes) {
          // An ADRP of a page the lists are on, used further on (stock 11.0-1's version_init has one for a variable just
          // after the lists): look further for its use; still nothing counts as a use of the lists.
          Use = FormedAddress(C, End, Resolved, 256);
          if (!Resolved) {
            Use = std::max(Use, Lists);
          }
        }
        if (Use < Lists || Use >= Lists + WineQueuesBytes) {
          continue;
        }
        const uint64_t At = reinterpret_cast<uint64_t>(C);
        bool Inside = false;
        for (auto& T : Targets) {
          if (At >= T.Begin && At < T.Begin + T.Length) {
            ++T.Uses;
            Inside = true;
          }
        }
        if (!Inside) {
          if (!Outside) {
            FirstOutside = At;
          }
          ++Outside;
        }
      }
    }
    if (Outside) {
      return fextl::fmt::format("Wine's wait lists (ntdll+{:x}) used {} times outside the three functions (first at ntdll+{:x}), left alone",
                                Lists - NtDllBase, Outside, FirstOutside - NtDllBase);
    }
    for (const auto& T : Targets) {
      if (!T.Uses) {
        return fextl::fmt::format("{} doesn't use Wine's wait lists at ntdll+{:x}, left alone", T.Name, Lists - NtDllBase);
      }
    }

    for (size_t i = 0; i < QueueCount; ++i) {
      Queues[i].Head.Next = Queues[i].Head.Prev = &Queues[i].Head;
      Queues[i].Head.Addr = nullptr;
      Queues[i].Lock.store(0, std::memory_order_relaxed);
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    // Still nobody waiting in Wine's lists, right before the switch.
    if (!WineQueuesEmpty(Lists)) {
      return "a thread was already waiting in Wine's wait lists, left alone";
    }
    const uint64_t Code[3] = {Targets[0].Code, Targets[1].Code, Targets[2].Code};
    const uint64_t To[3] = {reinterpret_cast<uint64_t>(Targets[0].Replacement), reinterpret_cast<uint64_t>(Targets[1].Replacement),
                            reinterpret_cast<uint64_t>(Targets[2].Replacement)};
    if (fextl::string Failed = PatchAll(Code, To); !Failed.empty()) {
      return Failed;
    }
    return fextl::fmt::format("in use: ntdll's RtlWaitOnAddress (+{:x}), RtlWakeAddressAll (+{:x}) and RtlWakeAddressSingle (+{:x}) run "
                              "FEX's copy with upstream Wine's fix (a waker unlinks an entry before marking it woken), {} lists, list "
                              "lock {}; Wine's lists at ntdll+{:x} were empty and used only there ({}/{}/{} uses)",
                              Targets[0].Code - NtDllBase, Targets[1].Code - NtDllBase, Targets[2].Code - NtDllBase, QueueCount,
                              Active == Mode::Spin ? "spins like Wine's (FEX_WINE_WAIT_FIX=2)" : "sleeps in the kernel after a short spin",
                              Lists - NtDllBase, Targets[0].Uses, Targets[1].Uses, Targets[2].Uses);
  }
} // namespace

void Init() {
  const char* Value = FEX::Windows::Switch("FEX_WINE_WAIT_FIX");
  if (!Value || !Value[0] || Value[0] == '0') {
    Outcome = "off, Wine's own wait lists (FEX_WINE_WAIT_FIX=0)";
    return;
  }
  Active = Value[0] == '2' ? Mode::Spin : Mode::Kernel;
  Outcome = CheckAndPatch();
  if (!Outcome.starts_with("in use")) {
    Active = Mode::Off;
  }
}

void StartStats(bool StatsLog) {
  if (!StatsLog) {
    return;
  }
  FEX::Windows::StatsLogNote(fextl::fmt::format(
    "wine waits (build 141): {}. Proton 11.0's code marks a woken waiter before unlinking it, and a waiter that comes back on its own "
    "can leave while the waker still uses its list entry; the list then never ends and every thread on it spins: the freezes of the "
    "build 118 and 139 runs (main in RtlWakeConditionVariable, workers in SleepConditionVariableSRW, no frames){}",
    Outcome, CodeHash ? fextl::fmt::format(" | ntdll wait code {:016x}", CodeHash) : fextl::string {}));
  if (Active != Mode::Off) {
    Counting = true;
    FEX::Windows::StatsLogSetLineProvider(StatsLine);
  }
}
} // namespace WineWaitFix
