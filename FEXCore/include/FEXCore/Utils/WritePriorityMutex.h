// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <cstdint>
#include <limits.h>

#if !defined(_WIN32)
#include <linux/futex.h> /* Definition of FUTEX_* constants */
#include <sys/syscall.h> /* Definition of SYS_* constants */
#include <unistd.h>
#else
#include <synchapi.h>
// Don't pull in all WIN32 headers for INFINITE. Causes too many problems.
#ifndef INFINITE
#define INFINITE 0xffffffff
#endif
#endif

#include <FEXCore/Utils/LogManager.h>

#include <FEXCore/Utils/SpinWaitLock.h>

namespace FEXCore::Utils::WritePriorityMutex {

// Polite writers and contention stats are only built for Windows on ARM64 (ARM64EC and WOW64).
#if defined(_WIN32) && defined(ARCHITECTURE_arm64)
#define FEX_WRITE_PRIORITY_MUTEX_EXTRAS 1
#endif

#ifdef FEX_WRITE_PRIORITY_MUTEX_EXTRAS
// Kernel waits (FEX_KERNEL_FUTEX, set once at process start before any thread can wait): sleep and wake through the
// Linux kernel's futex call, as the Linux build does, instead of Wine's WaitOnAddress. Wine keeps its waiters in 256
// shared lists, each guarded by a plain spin lock taken in user code; a thread pushed off its CPU while it holds one
// (in the build 124 log a game thread on a crowded core, ~120 ms) leaves every thread that waits or wakes through that
// list spinning, and those spinners kept it off the CPU even longer. The kernel's own wait lists can't be held that way.
inline bool KernelFutex = false;

// Linux arm64 futex call, straight to the kernel.
inline int64_t KernelFutexCall(void* Address, int64_t Op, uint32_t Value, const void* Timeout, uint32_t Value3) {
  register int64_t X8 asm("x8") = 98; // __NR_futex
  register int64_t X0 asm("x0") = reinterpret_cast<int64_t>(Address);
  register int64_t X1 asm("x1") = Op;
  register int64_t X2 asm("x2") = Value;
  register int64_t X3 asm("x3") = reinterpret_cast<int64_t>(Timeout);
  register int64_t X4 asm("x4") = 0;
  register int64_t X5 asm("x5") = Value3;
  asm volatile("svc #0" : "+r"(X0) : "r"(X8), "r"(X1), "r"(X2), "r"(X3), "r"(X4), "r"(X5) : "memory", "cc");
  return X0;
}

constexpr int64_t KFUTEX_WAIT = 0;
constexpr int64_t KFUTEX_WAKE = 1;
constexpr int64_t KFUTEX_WAIT_BITSET = 9;
constexpr int64_t KFUTEX_WAKE_BITSET = 10;
constexpr int64_t KFUTEX_PRIVATE_FLAG = 128;
#endif

// Exclusive lock timings for one mutex, read and reset by the stats log. Durations and times are in cycle counter
// ticks; each "At" is the cycle counter when that longest one ended.
struct ContentionStats {
  std::atomic<uint64_t> Locks;         // exclusive locks taken
  std::atomic<uint64_t> SlowLocks;     // of those, how many waited 1ms or more
  std::atomic<uint64_t> PoliteGiveUps; // polite waits that ran out and queued the writer, blocking new readers
  std::atomic<uint64_t> MaxWait;
  std::atomic<uint64_t> MaxWaitAt;
  std::atomic<uint64_t> MaxHold;
  std::atomic<uint64_t> MaxHoldAt;
  std::atomic<uint64_t> ReaderSleeps; // shared locks that had to sleep (all of them counted, not only exclusive ones)
  std::atomic<uint64_t> WriterSleeps;
};

// A custom mutex that prioritizes exclusive locks.
// In highly contested scenarios, this can help minimize overall contention time.
//
// Features:
//  - Up to 32767 pending exclusive locks ("writers")
//  - Up to 32767 pending shared_locks ("readers")
//  - Low-overhead waiting via WFE with a fallback to futex on timeout
//  - Direct writer->reader hand-off and vice-versa to further reduce overhead
//
// Trade-offs:
//  - No guaranteed order of wake-ups besides prioritizing writers
//  - No support for recursive locking
//  - We can't use FUTEX_LOCK_PI to enable priority inheritance
class Mutex final {
public:
  Mutex() = default;

