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
//  - counts descriptor updates, invalidated sets and push descriptor reuse per mode;
//  - reads 'Compile' once when the device is created (fex2 and later): CompilePushSpeculate lets loads through push
//    descriptor sets (vkd3d's root constant buffers) run in the shader's early preamble, ahead of the draw's waves.
//  - (fex3 and later) with ModeDepthClear3d in 'Mode', clears a depth/stencil attachment at the start of a direct
//    (sysmem) pass with a 3D draw through the depth unit instead of the 2D engine, and counts both kinds.
//  - (fex4 and later) with ModeDepthClearFastZ too, runs that 3D draw with the fragment shader disabled (fast Z).
//  - (fex5 and later) with ModeBudgetCache, the memory budget query (vkGetPhysicalDeviceMemoryProperties2 with
//    VK_EXT_memory_budget) reads /proc/meminfo at most every 250 ms instead of on every call, and counts the queries,
//    the file reads and their time.
//  - (fex6 and later) with CompilePreambleLoads in 'Compile', shader preambles issue their memory loads first (the
//    ldc.k copies of constant buffer ranges included) and store each value once, so the preamble that runs ahead of
//    every draw waits for memory fewer times.
//  - (fex7 and later) with CompileLoopPrefetch in 'Compile', fragment shader loops that walk a list read the next
//    entry while the current one is worked on (one memory wait per entry instead of two), and count the reads moved
//    and the shaders that had any in 'LoopPrefetchLoads' / 'LoopPrefetchShaders'.
//  - (fex9 and later) with CompileCpUbo in 'Compile', the command processor loads the ranges of push descriptor set
//    UBOs (vkd3d's root constant buffers) that graphics shaders read into their constants before each draw, and a draw
//    whose shaders then don't read the push set through descriptors keeps the previous descriptor set state when only
//    the push set changed (fewer 'descriptor updates' than 'push' on the 'turnip' line).
//  - (fex10 and later) with CompileUboTable as well, push set UBOs that shaders still read as UBOs (no known range,
//    like indexed bone matrices) are read through the shader's own UBO table, whose entries the driver writes from the
//    pushed descriptors before each draw, so those draws keep the descriptor set state too.
//  - (fex11 and later) with ModeSwitch in 'Mode', a graphics pipeline switch only sends what changed: state blocks with
//    the same bytes as the ones the GPU ran at the last draw keep that block's address (the command processor skips
//    them), and for pipelines without tessellation/geometry shaders only the vertex/fragment stage whose shader changed
//    is reset and sent again. The driver counts binds, blocks run/kept and stage resets/kept into the extension after
//    the base layout, which 'ExtSize' announces ('Size' stays the base layout so older drivers still accept the block).
//  - (fex12 and later) with CompileNoAliasAlt in 'Compile', big fragment shaders (1,024+ instructions, 64+ alias.tex
//    helpers; D2R's lighting shaders) get a second binary without alias.tex, and with ModeNoAlias in 'Mode' pipeline
//    binds use that binary. Counts in the extension (NaBinds..NaPipes, ExtSize 0x358).
//  - (fex13 and later) with CompileNoPreambleAlt in 'Compile', that second binary (fragment shaders with 1,024+
//    instructions and a preamble of 32+) is compiled without preamble instead (or as well), with its own constant
//    layout; ModeNoAlias (= ModeAltFs) picks it the same way. NaPreamble in the extension (ExtSize 0x360).
// Keep this layout identical to struct tu_fex_channel / tu_fex_slot in turnip-patches/.

#include <cstddef>
#include <cstdint>

