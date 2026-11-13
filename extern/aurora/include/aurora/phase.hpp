#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

// "What is this thread doing", for the port's hang watchdog (platform/port_watchdog.cpp).
// Two slots, the main thread and the render worker, each a string literal and the time it
// was set. Lock-free and cheap enough to call around anything that can block in a driver;
// readers see a name and a time that may belong to neighbouring updates, which is fine.
namespace aurora::phase {

enum Slot : int { Main = 0, Render = 1, SlotCount = 2 };

struct State {
  std::atomic<const char*> name{"startup"};
  std::atomic<int64_t> sinceNs{0};
};

inline State& state(Slot slot) noexcept {
  static State s_states[SlotCount];
  return s_states[slot];
}

inline int64_t now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

namespace detail {
// PF2_PHASE_LOG=1 (diagnostics): the main thread's time per phase, logged by Aurora every few
// seconds (aurora.cpp). Off: one relaxed load per set().
inline std::atomic<bool>& accounting() noexcept {
  static std::atomic<bool> s_on{false};
  return s_on;
}
void account(const char* name, int64_t sinceNs, int64_t nowNs) noexcept;
// The game thread (marked when accounting starts): other threads' sets of the main slot (the FIFO
// processor's pipeline compiles) are left out while accounting, so they do not split its phases.
inline bool& on_main_thread() noexcept {
  thread_local bool s_main = false;
  return s_main;
}
} // namespace detail

// `name` must outlive the process (a string literal).
inline void set(Slot slot, const char* name) noexcept {
  State& s = state(slot);
  const int64_t now = now_ns();
  if (slot == Main && detail::accounting().load(std::memory_order_relaxed)) {
    if (!detail::on_main_thread()) {
      return;
    }
    detail::account(s.name.load(std::memory_order_relaxed), s.sinceNs.load(std::memory_order_relaxed), now);
  }
  s.name.store(name, std::memory_order_relaxed);
  s.sinceNs.store(now, std::memory_order_relaxed);
}

// Sets a phase and puts the previous one back on exit.
class Scope {
public:
  Scope(Slot slot, const char* name) noexcept : m_slot(slot), m_previous(state(slot).name.load(std::memory_order_relaxed)) {
    set(slot, name);
  }
  ~Scope() { set(m_slot, m_previous); }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

private:
  Slot m_slot;
  const char* m_previous;
};

} // namespace aurora::phase