  // Move-only type
  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;
  Mutex(Mutex&& rhs) = delete;
  Mutex& operator=(Mutex&&) = delete;

  void lock() {
    // Try a non-blocking lock first.
    if (try_lock()) {
      return;
    }

#ifdef FEX_WRITE_PRIORITY_MUTEX_EXTRAS
    const uint64_t WaitBegin = Stats ? FEXCore::Utils::SpinWaitLock::GetCycleCounter() : 0;
    LockSlow();
    if (Stats) {
      NoteAcquired(WaitBegin);
    }
  }

private:
  void LockSlow() {
    // Try a quick WFE write-lock.
    if (Attempt_WFE_WriteLock()) {
      return;
    }

    // Wait without queueing first, so readers keep going while this writer waits for the current ones to finish.
    if (PoliteWaitCycles && Attempt_PoliteWriteLock()) {
      return;
    }
#else
    // Try a quick WFE write-lock.
    if (Attempt_WFE_WriteLock()) {
      return;
    }
#endif

    // Still couldn't get it. Start waiting.
    auto AtomicFutex = std::atomic_ref<uint32_t>(Futex);

    uint32_t Expected {};
    uint32_t Desired {};
#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
    Expected = AtomicFutex.load(std::memory_order_relaxed);
    do {
      // Increment the number of write waiters.
      Desired = Expected + WRITE_WAITER_INCREMENT;

      LOGMAN_THROW_A_FMT((Desired & WRITE_WAITER_COUNT_MASK) != 0, "Overflow in write-waiters!");
    } while (AtomicFutex.compare_exchange_strong(Expected, Desired, std::memory_order_acq_rel, std::memory_order_acquire) == false);
#else
    // Increment the number of writers waiting. The following loop will attempt to acquire the write-lock while decrementing the waiter count.
    Expected = AtomicFutex.fetch_add(WRITE_WAITER_INCREMENT);
    Desired = Expected + WRITE_WAITER_INCREMENT;
#endif

    // Thread added to waiter list.
    Expected = Desired;

    while (true) {
      bool Sleep = false;

      do {
        if ((Expected & WRITE_OWNED_BIT) == 0 && (Expected & READ_OWNER_COUNT_MASK) == 0) {
          // If not write-owned, and no read-owners, try to acquire.
          LOGMAN_THROW_A_FMT((Expected & WRITE_WAITER_COUNT_MASK) != 0, "Underflow in write-waiters!");

          // Add write-owned bit.
          Desired = Expected | WRITE_OWNED_BIT;

          // Remove ourselves from the wait list.
          Desired -= WRITE_WAITER_INCREMENT;

          Sleep = false;
        } else {
          // Already write-owned or read-locked. Go to sleep.
          Desired = Expected;
          Sleep = true;
          break;
        }

      } while (AtomicFutex.compare_exchange_strong(Expected, Desired, std::memory_order_acq_rel, std::memory_order_acquire) == false);

      if (!Sleep) {
        // Acquired early.
        LOGMAN_THROW_A_FMT((Desired & WRITE_OWNED_BIT) == WRITE_OWNED_BIT, "Somehow acquired a write-lock without it being set!");
        return;
      }

      // Two paths to get here.
      // Desired[31] = 1 (WRITE_OWNED_BIT)
      // OR
      // Desired[15:0] != 0 (READ_OWNER_COUNT_MASK)
      // Meaning that there was already a writer that owned the lock, or reads were owning it.
      // This thread already incremented `WRITE_WAITER_INCREMENT` before this loop.
      // - Linux waits for the full 32-bits to change (With bitset wakeup).
      // - Win32 also waits for the full 32-bits to change (with offset addr on the reader side to reduce stampeding).
      FutexWaitForWriteAvailable(Desired);

      Expected = AtomicFutex.load(std::memory_order_relaxed);
    }
  }

#ifdef FEX_WRITE_PRIORITY_MUTEX_EXTRAS
public:
#endif
  void lock_shared() {
    // Try an uncontended lock first.
    if (try_lock_shared()) {
      return;
    }

    // Try a quick WFE read-lock.
    if (Attempt_WFE_ReadLock()) {
      return;
    }

    auto AtomicFutex = std::atomic_ref<uint32_t>(Futex);

    uint32_t Expected = AtomicFutex.load(std::memory_order_relaxed);
    uint32_t Desired {};

    while (true) {
      bool Sleep = false;
      do {
        if ((Expected & WRITE_OWNED_BIT) == 0 && (Expected & WRITE_WAITER_COUNT_MASK) == 0) {
          // If no write-owner and no write-waiting, try and acquire.

          Desired = Expected + READ_OWNER_INCREMENT;
          LOGMAN_THROW_A_FMT((Desired & READ_OWNER_COUNT_MASK) != 0, "Overflow in read-owners!");
          Sleep = false;
        } else {
          // Waiting for lock to become available. Add to waiters.
          Desired = Expected | READ_WAITER_BIT;
          Sleep = true;
        }
      } while (AtomicFutex.compare_exchange_strong(Expected, Desired, std::memory_order_acq_rel, std::memory_order_acquire) == false);

      if (!Sleep) {
        // Acquired early.
        LOGMAN_THROW_A_FMT((Desired & WRITE_OWNED_BIT) != WRITE_OWNED_BIT, "Somehow read-locked and got a write lock!");
        return;
      }

      // Only one path to get here.
      // Desired[31][29:16] != 0 (Either writer-owned, or writer-waiting)
      // Desired[30][15:0]  == READ_WAIT_BIT and number of read-owners (draining to zero as write-side is set)
      // - Linux waits for full 32-bit futex.
      // - Win32 waits for upper 16-bits to not match (Either zero writer owned, writer-wait is draining, and `READ_WAITER_BIT` changed).
      // Can get some spurious wake-ups which will `or` the `READ_WAITER_BIT` again, which does nothing.
      FutexWaitForReadAvailable(Desired);

      Expected = AtomicFutex.load(std::memory_order_relaxed);
    }
  }

