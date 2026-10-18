// SPDX-License-Identifier: MIT
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/memory.h>
#include <FEXCore/fextl/vector.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/SpinWaitLock.h>
#include <FEXCore/Utils/Threads.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <windef.h>
#include <winbase.h>
#define __WINESRC__
#include <winternl.h>
#include <libloaderapi.h>
#include "FEXUnixLib.h"
#include "Priv.h"
#include "StatsLog.h"

#define PR_SET_VMA 0x53564d41
#define PR_SET_VMA_ANON_NAME 0

#define MADV_HUGEPAGE 14
#define MADV_NOHUGEPAGE 15

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace FEX::Windows::UnixLib {
static bool SupportsVirtualName {true};
unixlib_handle_t UnixLibHandle {};

decltype(__wine_unix_call_dispatcher) UnixCallDispatcher {};

using wine_server_handle_to_fd_t = NTSTATUS(CDECL*)(HANDLE, unsigned int, int*, unsigned int*);
static wine_server_handle_to_fd_t WineServerHandleToFd {};

#ifdef ARCHITECTURE_arm64ec
// On ARM64EC, indirect calls go through __os_arm64x_dispatch_icall which invokes
// FEX's custom call checker. Use a naked trampoline to bypass the dispatch mechanism
// and call the unix dispatcher directly via a register branch.
static decltype(__wine_unix_call_dispatcher) UnixCallDispatcherDirect {};

static NTSTATUS __attribute__((naked)) TrampolineCall(unixlib_handle_t, unsigned int, void*) {
  asm(R"(
  adrp x16, %[Displace];
  ldr x16, [x16, #:lo12:%[Displace]]
  br x16
  )" ::[Displace] "S"(&UnixCallDispatcherDirect)
      : "memory");
}
#endif
bool Init(HMODULE NtDll) {
  const auto Sym = GetProcAddress(NtDll, "__wine_unix_call_dispatcher");
  WineServerHandleToFd = reinterpret_cast<wine_server_handle_to_fd_t>(GetProcAddress(NtDll, "wine_server_handle_to_fd"));

  if (!Sym) {
    return false;
  }

  auto TryNewWineMethod = []() {
#ifdef ARCHITECTURE_arm64ec
    auto Name = InitUnicodeString(L"libarm64ecfex");
#else
    auto Name = InitUnicodeString(L"libwow64fex");
#endif

    // Not supported in Proton at all, but supported in upstream WINE.
    uint64_t Result[2];
    if (NtQueryVirtualMemory(NtCurrentProcess(), &Name, MemoryWineLoadUnixLibByName, Result, sizeof(Result), nullptr)) {
      return false;
    }

    // Result[0] = unixlib_module_t
    // Result[1] = unixlib_handle_t
    // Module is ignored as it's only used to unload.
    UnixLibHandle = Result[1];
    return true;
  };

  auto TryOldWineMethod = []() {
    // Supported in Proton 11 and Experimental (2026-06-26).
    return NtQueryVirtualMemory(NtCurrentProcess(), &__ImageBase, MemoryWineUnixFuncs, &UnixLibHandle, sizeof(UnixLibHandle), nullptr) == 0;
  };

  if (!TryNewWineMethod() && !TryOldWineMethod()) {
    return false;
  }

#ifdef ARCHITECTURE_arm64ec
  UnixCallDispatcherDirect = *reinterpret_cast<decltype(__wine_unix_call_dispatcher)*>(Sym);
  UnixCallDispatcher = TrampolineCall;
#else
  UnixCallDispatcher = *reinterpret_cast<decltype(__wine_unix_call_dispatcher)*>(Sym);
#endif

  // Give a log saying that the unix lib was loaded.
  LogMan::Msg::IFmt("FEX: Loaded FEXUnixLib");
  return true;
}

bool Available() {
  return UnixLibHandle != 0;
}

bool TryEnableHardwareTSO() {
  if (!Available()) {
    return false;
  }

  FEXUnixLib_SetHardwareTSOControlArgs Args {
    .Enable = true,
  };

  return Call(FEXUnixLibFunctions::SetHardwareTSOControl, &Args) == STATUS_SUCCESS;
}

bool SetKernelUnalignedAtomicControl(uint64_t Flags) {
  if (!Available()) {
    return false;
  }

  FEXUnixLib_SetKernelUnalignedAtomicControl Args {
    .Flags = Flags,
  };

  return Call(FEXUnixLibFunctions::SetKernelUnalignedAtomicControl, &Args) == STATUS_SUCCESS;
}

void VirtualTHPControl(const void* Ptr, size_t Size, FEXCore::Allocator::THPControl Control) {
  if (!Available()) {
    return;
  }

  FEXUnixLib_Madvise Args {
    .Addr = Ptr,
    .Size = Size,
    .Advise = Control == FEXCore::Allocator::THPControl::Disable ? MADV_NOHUGEPAGE : MADV_HUGEPAGE,
  };

  Call(FEXUnixLibFunctions::Madvise, &Args);
}

void VirtualName(const char* Name, const void* Ptr, size_t Size) {
  if (!SupportsVirtualName || !Available()) {
    return;
  }

  FEXUnixLib_SetVMAName Args {
    .Addr = Ptr,
    .Size = Size,
    .Name = Name,
  };

  if (Call(FEXUnixLibFunctions::SetVMAName, &Args) != STATUS_SUCCESS) {
    SupportsVirtualName = false;
  }
}

SHMSlotResult AllocateSHMSlots(void* SHMBase, uint32_t MapSize, uint32_t MaxSize) {
  if (!Available()) {
    return {};
  }

  FEXUnixLib_GetSHMStatsVMA Args {
    .SHMBase = SHMBase,
    .MapSize = MapSize,
    .MaxSize = MaxSize,
  };

  if (Call(FEXUnixLibFunctions::GetSHMStatsVMA, &Args) == STATUS_SUCCESS) {
    return {
      .SHMBase = Args.SHMBase,
      .MappedSize = Args.MapSize,
    };
  }

  return {};
}

void DeleteSHMStatsFile() {
  if (!Available()) {
    return;
  }

  Call(FEXUnixLibFunctions::DeleteSHMStatsFile, nullptr);
}

namespace {
  // Disk cache prefetch: touch every page of the mapped cache file on a low-priority thread at startup, so lookups during
  // play copy from memory that is already mapped. Without it each hit takes a page fault, which costs ~50-250 us during
  // play (the game's protector keeps changing page protections, so faults wait on the kernel's memory map lock) and
  // occasionally 50 ms. FEX_DISKCACHE_PREFETCH=0 turns it off, FEX_DISKCACHE_PREFETCH_MB=N caps it.
  struct PrefetchJob {
    const volatile uint8_t* Base;
    uint64_t Size;
  };

  void* PrefetchMapping(void* Arg) {
    const auto* Job = static_cast<const PrefetchJob*>(Arg);
    uint64_t Frequency {};
    __asm volatile("mrs %[Res], CNTFRQ_EL0" : [Res] "=r"(Frequency));
    const uint64_t Start = FEXCore::Utils::SpinWaitLock::GetCycleCounter();
    uint64_t Sum {};
    for (uint64_t Offset = 0; Offset < Job->Size; Offset += 4096) {
      Sum += Job->Base[Offset];
    }
    const uint64_t End = FEXCore::Utils::SpinWaitLock::GetCycleCounter();
    const uint64_t Ms = Frequency ? (End - Start) * 1000 / Frequency : 0;
    static_cast<void>(Sum);
    FEX::Windows::StatsLogNote(fextl::fmt::format("diskcache prefetch: {} MB in {} ms", Job->Size >> 20, Ms));
    return nullptr;
  }

  void StartPrefetch(const void* Base, uint64_t Size) {
    const char* Switch = getenv("FEX_DISKCACHE_PREFETCH");
    if (!Base || !Size || (Switch && Switch[0] == '0')) {
      FEX::Windows::StatsLogNote("diskcache prefetch: off");
      return;
    }
    // FEX_DISKCACHE_PREFETCH_MB caps how much of the file is kept in memory (default 2048, from the start of the file).
    const char* Cap = getenv("FEX_DISKCACHE_PREFETCH_MB");
    const uint64_t CapMB = Cap && Cap[0] ? strtoull(Cap, nullptr, 10) : 2048;
    Size = std::min<uint64_t>(Size, CapMB << 20);
    if (!Size) {
      FEX::Windows::StatsLogNote("diskcache prefetch: off (FEX_DISKCACHE_PREFETCH_MB=0)");
      return;
    }
    // Both stay alive for the whole process: the thread reads the job after creation.
    static fextl::vector<fextl::unique_ptr<PrefetchJob>> Jobs;
    static fextl::vector<fextl::unique_ptr<FEXCore::Threads::Thread>> Threads;
    auto& Job = Jobs.emplace_back(fextl::make_unique<PrefetchJob>(PrefetchJob {static_cast<const volatile uint8_t*>(Base), Size}));
    Threads.emplace_back(FEXCore::Threads::Thread::Create(PrefetchMapping, Job.get(), {.LowPriority = true, .Internal = true},
                                                          "FEXCachePrefetch"));
  }
} // namespace

void* MapFile(HANDLE FileHandle, uint64_t MapSize, uint64_t* MappedSize) {
  if (Available() && WineServerHandleToFd) {
    FEXUnixLib_MapFile Args {
      .FD = -1,
      .MapSize = MapSize,
    };

    // this is a dup equivalent
    auto Result = WineServerHandleToFd(FileHandle, FILE_READ_DATA, &Args.FD, nullptr);
    if (Result != STATUS_SUCCESS || Args.FD == -1) {
      return nullptr;
    }

    if (Call(FEXUnixLibFunctions::MapFile, &Args) == STATUS_SUCCESS) {
      *MappedSize = MapSize;
      FEX::Windows::StatsLogNote(fextl::fmt::format("diskcache file mapping: unixlib, {} bytes", MapSize));
      return Args.Result;
    }
  }

  // Without the unixlib (GameNative ships only the PE DLLs), map the file through a Windows section instead. A section
  // can't be larger than the read-only file, so this only covers what the file holds now; entries written later in
  // this session are read from the file.
  HANDLE Section {};
  auto Status = NtCreateSection(&Section, SECTION_MAP_READ | SECTION_QUERY, nullptr, nullptr, PAGE_READONLY, SEC_COMMIT, FileHandle);
  if (!NT_SUCCESS(Status)) {
    FEX::Windows::StatsLogNote(fextl::fmt::format("diskcache file mapping: none (section status {:#x})", static_cast<uint32_t>(Status)));
    return nullptr;
  }
  void* View {};
  SIZE_T ViewSize {};
  Status = NtMapViewOfSection(Section, NtCurrentProcess(), &View, 0, 0, nullptr, &ViewSize, ViewUnmap, 0, PAGE_READONLY);
  NtClose(Section);
  if (!NT_SUCCESS(Status)) {
    FEX::Windows::StatsLogNote(fextl::fmt::format("diskcache file mapping: none (view status {:#x})", static_cast<uint32_t>(Status)));
    return nullptr;
  }
  *MappedSize = ViewSize;
  FEX::Windows::StatsLogNote(fextl::fmt::format("diskcache file mapping: section, {} bytes", ViewSize));
  // Only touch bytes the file holds: past the end of the file a mapped page may not be backed.
  FILE_STANDARD_INFORMATION FileInfo {};
  IO_STATUS_BLOCK IOSB {};
  if (NT_SUCCESS(NtQueryInformationFile(FileHandle, &IOSB, &FileInfo, sizeof(FileInfo), FileStandardInformation))) {
    StartPrefetch(View, std::min<uint64_t>(ViewSize, FileInfo.EndOfFile.QuadPart));
  }
  return View;
}

uint32_t GetPID() {
  if (Available()) {
    FEXUnixLib_GetPID Args {};
    Call(FEXUnixLibFunctions::GetPID, &Args);
    return Args.Result;
  }

  return GetCurrentProcessId();
}

} // namespace FEX::Windows::UnixLib
