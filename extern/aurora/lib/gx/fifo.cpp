#include "fifo.hpp"
#include "../gfx/perf_counters.hpp"

#include "../thread.hpp"
#include "command_processor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

#include <tracy/Tracy.hpp>

#if defined(__ANDROID__)
#include <unistd.h>
#endif

namespace aurora::gx::fifo {
namespace detail {
uint8_t* sBufferData = nullptr;
uint32_t sBufferSize = 0;
uint32_t sBufferCapacity = 0;
bool sInDisplayList = false;
uint8_t* sDlBuffer = nullptr;
uint32_t sDlSize = 0;
uint32_t sDlWritePos = 0;
} // namespace detail

namespace {
constexpr Module Log{"aurora::gx::fifo"};
constexpr auto kProcessingMode = ProcessingMode::Thread;
constexpr uint32_t kDrawBatchSize = 1;

bool sFrameActive = false;
uint32_t sPendingDraws = 0;
std::atomic<uint64_t> sPublished{0};
std::atomic<uint64_t> sProcessed{0};
uint64_t sStreamBase = 0;
std::mutex sBufferMutex;
std::atomic<uint32_t> sWorkerWake{0};
thread::Thread sWorkerThread;
// The kernel's id for the worker, for an OpenXR runtime's scheduling hints.
std::atomic<uint32_t> sWorkerNativeId{0};
std::atomic<DrawDoneCallback> sDrawDoneCallback{nullptr};
// The FIFO processor's own thread (drain and join are no-ops there).
thread_local bool tOnWorker = false;

// PF2_ASYNC_SEAL (seal_async): the frame's tail, run by the FIFO processor once the stream
// has been processed up to sTailTarget. sTail and sTailTarget are written by the producer
// before sTailState goes to Queued (release) and read by the worker after it sees Queued.
enum TailState : uint32_t { TailNone = 0, TailQueued = 1, TailDone = 2 };
std::function<void()> sTail;
uint64_t sTailTarget = 0;
std::atomic<uint32_t> sTailState{TailNone};
// The thread that queued it (the game thread); join() waits only there.
std::atomic<std::thread::id> sProducerThread{};
// Diagnostics: when the tail was queued and when it finished (steady ns), and how long
// the producer blocked in join() for it.
std::atomic<int64_t> sTailQueuedNs{0};
std::atomic<int64_t> sTailDoneNs{0};
JoinStats sJoinStats;

int64_t steady_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Runs the queued tail once everything before it is processed (worker thread).
bool run_tail_if_due(uint64_t processed) noexcept {
  if (sTailState.load(std::memory_order_acquire) != TailQueued || processed < sTailTarget) {
    return false;
  }
  auto tail = std::move(sTail);
  sTail = nullptr;
  if (tail) {
    tail();
  }
  sTailDoneNs.store(steady_ns(), std::memory_order_relaxed);
  sTailState.store(TailDone, std::memory_order_release);
  sTailState.notify_all();
  return true;
}

void dispatch_draw_done() noexcept {
  if (const auto callback = sDrawDoneCallback.load(std::memory_order_acquire); callback != nullptr) {
    callback();
  }
}

void wake_worker() noexcept {
  sWorkerWake.fetch_add(1, std::memory_order_release);
  sWorkerWake.notify_all();
}

void process_to(uint64_t target, std::memory_order order) noexcept {
  uint64_t processed = sProcessed.load(std::memory_order_relaxed);
  while (processed < target) {
    ProcessResult result{};
    {
      std::lock_guard lock{sBufferMutex};
      // The producer may already be appending the next batch. Only its atomic
      // published watermark is shared with us; sBufferSize is producer-owned.
      const uint64_t published = sPublished.load(std::memory_order_acquire);
      AURORA_ASSERT(processed >= sStreamBase && target <= published,
                    "FIFO processing range [{}, {}) is outside published range [{}, {})", processed, target, sStreamBase,
                    published);
      const auto start = static_cast<uint32_t>(processed - sStreamBase);
      const auto size = static_cast<uint32_t>(target - processed);
      {
        const gfx::perf::Timer timer{gfx::perf::g_fifoNs};
        result = process(detail::sBufferData + start, size);
      }
      if (gfx::perf::enabled()) {
        gfx::perf::g_fifoBytes.fetch_add(result.bytesProcessed, std::memory_order_relaxed);
      }
    }
    AURORA_ASSERT(result.bytesProcessed > 0 && result.bytesProcessed <= target - processed,
                  "FIFO processor made invalid progress: processed {} of {} remaining bytes", result.bytesProcessed,
                  target - processed);
    if (result.drawDone) {
      dispatch_draw_done();
    }
    processed += result.bytesProcessed;
    sProcessed.store(processed, order);
    sProcessed.notify_all();
  }
}

void worker_main(std::stop_token token) noexcept {
  std::stop_callback wakeOnStop{token, wake_worker};
  tOnWorker = true;
#if defined(__ANDROID__)
  sWorkerNativeId.store(static_cast<uint32_t>(gettid()), std::memory_order_release);
#endif
  while (true) {
    const uint32_t event = sWorkerWake.load(std::memory_order_acquire);
    const uint64_t processed = sProcessed.load(std::memory_order_relaxed);
    const uint64_t published = sPublished.load(std::memory_order_acquire);
    if (published != processed) {
      process_to(published, std::memory_order_release);
      continue;
    }
    if (run_tail_if_due(processed)) {
      continue;
    }

    if (token.stop_requested()) {
      break;
    }
    sWorkerWake.wait(event, std::memory_order_acquire);
  }
}

void start_worker() {
  if (kProcessingMode != ProcessingMode::Thread || sWorkerThread.joinable()) {
    return;
  }
  sWorkerThread = thread::Thread{{
                                     .name = "Aurora FIFO processor",
                                     .affinity = thread::Affinity::SharedCache,
                                 },
                                 worker_main};
}

void stop_worker() {
  if (!sWorkerThread.joinable()) {
    return;
  }
  join();
  sWorkerThread.request_stop();
  sWorkerThread.join();
  sWorkerNativeId.store(0, std::memory_order_release);
}
} // namespace

ProcessingMode processing_mode() noexcept { return kProcessingMode; }

uint32_t native_thread_id() noexcept { return sWorkerNativeId.load(std::memory_order_acquire); }

void init() {
  stop_worker();
  reset_draw_cache();

  constexpr uint32_t initialCapacity = 64 * 1024;
  free(detail::sBufferData);
  detail::sBufferData = static_cast<uint8_t*>(malloc(initialCapacity));
  AURORA_ASSERT(detail::sBufferData != nullptr, "fifo::init: failed to allocate {} bytes", initialCapacity);
  detail::sBufferSize = 0;
  detail::sBufferCapacity = initialCapacity;
  detail::sInDisplayList = false;
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;

  sFrameActive = false;
  sPendingDraws = 0;
  sStreamBase = 0;
  sPublished.store(0, std::memory_order_relaxed);
  sProcessed.store(0, std::memory_order_relaxed);
  sWorkerWake.store(0, std::memory_order_relaxed);

  start_worker();
}

void shutdown() {
  stop_worker();
  reset_draw_cache();
}

void begin_frame() noexcept { sFrameActive = true; }

void end_frame() noexcept {
  sFrameActive = false;
  clear_draw_cache(); // command_processor
}

void end_frame_processor_state() noexcept { clear_draw_cache(); }

bool frame_active() noexcept { return sFrameActive; }

void write_data_grow(const void* data, uint32_t length) {
  const uint64_t needed64 = static_cast<uint64_t>(detail::sBufferSize) + length;
  AURORA_ASSERT(needed64 <= std::numeric_limits<uint32_t>::max(), "fifo::write_data: buffer size overflow");
  const auto needed = static_cast<uint32_t>(needed64);
  const auto doubledCapacity = static_cast<uint64_t>(detail::sBufferCapacity) * 2;
  const auto newCapacity = static_cast<uint32_t>(
      std::min<uint64_t>(std::max(doubledCapacity, needed64), std::numeric_limits<uint32_t>::max()));
  const auto grow = [newCapacity] {
    auto* resized = static_cast<uint8_t*>(realloc(detail::sBufferData, newCapacity));
    AURORA_ASSERT(resized != nullptr, "fifo::write_data: failed to allocate {} bytes", newCapacity);
    detail::sBufferData = resized;
  };
  if (sWorkerThread.joinable()) {
    std::lock_guard lock{sBufferMutex};
    grow();
  } else {
    grow();
  }
  std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
  detail::sBufferSize = needed;
  detail::sBufferCapacity = newCapacity;
}

void publish() noexcept {
  if (!sFrameActive || kProcessingMode == ProcessingMode::Drain || detail::sInDisplayList) {
    return;
  }

  const uint64_t target = sStreamBase + detail::sBufferSize;
  if (target > sPublished.load(std::memory_order_relaxed)) {
    sPendingDraws = 0;
    sPublished.store(target, std::memory_order_release);
    if (kProcessingMode == ProcessingMode::Thread) {
      wake_worker();
    } else {
      process_to(target, std::memory_order_relaxed);
    }
  }
}

DrawDoneCallback set_draw_done_callback(DrawDoneCallback callback) noexcept {
  return sDrawDoneCallback.exchange(callback, std::memory_order_acq_rel);
}

void finish_draw() noexcept {
  if (!sFrameActive || kProcessingMode == ProcessingMode::Drain || detail::sInDisplayList) {
    return;
  }
  if (++sPendingDraws >= kDrawBatchSize) {
    publish();
  }
}

void patch_u32(uint32_t offset, uint32_t val) {
  AURORA_ASSERT(!detail::sInDisplayList && offset <= detail::sBufferSize &&
                    sizeof(uint32_t) <= detail::sBufferSize - offset,
                "fifo::patch_u32: invalid patch offset {} (buffer size {})", offset, detail::sBufferSize);
  AURORA_ASSERT(sStreamBase + offset >= sPublished.load(std::memory_order_relaxed),
                "fifo::patch_u32: offset {} is below the published watermark", offset);
  const auto out = bswap(val);
  std::memcpy(detail::sBufferData + offset, &out, sizeof(out));
}

void begin_display_list(uint8_t* buf, uint32_t size) {
  detail::sInDisplayList = true;
  detail::sDlBuffer = buf;
  detail::sDlSize = size;
  detail::sDlWritePos = 0;
}

uint32_t end_display_list() {
  detail::sInDisplayList = false;
  const uint32_t bytesWritten = detail::sDlWritePos;
  const uint32_t padded = (bytesWritten + 31) & ~31u;
  while (detail::sDlWritePos < padded && detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = 0;
  }
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
  return padded;
}

bool in_display_list() { return detail::sInDisplayList; }

void drain() {
  if (tOnWorker) {
    // A frame tail (seal_async) runs here after everything before it was processed.
    return;
  }
  join();
  if (detail::sBufferSize == 0) {
    return;
  }

  ZoneScoped;
  const uint64_t target = sStreamBase + detail::sBufferSize;

  switch (kProcessingMode) {
  case ProcessingMode::Drain:
  case ProcessingMode::Inline:
    sPublished.store(target, std::memory_order_relaxed);
    process_to(target, std::memory_order_relaxed);
    break;
  case ProcessingMode::Thread: {
    sPublished.store(target, std::memory_order_release);
    wake_worker();

    if (gfx::perf::enabled()) {
      gfx::perf::g_drainCalls.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t processed = sProcessed.load(std::memory_order_acquire);
    if (processed < target) {
      const gfx::perf::Timer timer{gfx::perf::g_drainWaitNs};
      do {
        sProcessed.wait(processed, std::memory_order_acquire);
        processed = sProcessed.load(std::memory_order_acquire);
      } while (processed < target);
    }
    break;
  }
  }

  {
    std::lock_guard lock{sBufferMutex};
    sStreamBase = target;
    detail::sBufferSize = 0;
  }
  sPendingDraws = 0;
}

void seal_async(std::function<void()> tail) noexcept {
  AURORA_ASSERT(kProcessingMode == ProcessingMode::Thread && sWorkerThread.joinable() && !tOnWorker,
                "fifo::seal_async needs the FIFO processor thread");
  join();
  sProducerThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
  sFrameActive = false;
  const uint64_t target = sStreamBase + detail::sBufferSize;
  sTail = std::move(tail);
  sTailTarget = target;
  sTailQueuedNs.store(steady_ns(), std::memory_order_relaxed);
  sTailState.store(TailQueued, std::memory_order_release);
  sPendingDraws = 0;
  sPublished.store(target, std::memory_order_release);
  wake_worker();
}

bool seal_pending() noexcept { return sTailState.load(std::memory_order_acquire) != TailNone; }

void join() noexcept {
  // Other threads (the render worker, the pacing thread) never wait for it: the tail may itself
  // wait for the render worker's queue, and only the producer owns the buffer.
  if (tOnWorker || sTailState.load(std::memory_order_acquire) == TailNone ||
      sProducerThread.load(std::memory_order_relaxed) != std::this_thread::get_id()) {
    return;
  }
  uint32_t state = sTailState.load(std::memory_order_acquire);
  int64_t waitedNs = 0;
  if (state == TailQueued) {
    const int64_t start = steady_ns();
    do {
      sTailState.wait(state, std::memory_order_acquire);
      state = sTailState.load(std::memory_order_acquire);
    } while (state == TailQueued);
    waitedNs = steady_ns() - start;
  }
  // The stream up to the tail is processed: what the producer wrote since (commands issued
  // between frames) moves to the front of the buffer.
  {
    std::lock_guard lock{sBufferMutex};
    const auto consumed = static_cast<uint32_t>(sTailTarget - sStreamBase);
    const uint32_t rest = detail::sBufferSize - consumed;
    if (rest != 0 && consumed != 0) {
      std::memmove(detail::sBufferData, detail::sBufferData + consumed, rest);
    }
    sStreamBase = sTailTarget;
    detail::sBufferSize = rest;
  }
  sJoinStats.joins += 1;
  sJoinStats.waitedNs += waitedNs;
  sJoinStats.maxWaitNs = std::max(sJoinStats.maxWaitNs, waitedNs);
  sJoinStats.waited += waitedNs > 0 ? 1 : 0;
  const int64_t queued = sTailQueuedNs.load(std::memory_order_relaxed);
  const int64_t done = sTailDoneNs.load(std::memory_order_relaxed);
  if (done > queued) {
    sJoinStats.sealToDoneNs += done - queued;
    sJoinStats.maxSealToDoneNs = std::max(sJoinStats.maxSealToDoneNs, done - queued);
  }
  sTailState.store(TailNone, std::memory_order_release);
}

JoinStats take_join_stats() noexcept { return std::exchange(sJoinStats, JoinStats{}); }

const uint8_t* get_buffer_data() { return detail::sBufferData; }
uint32_t get_buffer_size() { return detail::sBufferSize; }

void clear_buffer() {
  const uint64_t processed = sProcessed.load(std::memory_order_acquire);
  AURORA_ASSERT(sPublished.load(std::memory_order_acquire) == processed,
                "fifo::clear_buffer: published commands are still pending");
  std::lock_guard lock{sBufferMutex};
  sStreamBase = processed;
  detail::sBufferSize = 0;
  sPendingDraws = 0;
}

} // namespace aurora::gx::fifo