  void unlock() {
    auto AtomicFutex = std::atomic_ref<uint32_t>(Futex);

#ifdef FEX_WRITE_PRIORITY_MUTEX_EXTRAS
    if (Stats && HeldSince) {
      NoteReleased();
    }
#endif

    uint32_t Expected = AtomicFutex.load(std::memory_order_relaxed);
    uint32_t Desired {};
    do {
      LOGMAN_THROW_A_FMT((Expected & WRITE_OWNED_BIT) == WRITE_OWNED_BIT, "Trying to write-unlock something not write-locked!");
      // Remove the exclusive lock bit.
      Desired = Expected & ~WRITE_OWNED_BIT;

      // If no more writers, then make sure to clear the read-waiters bit as well.
      if ((Desired & WRITE_WAITER_COUNT_MASK) == 0) {
        Desired &= ~READ_WAITER_BIT;
      }
    } while (AtomicFutex.compare_exchange_strong(Expected, Desired, std::memory_order_acq_rel, std::memory_order_acquire) == false);

    // `Expected` has old value. Containing `READ_WAITER_BIT` which was just masked off, and also `WRITE_WAITER_COUNT_MASK`.
    //
    // Two paths here to be careful about dead-locking other waiters:
    // - If there are any writers waiting, those get priority to wake.
    // - If there are zero writers waiting, and there are read waiters then make sure to wake them all.
    // Failure to send wake events can cause readers to "infinitely" hang! (ignoring spurious wake-up).
    if ((Expected & WRITE_WAITER_COUNT_MASK)) {
      // Handle write-write handoff.
      FutexWakeWriter();
    } else if ((Expected & READ_WAITER_BIT)) {
      // Handle write-reader handoff.
      FutexWakeReaders();
    }

#ifdef FEX_WRITE_PRIORITY_MUTEX_EXTRAS
    WakePoliteWriters();
#endif
  }

