// SPDX-License-Identifier: MIT
#include "Threads.h"
#include "CRT/CRT.h"
#include "Switches.h"

#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/Threads.h>
#include <FEXCore/fextl/memory.h>

#include <winternl.h>
#include <windows.h>

namespace FEX::Windows {
namespace WinThreadImpl {
  class Thread final : public FEXCore::Threads::Thread {
  public:
    Thread(FEXCore::Threads::ThreadFunc Func, void* Arg, FEXCore::Threads::Flags Flags, const char* ThreadName)
      : UserFunc {Func}
      , UserArg {Arg}
      , Flags {Flags} {
      // hide everything from guest, don't initialize anything, we'll do that manually in RunThread()
      ULONG CreateFlags = THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH | THREAD_CREATE_FLAGS_SKIP_LOADER_INIT | THREAD_CREATE_FLAGS_BYPASS_PROCESS_FREEZE;
      if (Flags.Internal) {
        CreateFlags |= THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER;
      }
      NTSTATUS Status = NtCreateThreadEx(&Handle, THREAD_ALL_ACCESS, nullptr, GetCurrentProcess(),
                                         reinterpret_cast<PRTL_THREAD_START_ROUTINE>(&Thread::RunThread), this, CreateFlags, 0, 0, 0, nullptr);
      if (Status < 0) {
        LogMan::Msg::EFmt("NtCreateThreadEx failed: 0x{:x}", static_cast<uint32_t>(Status));
        Handle = nullptr;
      }

      if (ThreadName) {
        UNICODE_STRING ThreadNameW;
        if (RtlCreateUnicodeStringFromAsciiz(&ThreadNameW, ThreadName)) {
          THREAD_NAME_INFORMATION info {
            .ThreadName = ThreadNameW,
          };
          NtSetInformationThread(Handle, static_cast<THREADINFOCLASS>(38) /* ThreadNameInformation */, &info, sizeof(info));
          RtlFreeUnicodeString(&ThreadNameW);
        }
      }
    }

    bool joinable() override {
      return Handle != nullptr;
    }

    bool join(void** ret) override {
      if (!Handle) {
        return false;
      }
      NtWaitForSingleObject(Handle, FALSE, nullptr);
      if (ret) {
        *ret = ReturnValue;
      }
      return true;
    }

    bool detach() override {
      // almost no-op, it's already detached, just deref handle
      if (Handle) {
        NtClose(Handle);
        Handle = nullptr;
      }
      return true;
    }

    bool IsSelf() override {
      return TID == GetCurrentThreadId();
    }

    ~Thread() override {
      if (Handle) {
        NtClose(Handle);
      }
    }

    FEXCore::Threads::Flags GetFlags() const {
      return Flags;
    }

  private:
    static bool HelpersAtNormal() {
      const char* Value = Switch("FEX_HELPER_PRIO");
      return Value && Value[0] == 'n';
    }

    static void RunThread(Thread* This) {
      This->TID = GetCurrentThreadId();
      // Upstream runs low priority helpers (disk cache writer, cache prefetch) at IDLE. Wine on Linux turns that into nice
      // 19, about 1.5% of a core next to busy game threads, while the writer holds locks every compile needs.
      // FEX_HELPER_PRIO=normal (the D2R default) runs them at normal priority instead.
      if (This->GetFlags().LowPriority && !HelpersAtNormal()) {
        LONG Priority = THREAD_BASE_PRIORITY_IDLE;
        NtSetInformationThread(NtCurrentThread(), ThreadBasePriority, &Priority, sizeof(Priority));
      }
      // do initialization we skipped earlier here around the user entrypoint
      FEX::Windows::InitCRTThread();
      This->ReturnValue = This->UserFunc(This->UserArg);
      FEX::Windows::DeinitCRTThread();
      NtTerminateThread(GetCurrentThread(), 0);
    }

    FEXCore::Threads::ThreadFunc UserFunc;
    void* UserArg;
    FEXCore::Threads::Flags Flags {};
    HANDLE Handle {};
    DWORD TID {};
    void* ReturnValue {};
  };

  fextl::unique_ptr<FEXCore::Threads::Thread>
  CreateThread(FEXCore::Threads::ThreadFunc Func, void* Arg, FEXCore::Threads::Flags Flags, const char* ThreadName) {
    return fextl::make_unique<Thread>(Func, Arg, Flags, ThreadName);
  }

  void CleanupAfterFork() {}
} // namespace WinThreadImpl

void SetupThreadHandlers() {
  FEXCore::Threads::Pointers Ptrs = {
    .CreateThread = WinThreadImpl::CreateThread,
    .CleanupAfterFork = WinThreadImpl::CleanupAfterFork,
  };
  FEXCore::Threads::Thread::SetInternalPointers(Ptrs);
}
} // namespace FEX::Windows
