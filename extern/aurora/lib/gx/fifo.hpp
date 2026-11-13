#pragma once

#include "../internal.hpp"

#include <cstring>
#include <functional>

namespace aurora::gx::fifo {

namespace detail {
extern uint8_t* sBufferData;
extern uint32_t sBufferSize;
extern uint32_t sBufferCapacity;
extern bool sInDisplayList;
extern uint8_t* sDlBuffer;
extern uint32_t sDlSize;
extern uint32_t sDlWritePos;
} // namespace detail

enum class ProcessingMode : uint8_t {
  // Process FIFO synchronously at drain()
  Drain,
  // Process FIFO synchronously at publish() (useful for profiling)
  Inline,
  // Process FIFO asynchronously on a worker thread; drain() synchronizes
  Thread
};
ProcessingMode processing_mode() noexcept;
// The FIFO processor thread's kernel id where the platform has one (Android), else 0.
uint32_t native_thread_id() noexcept;

void init();
void shutdown();

void begin_frame() noexcept;
void end_frame() noexcept;
// The FIFO processor's part of end_frame (its draw cache), for a seal_async tail; seal_async
// itself ends the producer's frame.
void end_frame_processor_state() noexcept;

// Whether a frame is currently being recorded, i.e. publish() will be honored.
bool frame_active() noexcept;

// Out-of-line slow path: grows internal buffer then appends data
void write_data_grow(const void* data, uint32_t length);

inline void write_data(const void* data, const uint32_t length) {
  if (!detail::sInDisplayList)
    LIKELY {
      if (length <= detail::sBufferCapacity - detail::sBufferSize)
        LIKELY {
          std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
          detail::sBufferSize += length;
          return;
        }
      write_data_grow(data, length);
    }
  else if (length <= detail::sDlSize - detail::sDlWritePos) {
    std::memcpy(detail::sDlBuffer + detail::sDlWritePos, data, length);
    detail::sDlWritePos += length;
  }
}

inline void write_u8(const uint8_t val) {
  if (!detail::sInDisplayList)
    LIKELY {
      if (detail::sBufferSize < detail::sBufferCapacity)
        LIKELY {
          detail::sBufferData[detail::sBufferSize++] = val;
          return;
        }
      write_data_grow(&val, 1);
    }
  else if (detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = val;
  }
}

inline void write_u16(const uint16_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_u32(const uint32_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_u64(const uint64_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_f32(const float val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

// Overwrites an unpublished u32 previously written at the given offset.
void patch_u32(uint32_t offset, uint32_t val);

// Marks a complete draw and publishes when the configured draw batch is full.
void finish_draw() noexcept;

// Makes commands written so far available to the FIFO processor.
void publish() noexcept;

using DrawDoneCallback = void (*)();
DrawDoneCallback set_draw_done_callback(DrawDoneCallback callback) noexcept;

// Display list recording
void begin_display_list(uint8_t* buf, uint32_t size);
uint32_t end_display_list();
bool in_display_list();

// Ensure all buffered commands have been processed (after the frame tail of seal_async).
void drain();

// PF2_ASYNC_SEAL: ends the frame without waiting for the FIFO processor. Everything written
// so far is published and `tail` (the rest of the frame's end: finish, end_frame) runs on the
// FIFO processor right after the last of it is processed, while the producer goes on. Commands
// written meanwhile stay unpublished until the next frame. join() waits for the tail; begin of
// the next frame, drain(), gfx::synchronize and shutdown join first.
void seal_async(std::function<void()> tail) noexcept;
// A tail queued by seal_async and not yet joined.
bool seal_pending() noexcept;
// Waits for a queued frame tail (no-op without one, or on the FIFO processor itself).
void join() noexcept;
// Since the last call: joined tails, how many and how long the producer waited for one, and
// the time from seal to tail done (all ns).
struct JoinStats {
  uint32_t joins = 0;
  uint32_t waited = 0;
  int64_t waitedNs = 0;
  int64_t maxWaitNs = 0;
  int64_t sealToDoneNs = 0;
  int64_t maxSealToDoneNs = 0;
};
JoinStats take_join_stats() noexcept;

// Internal buffer inspection
const uint8_t* get_buffer_data();
uint32_t get_buffer_size();
void clear_buffer();

} // namespace aurora::gx::fifo