  void unlock_shared() {
    auto AtomicFutex = std::atomic_ref<uint32_t>(Futex);

    uint32_t Desired {};
#if defined(ASSERTIONS_ENABLED) && ASSERTIONS_ENABLED
    uint32_t Expected = AtomicFutex.load(std::memory_order_relaxed);
    do {
      LOGMAN_THROW_A_FMT((Expected & WRITE_OWNED_BIT) != WRITE_OWNED_BIT, "Trying to read-unlock something write-locked!");
      LOGMAN_THROW_A_FMT((Expected & READ_OWNER_COUNT_MASK) != 0, "Trying to read-unlock something not read-locked!");

      // Decrement the shared counter.
      Desired = Expected - READ_OWNER_INCREMENT;
    } while (AtomicFutex.compare_exchange_strong(Expected, Desired, std::memory_order_acq_rel, std::memory_order_acquire) == false);
#else
    Desired = AtomicFutex.fetch_sub(READ_OWNER_INCREMENT) - READ_OWNER_INCREMENT;
#endif

    // Handle read->write handoff if there are any waiting writers, and no readers left.
    // Only one path here but still need to be careful to not dead-lock waiting writers.
    // - If there are waiters /but/ this is not the final unlock_shared, then don't wake writer.
    //   - Writer would wake and immediately sleep again if we woke on every unlock_shared.
    // - If there are waiters and this is the final unlock_shared, then wake a /single/ writer.
    // - We ignore any reader-waiters here as they must wait their turn for writers that are waiting.
    if ((Desired & WRITE_WAITER_COUNT_MASK) && (Desired & READ_OWNER_COUNT_MASK) == 0) {
      FutexWakeWriter();
    }

#ifdef FEX_WRITE_PRIORITY_MUTEX_EXTRAS
    if ((Desired & READ_OWNER_COUNT_MASK) == 0) {
      WakePoliteWriters();
    }
#endif
  }

  bool try_lock() {
    auto AtomicFutex = std::atomic_ref<uint32_t>(Futex);

    uint32_t Expected = 0;

    // Try and grab the owned bit.
    uint32_t Desired = WRITE_OWNED_BIT;

    // try to CAS immediately.
    const bool Acquired = AtomicFutex.compare_exchange_strong(Expected, Desired, std::memory_order_acq_rel, std::memory_order_acquire);
#ifdef FEX_WRITE_PRIORITY_MUTEX_EXTRAS
    if (Acquired && Stats) {
      NoteAcquired(0);
    }
#endif
    return Acquired;
  }

  // Can race with other threads trying to lock shared!
  bool try_lock_shared() {
    auto AtomicFutex = std::atomic_ref<uint32_t>(Futex);
    uint32_t Expected = AtomicFutex.load(std::memory_order_relaxed);

    // Exclusively owned or has a list of waiting owners. Can't pass.
    if ((Expected & WRITE_OWNED_BIT) || (Expected & WRITE_WAITER_COUNT_MASK)) {
      return false;
    }

    // Try to add reader.
    uint32_t Desired = Expected + READ_OWNER_INCREMENT;
    LOGMAN_THROW_A_FMT((Desired & READ_OWNER_COUNT_MASK) != 0, "Overflow in read-owners!");

    // Uncontended mutex check
    return AtomicFutex.compare_exchange_strong(Expected, Desired, std::memory_order_acq_rel, std::memory_order_acquire);
  }

#ifdef FEX_WRITE_PRIORITY_MUTEX_EXTRAS
  // How long a writer that can't get the lock right away waits without queueing. Queued writers block new readers,
  // so one slow reader plus a queued writer stops every other reader; a polite writer lets them carry on and takes
  // the lock the moment no reader holds it. After this long it queues as usual. 0 (the default) never waits politely.
  void SetPoliteWriteWait(uint32_t Milliseconds) {
    PoliteWaitCycles = FEXCore::Utils::SpinWaitLock::CycleCounterFrequency * Milliseconds / 1000;
  }

  // Exclusive lock timings go to Stats from now on (nullptr stops them). Stats must outlive the mutex's use.
  void SetContentionStats(ContentionStats* NewStats) {
    Stats = NewStats;
  }
#endif

#if !defined(_WIN32)
  // Initialize the internal mutex object to its default initializer state.
  // Should only ever be used in the child process when a Linux fork() has occured.
  void StealAndDropActiveLocks() {
    Futex = 0;
  }
#endif

private:

#if !defined(_WIN32)
  void FutexWaitForWriteAvailable(uint32_t Expected) {
    ::syscall(SYS_futex, &Futex, FUTEX_PRIVATE_FLAG | FUTEX_WAIT_BITSET, Expected, nullptr, nullptr, FUTEX_BITSET_WAIT_WRITERS);
  }

  // Read-lock waiting for writers to drain out.
  void FutexWaitForReadAvailable(uint32_t Expected) {
    ::syscall(SYS_futex, &Futex, FUTEX_PRIVATE_FLAG | FUTEX_WAIT_BITSET, Expected, nullptr, nullptr, FUTEX_BITSET_WAIT_READERS);
  }

