.text
.balign 16

  // __os_arm64x_x64_jump in ARM64EC docs
  // Expects target code address in x9
.globl DispatchJump
DispatchJump:
  str lr, [sp, #-8]! // Push return address to stack, this will be popped by the x86 RET instr.
  b check_target_ec

  // __os_arm64x_dispatch_ret in ARM64EC docs
  // Expects target code address in lr
.globl RetToEntryThunk
RetToEntryThunk:
  mov x9, lr

check_target_ec:
  // Check if target is in fact x86 code
  ldr x16, [x18, #0x60] // TEB->PEB
  ldr x16, [x16, #0x368] // PEB->EcCodeBitMap
  lsr x17, x9, #18
  ldr x16, [x16, x17, lsl #3]
  lsr x17, x9, #12
  lsr x16, x16, x17
  tbnz x16, #0, ExitFunctionEC
  b enter_jit

  // __os_arm64x_dispatch_call_no_redirect in ARM64EC docs
  // Expects target code address in x9, and to be called using a 'blr x16' instruction.
.globl ExitToX64
ExitToX64:
  str lr, [sp, #-8]! // Push return address to stack, this will be popped by the x86 RET instr.

enter_jit:
  ldr x17, [x18, #0x1788] // TEB->ChpeV2CpuAreaInfo
  mov w16, #1
  strb w16, [x17, #0x0] // ChpeV2CpuAreaInfo->InSimulation
  ldr x16, [x17, #0x40] // ChpeV2CpuAreaInfo->EmulatorData[2] - DispatcherLoopTopEnterEC
  br x16 // DispatcherLoopTopEnterEC(RIP:x9, CPUArea:x17)

  // Invoked by KiUserEmulationDispatcher after e.g. an NtContinue to x86 code
.global BeginSimulation
BeginSimulation:
  ldr x17, [x18, #0x1788] // TEB->ChpeV2CpuAreaInfo
  ldr x16, [x17, #0x8] // ChpeV2CpuAreaInfo->EmulatorStackBase
  mov sp, x16
  ldr x0, [x17, #0x18] // ChpeV2CpuAreaInfo->ContextAmd64
  bl "#SyncThreadContext"
  ldr x17, [x18, #0x1788] // TEB->ChpeV2CpuAreaInfo
  ldr x16, [x17, #0x48] // ChpeV2CpuAreaInfo->EmulatorData[3] - DispatcherLoopTopEnterECFillSRA
  mov x11, #0 // Zero ENTRY_FILL_SRA_SINGLE_INST_REG to avoid single step
  br x16 // DispatcherLoopTopEnterECFillSRA(SingleInst:x10, CPUArea:x17)

  // Called into by FEXCore
  // Expects the target code address in x9
.global ExitFunctionEC
ExitFunctionEC:
  // Clear any the AFP NEP and AH bits in FPCR as native code won't expect their behaviour.
  mrs x17, fpcr
  and x17, x17, #~6 // NEP + AH
  msr fpcr, x17
  ldr x17, [x18, #0x1788] // TEB->ChpeV2CpuAreaInfo
  strb wzr, [x17, #0x0] // ChpeV2CpuAreaInfo->InSimulation
  str x9, [x17, #0x50] // ChpeV2CpuAreaInfo->EmulatorDataInline: the native function being entered, for the stats log's sampler
  ldr x17, [x17, #0x20] // ChpeV2CpuAreaInfo->SuspendDoorbell
  ldr w17, [x17]
  cbnz w17, ExitFunctionSuspendPoint

.global ExitFunctionSuspendResumePoint
ExitFunctionSuspendResumePoint:
  // A call from x64 code into XInputSetState (one of FEXRumbleTargets[0..2], all 0 until Rumble.cpp finds xinput): run
  // FEX's replacement (NativeRumbleTarget, see Rumble.h) instead. The entry the game called goes to FEXRumbleTargets[3]
  // for it. A return address is never a function's first instruction, so returns never match. x16 and x17 are free here.
  adrp x16, FEXRumbleTargets
  add x16, x16, #:lo12:FEXRumbleTargets
  ldr x17, [x16]
  cbz x17, rumble_done
  cmp x17, x9
  b.eq rumble_hit
  ldr x17, [x16, #8]
  cmp x17, x9
  b.eq rumble_hit
  ldr x17, [x16, #16]
  cmp x17, x9
  b.ne rumble_done
rumble_hit:
  str x9, [x16, #24]
  adrp x9, NativeRumbleTarget
  add x9, x9, #:lo12:NativeRumbleTarget
rumble_done:

  // A call from one of the game's loader threads (its TEB in FEXLoaderWatch[1..4], see LoaderWatch.h) into CreateEventW,
  // CloseHandle or WaitForMultipleObjects runs FEX's counting wrapper instead, which calls the one it was going to. Only
  // x16 and x17, as above; [0] stays 0 until a loader thread is found.
  adrp x16, FEXLoaderWatch
  add x16, x16, #:lo12:FEXLoaderWatch
  ldr x17, [x16]
  cbnz x17, loader_watch_check
loader_watch_done:

  // Either return to an exit thunk (return to ARM64EC function) or call an entry thunk (call to ARM64EC function).
  // It is assumed that a 'blr x16' instruction is only ever used to call into x86 code from an exit thunk, and that all
  // exported ARM64EC functions have a 4-byte offset to their entry thunk immediately before their first instruction.
  mov x17, x9
  mov w16, #0x200
  movk w16, #0xd63f, lsl 16 // blr x16
  ldursw x23, [x17, #-0x4] // Load either the entry thunk offset or the calling instruction.
  cmp w23, w16
  beq ret_sp_aligned

  // A call into native code. If its x64 return address is the game's once-per-frame clock read (FEXFrameMarkReturn, set
  // by NativeRedirect.cpp once found), store the time in the FEXFrameMarks ring for the stats log's frame times. x4 and lr
  // are set again below before use, x16 is not used again, and frame_mark_check reloads x23.
  adrp x16, FEXFrameMarkReturn
  ldr x16, [x16, #:lo12:FEXFrameMarkReturn]
  cbnz x16, frame_mark_check
frame_mark_done:

  // If it is one of the wait functions FEXWaitWatchTargets names (FrameLatency.cpp), record the call in FEXWaitWatch when
  // it comes from the watched thread. Same registers as above.
  adrp x16, FEXWaitWatchTargets
  add x16, x16, #:lo12:FEXWaitWatchTargets
  ldp x16, x4, [x16]
  cmp x16, x9
  ccmp x4, x9, #4, ne
  b.eq wait_watch_check
wait_watch_done:

  and x23, x23, #-0x4
  add x17, x17, x23 // Resolve entry thunk address.

  mov x4, sp
  tbz x4, #3, ret_sp_misaligned
  ldr lr, [x4], #0x8 // Pop the return address into lr.
  mov sp, x4

ret_sp_aligned:
  br x17

ret_sp_misaligned:
  // In the case of the x64 caller leaving sp only 8-byte aligned, leave the return address on the stack to keep 16-byte
  // alignment and have the callee return to an x86 ret instruction. FEX can then return to the actual caller keeping
  // the misaligned RSP.
  adrp lr, X64ReturnInstr
  ldr lr, [lr, #:lo12:X64ReturnInstr]
  br x17

frame_mark_check:
  mov x4, sp // sp may be only 8-byte aligned here, and loads through a misaligned sp fault
  ldr x4, [x4]
  cmp x4, x16
  b.ne frame_mark_done
  mrs x4, cntvct_el0
  adrp x16, FEXFrameMarks
  add x16, x16, #:lo12:FEXFrameMarks
  ldr lr, [x16] // FEXFrameMarks[0]: marks written so far, then a ring of 1024 times
  and x23, lr, #0x3ff
  add x23, x16, x23, lsl #3
  str x4, [x23, #8]
  add lr, lr, #1
  stlr lr, [x16]
  ldursw x23, [x17, #-0x4]
  b frame_mark_done

wait_watch_check:
  adrp x16, FEXWaitWatch
  add x16, x16, #:lo12:FEXWaitWatch
  ldr x4, [x16, #8] // FEXWaitWatch[1]: the watched thread's TEB
  cmp x4, x18
  b.ne wait_watch_done
  ldr lr, [x16] // FEXWaitWatch[0]: calls recorded so far, then from [4] a ring of 1024 (handle, timeout, function, caller)
  and x23, lr, #0x3ff
  add x23, x16, x23, lsl #5
  mov x4, sp // sp may be only 8-byte aligned here, see frame_mark_check
  ldr x4, [x4] // the x64 return address
  stp x0, x1, [x23, #32]
  stp x9, x4, [x23, #48]
  add lr, lr, #1
  stlr lr, [x16]
  ldursw x23, [x17, #-0x4]
  b wait_watch_done

loader_watch_check:
  ldr x17, [x16, #8]
  cmp x17, x18
  b.eq loader_watch_thread
  ldr x17, [x16, #16]
  cmp x17, x18
  b.eq loader_watch_thread
  ldr x17, [x16, #24]
  cmp x17, x18
  b.eq loader_watch_thread
  ldr x17, [x16, #32]
  cmp x17, x18
  b.ne loader_watch_done
loader_watch_thread:
  ldr x17, [x16, #40]
  cmp x17, x9
  b.eq loader_watch_create
  ldr x17, [x16, #48]
  cmp x17, x9
  b.eq loader_watch_create
  ldr x17, [x16, #56]
  cmp x17, x9
  b.eq loader_watch_create
  ldr x17, [x16, #64]
  cmp x17, x9
  b.eq loader_watch_create
  ldr x17, [x16, #72]
  cmp x17, x9
  b.eq loader_watch_close
  ldr x17, [x16, #80]
  cmp x17, x9
  b.eq loader_watch_close
  ldr x17, [x16, #88]
  cmp x17, x9
  b.eq loader_watch_close
  ldr x17, [x16, #96]
  cmp x17, x9
  b.eq loader_watch_close
  ldr x17, [x16, #104]
  cmp x17, x9
  b.eq loader_watch_wait
  ldr x17, [x16, #112]
  cmp x17, x9
  b.eq loader_watch_wait
  ldr x17, [x16, #120]
  cmp x17, x9
  b.eq loader_watch_wait
  ldr x17, [x16, #128]
  cmp x17, x9
  b.eq loader_watch_wait
  b loader_watch_done
loader_watch_create:
  str x9, [x16, #136] // FEXLoaderWatch[17]: the CreateEventW entry called
  adrp x9, NativeLoaderCreateEventTarget
  add x9, x9, #:lo12:NativeLoaderCreateEventTarget
  b loader_watch_done
loader_watch_close:
  str x9, [x16, #144] // [18]: the CloseHandle entry
  adrp x9, NativeLoaderCloseHandleTarget
  add x9, x9, #:lo12:NativeLoaderCloseHandleTarget
  b loader_watch_done
loader_watch_wait:
  str x9, [x16, #152] // [19]: the WaitForMultipleObjects entry
  adrp x9, NativeLoaderWaitTarget
  add x9, x9, #:lo12:NativeLoaderWaitTarget
  b loader_watch_done

.global ExitFunctionSuspendPoint
ExitFunctionSuspendPoint:
  brk #0xCAFE
  // Resume will jump back to `ExitFunctionSuspendResumePoint`

  // This table will contain all of the possible SVC #x instructions that we might need.
  // We index into this table in DIRECT_SYSCALL_WRAPPER based on the syscall number we find
  // by parsing the table __arm64ec_syscall_ffs from NTDLL.
#define SYSCALL_TABLE_ENTRIES 512
.global SyscallTable
SyscallTable:
  .set SyscallId, 0
  .rept SYSCALL_TABLE_ENTRIES
    svc #SyscallId
    ret
    .set SyscallId, SyscallId + 1
  .endr
.global SyscallTableEnd
SyscallTableEnd:

  // Makes a wrapper for calling a system call directly, skipping the usual ntdll thunks
#define HASH #
#define DIRECT_SYSCALL_WRAPPER(Name, WineIdName) \
  .global Name; \
  Name:; \
    adrp x16, WineSyscallDispatcher; \
    ldr x16, [x16, HASH:lo12:WineSyscallDispatcher]; \
    cbz x16, 1f; \
    mov x9, x30; \
    adrp x8, WineIdName; \
    ldr x8, [x8, HASH:lo12:WineIdName]; \
    blr x16; \
    ret; \
  1:; \
    adrp x16, SyscallTable; \
    add x16, x16, HASH:lo12:SyscallTable; \
    adrp x17, WineIdName; \
    ldr x17, [x17, HASH:lo12:WineIdName]; \
    add x16, x16, x17, lsl HASH 3; \
    br x16

  // Allows for continuing from a full native context, as the NTDLL NtContinue export takes in an x64 context with EC and
  // the conversion to that loses the ARM64EC ABI-disallowed registers that FEX uses.
DIRECT_SYSCALL_WRAPPER("#NtContinueNative", WineNtContinueSyscallId)
DIRECT_SYSCALL_WRAPPER("#NtRaiseExceptionNative", WineNtRaiseExceptionSyscallId)

  // Both of these are wrapped as FEX needs them to setup its call checker at startup time and their NTDLL thunks could
  // already be patched by then (and because the call checker isn't installed, their patched x86 versions would be invoked
  // when called by FEX).
DIRECT_SYSCALL_WRAPPER("#NtAllocateVirtualMemoryNative", WineNtAllocateVirtualMemorySyscallId)
DIRECT_SYSCALL_WRAPPER("#NtProtectVirtualMemoryNative", WineNtProtectVirtualMemorySyscallId)

  // A replacement for the standard ARM64EC call checker that ignores any FFS patches and always redirects to a function's
  // native implementation. As the only library FEX calls into is NTDLL, this is done using a LUT generated at init time.
  // Expects the FFS address in x11, exit thunk address in x10 (unused) and it's own address in x9. Return address is in x11.
.global "CheckCall"
"CheckCall":
  adrp x9, NtDllBase
  ldr x9, [x9, #:lo12:NtDllBase]
  subs x16, x11, x9
  b.lo end
  adrp x17, NtDllRedirectionLUTSize
  ldr x17, [x17, #:lo12:NtDllRedirectionLUTSize]
  cmp x16, x17
  b.hi end
  adrp x17, NtDllRedirectionLUT
  ldr x17, [x17, #:lo12:NtDllRedirectionLUT]
  ldr w11, [x17, x16, lsl #2]
  add x11, x11, x9
end:
  ret

  // Expects target address in x0, and the SP to set in x1
.global "#JumpSetStack"
"#JumpSetStack":
  mov sp, x1
  br x0

  // Native redirects, see NativeRedirect.cpp. RedirectBlock hands out a <Name>Redirect stub in place of a compiled block
  // for a guest function's entry point, so it runs with the register state of a JIT block entry: REG_CALLRET_SP in x17,
  // the CPU state frame in x28, the guest RSP in x23 and sp on the emulator stack. It leaves the JIT exactly like a JIT
  // branch to an ARM64EC function does (ExitFunction in BranchOps.cpp): ExitFunctionEC reads the entry thunk offset
  // stored before <Name>Target, pops the x64 return address into lr and runs <Name>Thunk. The thunk saves what the x64
  // ABI needs preserved beyond AAPCS64 (all of q6-q15), calls the native implementation with the guest's argument
  // registers, puts the result in x8 (RAX) and returns to the x64 caller through RetToEntryThunk. Extra runs first, to pass
  // more arguments.
.macro NATIVE_REDIRECT Name, Target, Extra=
.balign 16
.global \Name\()Redirect
\Name\()Redirect:
  adrp x11, NativeRedirectCallRetSPOffset
  ldr x11, [x11, #:lo12:NativeRedirectCallRetSPOffset]
  str x17, [x28, x11]
  mov sp, x23
  adr x9, \Name\()Target
  b ExitFunctionEC
  .word \Name\()Thunk - \Name\()Target
.global \Name\()Target
\Name\()Target:
  b "\Target"
\Name\()Thunk:
  \Extra
  stp x29, x30, [sp, #-176]!
  mov x29, sp
  stp q6, q7, [sp, #16]
  stp q8, q9, [sp, #48]
  stp q10, q11, [sp, #80]
  stp q12, q13, [sp, #112]
  stp q14, q15, [sp, #144]
  bl "\Target"
  mov x8, x0
  ldp q6, q7, [sp, #16]
  ldp q8, q9, [sp, #48]
  ldp q10, q11, [sp, #80]
  ldp q12, q13, [sp, #112]
  ldp q14, q15, [sp, #144]
  ldp x29, x30, [sp], #176
  b RetToEntryThunk
.endm

  // inflate_fast also gets the guest stack above its return address (x2) and that return address (x3), to see who
  // asked for the stream.
NATIVE_REDIRECT NativeInflateFast, #FEXNativeInflateFast, "mov x2, sp; mov x3, x30"
NATIVE_REDIRECT NativeAdler32Z, #FEXNativeAdler32Z
NATIVE_REDIRECT NativeMD5Update, #FEXNativeMD5Update
NATIVE_REDIRECT NativeMD5Transform, #FEXNativeMD5Transform
  // The job system getter takes no arguments; it gets its return address (x0) to tell the frame wait loop's call apart.
NATIVE_REDIRECT NativeJobSystemGet, #FEXNativeJobSystemGet, "mov x0, x30"
  // XInputSetState(index, vibration), entered from ExitFunctionEC above (the Redirect stub is unused).
NATIVE_REDIRECT NativeRumble, #FEXNativeRumbleSetState
  // The loader call watch's wrappers (LoaderWatch.cpp), entered from ExitFunctionEC above. Each also gets lr and sp after
  // its arguments, for the x64 return address (lr, or [sp] when lr is X64ReturnInstr, see ret_sp_misaligned).
NATIVE_REDIRECT NativeLoaderCreateEvent, #FEXNativeLoaderCreateEventW, "mov x4, x30; mov x5, sp"
NATIVE_REDIRECT NativeLoaderCloseHandle, #FEXNativeLoaderCloseHandle, "mov x1, x30; mov x2, sp"
NATIVE_REDIRECT NativeLoaderWait, #FEXNativeLoaderWaitForMultiple, "mov x4, x30; mov x5, sp"