namespace FEX::Windows::Turnip {
constexpr uint32_t Magic = 0x58465554u; // 'TUFX'
constexpr uint32_t Version = 1;
constexpr uint32_t MaxCounters = 24;

constexpr uint32_t ModeChangedOnly = 0x1;
constexpr uint32_t ModePushReuse = 0x2;
constexpr uint32_t ModeDepthClear3d = 0x4; // fex3 and later
constexpr uint32_t ModeDepthClearFastZ = 0x8; // fex4 and later: the 3D depth clear draw runs without fragment shader
constexpr uint32_t ModeBudgetCache = 0x10;    // fex5 and later: /proc/meminfo read at most every 250 ms by the budget query
constexpr uint32_t ModeSwitch = 0x20;         // fex11 and later: pipeline switches send only what changed
constexpr uint32_t ModeNoAlias = 0x40;        // fex12 and later: pipelines use the fragment shader binary without alias.tex
constexpr uint32_t ModeAltFs = ModeNoAlias;   // fex13 and later: the same bit picks the second binary however it was made
// Slot::Mode bit (fex3 and later): the pass clears a depth/stencil attachment at its start.
constexpr uint8_t SlotDepthClear = 0x80;

constexpr uint32_t CompilePushSpeculate = 0x1;
constexpr uint32_t CompilePreambleLoads = 0x2; // fex6 and later: preamble loads first, each value stored once
constexpr uint32_t CompileLoopPrefetch = 0x4;  // fex7 and later: list loops read the next entry one iteration ahead
constexpr uint32_t CompileCpUbo = 0x8;         // fex9 and later: push set UBO ranges loaded by the command processor
constexpr uint32_t CompileUboTable = 0x10;     // fex10 and later: other push set UBO reads through the shader's UBO table
constexpr uint32_t CompileNoAliasAlt = 0x20;   // fex12 and later: big fragment shaders also compiled without alias.tex
constexpr uint32_t CompileNoPreambleAlt = 0x40; // fex13 and later: that second binary is compiled without preamble

constexpr uint32_t StatusAttached = 0x1;
constexpr uint32_t StatusDescPatch = 0x2;
constexpr uint32_t StatusCounters = 0x4;
constexpr uint32_t StatusRing = 0x8;
constexpr uint32_t StatusNotKgsl = 0x10;
constexpr uint32_t StatusRingFailed = 0x20;
constexpr uint32_t StatusNotA7xx = 0x40;
constexpr uint32_t StatusPushSpeculate = 0x80; // fex2 and later: CompilePushSpeculate honored
constexpr uint32_t StatusDepthClear3d = 0x100;  // fex3 and later: ModeDepthClear3d honored (a7xx)
constexpr uint32_t StatusDepthClearFastZ = 0x200; // fex4 and later: ModeDepthClearFastZ honored (a7xx)
constexpr uint32_t StatusBudgetCache = 0x400;     // fex5 and later: ModeBudgetCache honored
constexpr uint32_t StatusPreambleLoads = 0x800;   // fex6 and later: CompilePreambleLoads honored
constexpr uint32_t StatusLoopPrefetch = 0x1000;   // fex7 and later: CompileLoopPrefetch honored
constexpr uint32_t StatusCpUbo = 0x8000;          // fex9 and later: CompileCpUbo honored (a7xx without consts via preamble)
constexpr uint32_t StatusUboTable = 0x10000;      // fex10 and later: CompileUboTable honored (needs StatusCpUbo)
constexpr uint32_t StatusSwitch = 0x20000;        // fex11 and later: ModeSwitch honored (a7xx)
constexpr uint32_t StatusSwitchStats = 0x40000;   // fex11 and later: the switch counts in the extension are written
constexpr uint32_t StatusNoAlias = 0x80000;       // fex12 and later: CompileNoAliasAlt honored (a7xx)
constexpr uint32_t StatusNoAliasStats = 0x100000; // fex12 and later: the no-alias counts in the extension are written
constexpr uint32_t StatusNoPreamble = 0x200000;   // fex13 and later: CompileNoPreambleAlt honored (a7xx, consts by the CP)

// KGSL_PERFCOUNTER_GROUP_*
constexpr uint32_t GroupCp = 0x0;
constexpr uint32_t GroupPc = 0x2;
constexpr uint32_t GroupVfd = 0x3;
constexpr uint32_t GroupHlsq = 0x4;
constexpr uint32_t GroupTse = 0x6;
constexpr uint32_t GroupRas = 0x7;
constexpr uint32_t GroupUche = 0x8; // the GPU's L2 cache
constexpr uint32_t GroupTp = 0x9;
constexpr uint32_t GroupSp = 0xA;
constexpr uint32_t GroupRb = 0xB;
constexpr uint32_t GroupLrz = 0x19;

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
  uint32_t Compile; // 0x018, Compile*: read once when the driver creates a device (fex2 and later; fex1 ignores it)
  uint32_t ExtSize; // 0x01c (fex11 and later): bytes FEX allocated, extension included (0 in older FEX builds)
  Counter Counters[MaxCounters]; // 0x020
  // 0x1a0: written by FEX while the game runs
  volatile uint32_t Mode;
  volatile uint32_t BudgetReads; // 0x1a4 (fex5 and later, driver): /proc/meminfo reads by the memory budget query
  // 0x1a8 (fex7 and later, driver): list reads moved one loop iteration ahead by this run's shader compiles (shaders
  // from a cache are not counted), and the fragment shaders that had any
  volatile uint32_t LoopPrefetchLoads;
  volatile uint32_t LoopPrefetchShaders;
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
  // 0x268 (fex3 and later, 0 before): depth/stencil clears recorded at the start of passes that may run direct,
  // [0] = 2D engine, [1] = 3D draw (ModeDepthClear3d), and the pixels of their render areas
  volatile uint64_t DepthClears[2];
  volatile uint64_t DepthClearPx[2];
  // 0x288 (fex4 and later): of the 3D draw clears, the ones without fragment shader (ModeDepthClearFastZ)
  volatile uint64_t DepthClearsFastZ;
  volatile uint64_t DepthClearPxFastZ;
  // 0x298 (fex5 and later): memory budget queries, and the nanoseconds they spent getting the available system memory
  volatile uint64_t BudgetQueries;
  volatile uint64_t BudgetNs;
  // 0x2a8 (fex11 and later, driver, only when ExtSize covers it): graphics pipeline binds as recorded, added at
  // vkEndCommandBuffer; [0] = command buffers without ModeSwitch, [1] = with it
  volatile uint64_t SwBinds[2];
  volatile uint64_t SwGroupsRun[2];  // draw state blocks those binds made the command processor run (new address or dirty)
  volatile uint64_t SwGroupsKept[2]; // blocks kept at the address the GPU already had (skipped)
  volatile uint64_t SwStageResets;   // vertex/fragment stages reset because their shader changed (ModeSwitch)
  volatile uint64_t SwStagesKept;    // vertex/fragment stages left alone (ModeSwitch)
  volatile uint64_t SwStock;         // binds with ModeSwitch that took the stock path
  volatile uint64_t SwDrawGroups[2]; // blocks re-made at draws (dynamic state, depth plane): [0] run, [1] kept
  // 0x300 (fex12 and later, driver, only when ExtSize covers it): binds of graphics pipelines whose fragment shader has
  // the binary without alias.tex, added at vkEndCommandBuffer; [0] = stock binary used, [1] = the one without (ModeNoAlias)
  volatile uint64_t NaBinds[2];
  // 0x310: fragment shaders that got the second binary (compiled or loaded from a cache), and totals for both binaries
  volatile uint32_t NaShaders;
  volatile uint32_t NaRefused;  // second binary compiled but dropped (differs in more than code and registers)
  volatile uint32_t NaInstr[2]; // instructions [0] stock, [1] without alias.tex
  volatile uint32_t NaNops[2];
  volatile uint32_t NaAlias[2]; // alias.tex instructions
  volatile uint32_t NaMovs[2];
  volatile uint32_t NaRegs[2];  // full registers (vec4 units)
  volatile uint32_t NaSsStall[2]; // ir3's estimated (ss) stall cycles
  volatile uint32_t NaSyStall[2]; // ir3's estimated (sy) stall cycles
  // 0x350: graphics pipelines made whose fragment shader has the second binary: [0] can use it, [1] cannot (they use
  // the reduced-constants binary)
  volatile uint32_t NaPipes[2];
  // 0x358 (fex13 and later): preamble instructions of both binaries (totals like NaInstr)
  volatile uint32_t NaPreamble[2];
};
// 'Size' = the layout before fex11.
constexpr uint32_t ChannelBaseSize = 0x2a8;
// The fex11 layout (switch counts).
constexpr uint32_t ChannelSwSize = 0x300;
// The fex12 layout (no-alias counts).
constexpr uint32_t ChannelNaSize = 0x358;

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
static_assert(offsetof(Channel, Compile) == 0x18);
static_assert(offsetof(Channel, Mode) == 0x1a0);
static_assert(offsetof(Channel, Status) == 0x1b0);
static_assert(offsetof(Channel, RingCpu) == 0x1f0);
static_assert(offsetof(Channel, RingNext) == 0x200);
static_assert(offsetof(Channel, CmdBufs) == 0x208);
static_assert(offsetof(Channel, DepthClears) == 0x268);
static_assert(offsetof(Channel, DepthClearsFastZ) == 0x288);
static_assert(offsetof(Channel, BudgetReads) == 0x1a4);
static_assert(offsetof(Channel, LoopPrefetchLoads) == 0x1a8);
static_assert(offsetof(Channel, BudgetQueries) == 0x298);
static_assert(offsetof(Channel, ExtSize) == 0x1c);
static_assert(offsetof(Channel, SwBinds) == ChannelBaseSize);
static_assert(offsetof(Channel, SwDrawGroups) == 0x2f0);
static_assert(offsetof(Channel, NaBinds) == ChannelSwSize);
static_assert(offsetof(Channel, NaShaders) == 0x310);
static_assert(offsetof(Channel, NaRegs) == 0x338);
static_assert(offsetof(Channel, NaSsStall) == 0x340);
static_assert(offsetof(Channel, NaPipes) == 0x350);
static_assert(offsetof(Channel, NaPreamble) == ChannelNaSize);
static_assert(sizeof(Channel) == 0x360);
static_assert(sizeof(Slot) == 48);

