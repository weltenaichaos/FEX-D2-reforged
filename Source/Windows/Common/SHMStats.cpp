// SPDX-License-Identifier: MIT
#include "Windows/Common/FEXUnixLib.h"
#include "Windows/Common/SHMStats.h"

#include <FEXCore/fextl/fmt.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <ntstatus.h>
#include <windef.h>
#include <winternl.h>
#include <winnt.h>
#include <winbase.h>
#include <wine/debug.h>

namespace FEX::Windows {
uint32_t StatAlloc::FrontendAllocateSlots(uint32_t NewSize) {
  if (CurrentSize == MAX_STATS_SIZE || IsLocal) {
    LogMan::Msg::DFmt("Ran out of slots. Can't allocate more");
    return CurrentSize;
  }

  auto Result = UnixLib::AllocateSHMSlots(Base, std::min(NewSize, MAX_STATS_SIZE), MAX_STATS_SIZE);

  // Return the new size allocated, if it happened to change.
  return Result.MappedSize;
}

StatAlloc::StatAlloc(FEXCore::SHMStats::AppType AppType, bool LocalFallback) {
  auto Result = UnixLib::AllocateSHMSlots(Base, FEXCore::Utils::FEX_PAGE_SIZE, MAX_STATS_SIZE);

  if (Result.SHMBase) {
    CurrentSize = Result.MappedSize;
    Base = Result.SHMBase;
    SaveHeader(AppType);
    return;
  }

  if (!LocalFallback) {
    return;
  }

  // Commit the whole region up front: the slot allocator can't grow a private allocation in place.
  Base = VirtualAlloc(nullptr, MAX_STATS_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (Base) {
    IsLocal = true;
    CurrentSize = MAX_STATS_SIZE;
    SaveHeader(AppType);
  }
}

StatAlloc::~StatAlloc() {
  if (IsLocal) {
    VirtualFree(Base, 0, MEM_RELEASE);
    return;
  }
  UnixLib::DeleteSHMStatsFile();
}

} // namespace FEX::Windows