  // Read-Lock or Write-lock unlocked, wake one writer.
  // - Read->Write handoff.
  // - Write->Write handoff.
  void FutexWakeWriter() {
    ::syscall(SYS_futex, &Futex, FUTEX_PRIVATE_FLAG | FUTEX_WAKE_BITSET, 1, nullptr, nullptr, FUTEX_BITSET_WAIT_WRITERS);
  }

  // Write-lock unlocked, wake read-locks waiting.
  void FutexWakeReaders() {
    // Wake all readers.
    ::syscall(SYS_futex, &Futex, FUTEX_PRIVATE_FLAG | FUTEX_WAKE_BITSET, INT_MAX, nullptr, nullptr, FUTEX_BITSET_WAIT_READERS);
  }
#else
  // Writers wait for the full 32-bit futex.
  void FutexWaitForWriteAvailable(uint32_t Expected) {
#ifdef FEX_WRITE_PRIORITY_MUTEX_EXTRAS
    if (Stats) {
      Stats->WriterSleeps.fetch_add(1, std::memory_order_relaxed);
    }
    if (KernelFutex) {
      // As on Linux: the whole word, writers' wake bit. A signal or a changed word just sends it round the loop again.
      KernelFutexCall(&Futex, KFUTEX_WAIT_BITSET | KFUTEX_PRIVATE_FLAG, Expected, nullptr, FUTEX_BITSET_WAIT_WRITERS);
      return;
    }
#endif
    WaitOnAddress(&Futex, &Expected, sizeof(Futex), INFINITE);
  }

  // Readers wait for Futex bits [31:16] to be zero.
  void FutexWaitForReadAvailable(uint32_t Expected) {
#ifdef FEX_WRITE_PRIORITY_MUTEX_EXTRAS
    if (Stats) {
      Stats->ReaderSleeps.fetch_add(1, std::memory_order_relaxed);
    }
    if (KernelFutex) {
      KernelFutexCall(&Futex, KFUTEX_WAIT_BITSET | KFUTEX_PRIVATE_FLAG, Expected, nullptr, FUTEX_BITSET_WAIT_READERS);
      return;
    }
#endif
    auto ReadWaiterAddress = reinterpret_cast<uint8_t*>(&Futex) + 2;
    uint16_t smol_Expected = Expected >> 16;
    WaitOnAddress(ReadWaiterAddress, &smol_Expected, sizeof(smol_Expected), INFINITE);
  }

  void FutexWakeWriter() {
#ifdef FEX_WRITE_PRIORITY_MUTEX_EXTRAS
    if (KernelFutex) {
      KernelFutexCall(&Futex, KFUTEX_WAKE_BITSET | KFUTEX_PRIVATE_FLAG, 1, nullptr, FUTEX_BITSET_WAIT_WRITERS);
      return;
    }
#endif
    WakeByAddressSingle(&Futex);
  }

  void FutexWakeReaders() {
#ifdef FEX_WRITE_PRIORITY_MUTEX_EXTRAS
    if (KernelFutex) {
      KernelFutexCall(&Futex, KFUTEX_WAKE_BITSET | KFUTEX_PRIVATE_FLAG, INT_MAX, nullptr, FUTEX_BITSET_WAIT_READERS);
      return;
    }
#endif
    auto ReadWaiterAddress = reinterpret_cast<uint8_t*>(&Futex) + 2;
    WakeByAddressAll(ReadWaiterAddress);
  }
#endif