// The channel EnvironmentVariablesHandling set up for this process (FEX_TU_CHANNEL), or nullptr.
Channel* GetChannel();
// FEX_TU_DESC: 0 stock, 1 changed-only + push reuse always, 2 alternate every second (needs the stats log).
uint32_t DescSwitch();
// FEX_TU_DEPTH_CLEAR: 0 stock (2D engine), 1 3D draw always, 2 3D draw always with fast Z in every other second
// (needs the stats log), 3 3D draw with fast Z always.
uint32_t DepthClearSwitch();
// FEX_TU_BUDGET_CACHE: true = ModeBudgetCache asked for.
bool BudgetCacheSwitch();
// FEX_TU_SWITCH: 0 stock pipeline switches, 1 ModeSwitch always, 2 ModeSwitch in every other second (needs the stats
// log; without it nothing flips it, so it stays on).
uint32_t PipelineSwitchSwitch();
// FEX_TU_NOALIAS: 0 stock lighting shaders, 1 the binaries without alias.tex always, 2 those in every other second
// (needs the stats log; without it nothing flips it, so it stays on).
uint32_t NoAliasSwitch();
// FEX_TU_NOPREAMBLE: 0 stock, 1 IR3_SHADER_DEBUG=nopreamble for every shader (set before the driver loads), 2 (alt) the
// lighting shaders' second binary without preamble in every other second (needs the stats log; without it nothing flips
// it, so it stays on), 3 (lit) that binary always.
uint32_t NoPreambleSwitch();
} // namespace FEX::Windows::Turnip
