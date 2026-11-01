#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// Detects a step that never returns. The orchestrator's recovery path tears
// down and rebuilds the capture pipeline; if a GStreamer thread spins, that
// teardown can block forever, and then nothing else in the engine notices
// (the one thread that would is the one stuck). A watchdog thread that does not
// share the stuck thread's fate can: arm it before a step, disarm it after, and
// if the step overruns its limit the stall handler runs.
//
// The engine's handler logs and exits the process, so the watcher restarts it;
// the tests' handler just records the call. Header-only and free of engine
// dependencies so the timing behavior is unit tested on the host.
namespace stall_watchdog {

class StallWatchdog {
 public:
  using StallHandler = std::function<void(const std::string& phase, std::chrono::milliseconds limit)>;

  explicit StallWatchdog(StallHandler on_stall)
      : on_stall_(std::move(on_stall)), thread_([this] { run(); }) {}

  ~StallWatchdog() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    changed_.notify_all();
    thread_.join();
  }

  StallWatchdog(const StallWatchdog&) = delete;
  StallWatchdog& operator=(const StallWatchdog&) = delete;

  // Starts timing a phase. If disarm() is not called within limit, the handler
  // runs once, on the watchdog thread. Arming while a phase is already armed
  // replaces it.
  void arm(std::string phase, std::chrono::milliseconds limit) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      phase_ = std::move(phase);
      limit_ = limit;
      deadline_ = std::chrono::steady_clock::now() + limit;
      armed_ = true;
      ++generation_;
    }
    changed_.notify_all();
  }

  // The armed phase finished in time.
  void disarm() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      armed_ = false;
      ++generation_;
    }
    changed_.notify_all();
  }

 private:
  void run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
      if (!armed_) {
        changed_.wait(lock, [this] { return stopping_ || armed_; });
        continue;
      }
      const uint64_t watched = generation_;
      const auto deadline = deadline_;
      // True when the phase was disarmed or replaced, or we are stopping;
      // false only when the deadline passed with the same phase still armed.
      if (changed_.wait_until(lock, deadline, [&] { return stopping_ || generation_ != watched; })) {
        continue;
      }
      const std::string phase = phase_;
      const auto limit = limit_;
      armed_ = false;  // report a stall once, not on every wake-up
      lock.unlock();
      on_stall_(phase, limit);
      lock.lock();
    }
  }

  StallHandler on_stall_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool armed_{false};
  bool stopping_{false};
  std::string phase_;
  std::chrono::milliseconds limit_{0};
  std::chrono::steady_clock::time_point deadline_;
  uint64_t generation_{0};
  // Declared last: the thread starts in the constructor and must see every
  // member above already initialized.
  std::thread thread_;
};

// Arms a phase for the lifetime of a scope, so every exit path (including early
// returns) disarms it.
class ScopedStallPhase {
 public:
  ScopedStallPhase(StallWatchdog& watchdog, std::string phase, std::chrono::milliseconds limit)
      : watchdog_(watchdog) {
    watchdog_.arm(std::move(phase), limit);
  }
  ~ScopedStallPhase() { watchdog_.disarm(); }

  ScopedStallPhase(const ScopedStallPhase&) = delete;
  ScopedStallPhase& operator=(const ScopedStallPhase&) = delete;

 private:
  StallWatchdog& watchdog_;
};

}  // namespace stall_watchdog
