// SPDX-License-Identifier: MIT
#pragma once

// The FEX link of FEX's Turnip build (build 69, turnip-patches/ in this repo): a block of memory FEX allocates in the
// game's process and names to the driver by address in the Unix environment variable TU_FEX_CHANNEL=<hex>. The driver
// (src/freedreno/vulkan/tu_fex.h) checks magic, version and size, then
//  - reads 'Mode' at every vkBeginCommandBuffer: how graphics descriptor caches are invalidated (ModeChangedOnly) and
//    whether an identical push descriptor update keeps its old copy (ModePushReuse), so FEX can switch it each second
//    and one game run compares both ways;
//  - reserves the GPU counters listed in 'Counters' through KGSL and has the GPU write them, with its always-on timer,
//    around every render pass with at least 'SampleMinDraws' draws into a ring of slots FEX reads once a second;
//  - counts descriptor updates, invalidated sets and push descriptor reuse per mode.
// Keep this layout identical to struct tu_fex_channel / tu_fex_slot in turnip-patches/.

#include <cstddef>
#include <cstdint>

namespace FEX::Windows::Turnip {
constexpr uint32_t Magic = 0x58465554u; // 'TUFX'
constexpr uint32_t Version = 1;
constexpr uint32_t MaxCounters = 24;

constexpr uint32_t ModeChangedOnly = 0x1;
constexpr uint32_t ModePushReuse = 0x2;

constexpr uint32_t StatusAttached = 0x1;
constexpr uint32_t StatusDescPatch = 0x2;
constexpr uint32_t StatusCounters = 0x4;
constexpr uint32_t StatusRing = 0x8;
constexpr uint32_t StatusNotKgsl = 0x10;
constexpr uint32_t StatusRingFailed = 0x20;
constexpr uint32_t StatusNotA7xx = 0x40;

// KGSL_PERFCOUNTER_GROUP_*
constexpr uint32_t GroupCp = 0x0;
constexpr uint32_t GroupHlsq = 0x4;
constexpr uint32_t GroupTp = 0x9;
constexpr uint32_t GroupSp = 0xA;

struct Counter {
  uint32_t Group;
  uint32_t Countable;
  uint32_t Reg;  // driver: 0 = not reserved
  int32_t Result; // driver: 0 reserved, -errno failed, 1 not tried
};

struct Channel {
  // 0x000: written by FEX before the driver loads
  uint32_t Magic;
  uint32_t Version;
  uint32_t Size;
  uint32_t CounterCount;
  uint32_t SampleMinDraws;
  uint32_t RingSlots;
  uint32_t Reserved0[2];
  Counter Counters[MaxCounters]; // 0x020
  // 0x1a0: written by FEX while the game runs
  volatile uint32_t Mode;
  uint32_t Reserved1[3];
  // 0x1b0: written by the driver
  volatile uint32_t Status;
  uint32_t Patch;
  volatile uint32_t Devices;
  uint32_t CountersReserved;
  char Driver[48];
  uint64_t RingCpu; // 0x1f0
  uint32_t RingCount;
  uint32_t SlotSize;
  volatile uint64_t RingNext; // 0x200
  // [0] = command buffers without ModeChangedOnly, [1] = with it
  volatile uint64_t CmdBufs[2];
  volatile uint64_t DescEmits[2];
  volatile uint64_t DescSetsFlushed[2];
  volatile uint64_t PushTotal[2];
  volatile uint64_t PushReused[2];
  volatile uint64_t PassesSampled;
  volatile uint64_t PassesSmall;
  uint64_t Reserved2[8];
};

struct Slot {
  uint64_t Seq;
  uint32_t Draws;
  uint16_t Width;
  uint16_t Height;
  uint8_t Mode;
  uint8_t Sysmem;
  uint8_t Colors;
  uint8_t Attachments;
  uint32_t Format0;
  uint32_t Done; // (uint32_t)Seq once the GPU wrote the values
  uint32_t CmdBuf;
  uint64_t TsBegin; // CP_ALWAYS_ON_COUNTER, 19.2 MHz
  uint64_t TsEnd;
  // then uint64_t values at the pass start [CounterCount], then at its end [CounterCount]
};

static_assert(sizeof(Counter) == 16);
static_assert(offsetof(Channel, Mode) == 0x1a0);
static_assert(offsetof(Channel, Status) == 0x1b0);
static_assert(offsetof(Channel, RingCpu) == 0x1f0);
static_assert(offsetof(Channel, RingNext) == 0x200);
static_assert(offsetof(Channel, CmdBufs) == 0x208);
static_assert(sizeof(Channel) == 0x2a8);
static_assert(sizeof(Slot) == 48);

// The channel EnvironmentVariablesHandling set up for this process (FEX_TU_CHANNEL), or nullptr.
Channel* GetChannel();
// FEX_TU_DESC: 0 stock, 1 changed-only + push reuse always, 2 alternate every second (needs the stats log).
uint32_t DescSwitch();
} // namespace FEX::Windows::Turnip