  // Reuse the SpinWaitLock WFE implementations for read/write lock acquiring with WFE.
  // Can't reuse the spin-lock directly as some bit-representations are different.
  // WFE-write-lock is less likely to occur the more read-lock threads are participating. Can still occur so good to try.
  // WFE-read-lock is actually quite likely to succeed.
  // Return: true if the lock was acquired.
  bool Attempt_WFE_WriteLock() {
#ifdef ARCHITECTURE_arm64
    const auto Begin = FEXCore::Utils::SpinWaitLock::GetCycleCounter();
    auto Now = Begin;
    const auto Duration = FEXCore::Utils::SpinWaitLock::CycleCounterFrequency / CYCLECOUNT_DIVISOR;

    auto AtomicFutex = std::atomic_ref<uint32_t>(Futex);
    uint32_t Expected = AtomicFutex.load(std::memory_order_relaxed);

    while ((Now - Begin) < Duration) {
      if (Expected == 0) {
        // Try and grab the owned bit.
        uint32_t Desired = WRITE_OWNED_BIT;

        if (AtomicFutex.compare_exchange_strong(Expected, Desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
          return true;
        }
      }

      // One-shot attempt to wait for mask to be zero.
      Expected = FEXCore::Utils::SpinWaitLock::OneShotWFEBitComparison(&Futex, ~0U, 0U);
      Now = FEXCore::Utils::SpinWaitLock::GetCycleCounter();
    }
#endif

    return false;
  }

  // Return: true if the lock was acquired.
  bool Attempt_WFE_ReadLock() {
#ifdef ARCHITECTURE_arm64
    // Spin on a WFE for a short-amount of time, waiting for write-owned and writer-count to be zero.
    //  - Attempt to acquire read-lock at that point.
    //  - Don't add read-waiters bit on failure, return false.
    const auto Begin = FEXCore::Utils::SpinWaitLock::GetCycleCounter();
    auto Now = Begin;
    const auto Duration = FEXCore::Utils::SpinWaitLock::CycleCounterFrequency / CYCLECOUNT_DIVISOR;

    auto AtomicFutex = std::atomic_ref<uint32_t>(Futex);
    uint32_t Expected = AtomicFutex.load(std::memory_order_relaxed);
    uint32_t Desired {};

    while ((Now - Begin) < Duration) {
      if ((Expected & WRITE_OWNED_BIT) == 0 && (Expected & WRITE_WAITER_COUNT_MASK) == 0) {
        // If no write-owner and no write-waiting, try and acquire.

        Desired = Expected + READ_OWNER_INCREMENT;
        LOGMAN_THROW_A_FMT((Desired & READ_OWNER_COUNT_MASK) != 0, "Overflow in read-owners!");
        if (AtomicFutex.compare_exchange_strong(Expected, Desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
          return true;
        }
      }

      // One-shot attempt to wait for mask to be zero.
      Expected = FEXCore::Utils::SpinWaitLock::OneShotWFEBitComparison(&Futex, WRITE_OWNED_BIT | WRITE_WAITER_COUNT_MASK, 0U);
      Now = FEXCore::Utils::SpinWaitLock::GetCycleCounter();
    }
#endif

    return false;
  }

#ifdef FEX_WRITE_PRIORITY_MUTEX_EXTRAS
  // Return: true if the lock was acquired within PoliteWaitCycles, false to queue as usual.
  bool Attempt_PoliteWriteLock() {
    const auto Begin = FEXCore::Utils::SpinWaitLock::GetCycleCounter();
    const auto SpinCycles = FEXCore::Utils::SpinWaitLock::CycleCounterFrequency / POLITE_SPIN_DIVISOR;
    auto AtomicFutex = std::atomic_ref<uint32_t>(Futex);

    // Readers and writers that drop the lock check this count after releasing and wake PoliteWaitSequence if it's
    // non-zero. Both sides use sequentially consistent operations, so either this writer sees the lock free below or
    // the releasing thread sees this count and wakes it. Each sleep also ends after 1ms in case a wake is missed.
    std::atomic_ref<uint32_t>(PoliteWaiters).fetch_add(1);
    bool Acquired = false;
    while (true) {
      uint32_t Sequence = std::atomic_ref<uint32_t>(PoliteWaitSequence).load();
      uint32_t Expected = AtomicFutex.load();
      if ((Expected & (WRITE_OWNED_BIT | READ_OWNER_COUNT_MASK)) == 0) {
        // Free: take it without touching the other bits, so queued writers and waiting readers are still woken on unlock.
        if (AtomicFutex.compare_exchange_strong(Expected, Expected | WRITE_OWNED_BIT, std::memory_order_acq_rel, std::memory_order_acquire)) {
          Acquired = true;
          break;
        }
        continue;
      }

      const auto Waited = FEXCore::Utils::SpinWaitLock::GetCycleCounter() - Begin;
      if (Waited >= PoliteWaitCycles) {
        break;
      }
      if (Waited < SpinCycles) {
        // Most reader holds are short: wait for the lock word to change.
        FEXCore::Utils::SpinWaitLock::OneShotWFEBitComparison(&Futex, WRITE_OWNED_BIT | READ_OWNER_COUNT_MASK, 0U);
      } else if (KernelFutex) {
        struct {
          int64_t Seconds;
          int64_t Nanoseconds;
        } OneMs {0, 1'000'000};
        KernelFutexCall(&PoliteWaitSequence, KFUTEX_WAIT | KFUTEX_PRIVATE_FLAG, Sequence, &OneMs, 0);
      } else {
        WaitOnAddress(&PoliteWaitSequence, &Sequence, sizeof(Sequence), 1);
      }
    }
    std::atomic_ref<uint32_t>(PoliteWaiters).fetch_sub(1);

    if (!Acquired && Stats) {
      Stats->PoliteGiveUps.fetch_add(1, std::memory_order_relaxed);
    }
    return Acquired;
  }

  void WakePoliteWriters() {
    if (std::atomic_ref<uint32_t>(PoliteWaiters).load() != 0) {
      std::atomic_ref<uint32_t>(PoliteWaitSequence).fetch_add(1);
      if (KernelFutex) {
        KernelFutexCall(&PoliteWaitSequence, KFUTEX_WAKE | KFUTEX_PRIVATE_FLAG, INT_MAX, nullptr, 0);
      } else {
        WakeByAddressAll(&PoliteWaitSequence);
      }
    }
  }

  void NoteAcquired(uint64_t WaitBegin) {
    const auto Now = FEXCore::Utils::SpinWaitLock::GetCycleCounter();
    HeldSince = Now;
    Stats->Locks.fetch_add(1, std::memory_order_relaxed);
    if (!WaitBegin) {
      return;
    }
    const auto Waited = Now - WaitBegin;
    if (Waited >= FEXCore::Utils::SpinWaitLock::CycleCounterFrequency / 1000) {
      Stats->SlowLocks.fetch_add(1, std::memory_order_relaxed);
    }
    if (Waited > Stats->MaxWait.load(std::memory_order_relaxed)) {
      Stats->MaxWait.store(Waited, std::memory_order_relaxed);
      Stats->MaxWaitAt.store(Now, std::memory_order_relaxed);
    }
  }

  void NoteReleased() {
    const auto Now = FEXCore::Utils::SpinWaitLock::GetCycleCounter();
    const auto Held = Now - HeldSince;
    HeldSince = 0;
    if (Held > Stats->MaxHold.load(std::memory_order_relaxed)) {
      Stats->MaxHold.store(Held, std::memory_order_relaxed);
      Stats->MaxHoldAt.store(Now, std::memory_order_relaxed);
    }
  }

  // Polite writers spin on WFE for the first 0.2ms (200k ns), then sleep in 1ms steps.
  constexpr static uint64_t POLITE_SPIN_DIVISOR = 1'000'000'000ULL / 200'000U;

  uint64_t PoliteWaitCycles {};
  ContentionStats* Stats {};
  // Written only by the thread holding the exclusive lock.
  uint64_t HeldSince {};
  uint32_t PoliteWaiters {};
  uint32_t PoliteWaitSequence {};
#endif

  constexpr static uint32_t WRITE_OWNED_BIT = 1U << 31;
  constexpr static uint32_t READ_WAITER_BIT = 1U << 30;
  constexpr static uint32_t WRITE_WAITER_OFFSET = 16;
  constexpr static uint32_t WRITE_WAITER_INCREMENT = 1U << WRITE_WAITER_OFFSET;
  constexpr static uint32_t READ_OWNER_INCREMENT = 1;

  // Count masks
  constexpr static uint32_t WRITE_WAITER_COUNT_MASK = 0x3FFFU << WRITE_WAITER_OFFSET;
  constexpr static uint32_t READ_OWNER_COUNT_MASK = 0xFFFFU;

  // Independent futex bit-set masks.
  // Wait for readers to drain.
  constexpr static uint32_t FUTEX_BITSET_WAIT_READERS = 1U << 0;
  // Wait for writers to drain.
  constexpr static uint32_t FUTEX_BITSET_WAIT_WRITERS = 1U << 1;

  // Only spin on WFE for 0.01ms (10k ns).
  constexpr static uint64_t CYCLECOUNT_DIVISOR = 1'000'000'000ULL / 10'000U;

  // Layout:
  //    Bits[31]: Write-lock bit.
  //    Bits[30]: Read-waiter bit.
  // Bits[29:16]: Write-waiter count.
  //  Bits[15:0]: Read-owner count.
  uint32_t Futex {};
};
} // namespace FEXCore::Utils::WritePriorityMutex
