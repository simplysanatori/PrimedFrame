// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(MP_ENABLE_OPENXR) && (defined(__ANDROID__) || defined(_WIN32) || defined(__linux__))

// OpenXR's Vulkan structures are selected when openxr_platform.h is parsed. The Vulkan binding
// itself needs no window-system types, so the PC defines none.
#define XR_USE_GRAPHICS_API_VULKAN
#if defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR
#define XR_USE_PLATFORM_ANDROID
#elif !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "vr/openxr_vulkan_direct.h"
#include "vr/openxr_diagnostics.h"
#include "vr/openxr_eye_resize.h"
#include "vr/openxr_passthrough.h"
#include "vr/pf2_switches.h"
#if defined(__linux__) && !defined(__ANDROID__)
#define PF2_STEAMVR_READER 1
#include "vr/steamvr_frame_timing.h"
#endif
#include "vr/vr_settings.h"

#include <aurora/vulkan_direct_interop.h>

#if defined(__ANDROID__)
#include <jni.h>
#endif
#include <vulkan/vulkan.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace PortVr {
namespace {

bool SameVkCopyFamily(VkFormat a, VkFormat b) noexcept {
    return a == b ||
           ((a == VK_FORMAT_R8G8B8A8_UNORM || a == VK_FORMAT_R8G8B8A8_SRGB) &&
            (b == VK_FORMAT_R8G8B8A8_UNORM || b == VK_FORMAT_R8G8B8A8_SRGB)) ||
           ((a == VK_FORMAT_B8G8R8A8_UNORM || a == VK_FORMAT_B8G8R8A8_SRGB) &&
            (b == VK_FORMAT_B8G8R8A8_UNORM || b == VK_FORMAT_B8G8R8A8_SRGB));
}

VkFormat SrgbSibling(VkFormat format) noexcept {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return VK_FORMAT_B8G8R8A8_SRGB;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

const char* BeginStatusOperation(OpenXRFrameStatus status) noexcept {
    switch (status) {
    case OpenXRFrameStatus::Ready:
        return "ready";
    case OpenXRFrameStatus::SessionNotRunning:
        return "session is not running";
    case OpenXRFrameStatus::ExitRequested:
        return "runtime requested exit";
    case OpenXRFrameStatus::Error:
        return "xrWaitFrame failed";
    }
    return "unknown frame status";
}

OpenXRBeginStatus BeginStatusFor(OpenXRFrameStatus status) noexcept {
    return status == OpenXRFrameStatus::SessionNotRunning ? OpenXRBeginStatus::SessionNotRunning
           : status == OpenXRFrameStatus::ExitRequested  ? OpenXRBeginStatus::ExitRequested
                                                          : OpenXRBeginStatus::Error;
}

uint32_t PresentationTargetCount(const OpenXRPresentation& presentation) noexcept {
    return presentation.mode == OpenXRFrameMode::VirtualScreen ? 1u : kOpenXREyeCount;
}

#if defined(__ANDROID__)
const OpenXRVulkanGraphicsRequirements kNoRequirements{};
const std::string kNoBackend = "the Quest's OpenXR Vulkan backend was not set up";
constexpr const char* kPatchedDawnScript = "quest/Build-QuestDawn.ps1";
#elif defined(_WIN32)
constexpr const char* kPatchedDawnScript = "tools/Build-WindowsDawn.ps1";
#else
constexpr const char* kPatchedDawnScript = "tools/build-linux-dawn.sh";
#endif

// PF2_RELEASE_FENCE: when the GPU finished each eye set. Aurora's submitted callback (its frame
// worker, right after the frame's queue submission and the images' release barrier) makes an empty
// submission on Dawn's queue, under Dawn's device lock, that signals the next value of a timeline
// semaphore: a semaphore signal completes only after all work submitted before it. The XR thread
// waits for that value before xrReleaseSwapchainImage. A watcher thread records each set's time
// from that signal to its completion (the "GPU per set" figure of the [pf2] line). Without
// timeline semaphores the release drains the queue instead (correct, slower).
//
// PF2_DEV_GPU_LOAD_MB=<MB> (development only, for the mock): the signal's submission also fills
// that many MB of a scratch buffer first, so every eye set costs the GPU that much more (a fixed
// extra cost per set, measured in the [pf2] line's gpu/set like the rest).
class GpuFence {
public:
    enum class Mode { Off, Timeline, Drain };

    ~GpuFence() { Stop(); }

    Mode Start(PFN_vkGetInstanceProcAddr get_proc, VkInstance instance, VkDevice device, uint32_t family,
               uint32_t index, bool timeline) {
        Stop();
        if (get_proc == nullptr || instance == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
            return mode_ = Mode::Off;
        }
        const auto device_proc =
            reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_proc(instance, "vkGetDeviceProcAddr"));
        if (device_proc == nullptr) {
            return mode_ = Mode::Off;
        }
#define PF2_DEVICE_FN(name) name##_ = reinterpret_cast<PFN_##name>(device_proc(device, #name))
        PF2_DEVICE_FN(vkGetDeviceQueue);
        PF2_DEVICE_FN(vkQueueSubmit);
        PF2_DEVICE_FN(vkQueueWaitIdle);
        PF2_DEVICE_FN(vkCreateSemaphore);
        PF2_DEVICE_FN(vkDestroySemaphore);
        PF2_DEVICE_FN(vkWaitSemaphores);
        PF2_DEVICE_FN(vkCreateBuffer);
        PF2_DEVICE_FN(vkDestroyBuffer);
        PF2_DEVICE_FN(vkGetBufferMemoryRequirements);
        PF2_DEVICE_FN(vkAllocateMemory);
        PF2_DEVICE_FN(vkFreeMemory);
        PF2_DEVICE_FN(vkBindBufferMemory);
        PF2_DEVICE_FN(vkCreateCommandPool);
        PF2_DEVICE_FN(vkDestroyCommandPool);
        PF2_DEVICE_FN(vkAllocateCommandBuffers);
        PF2_DEVICE_FN(vkBeginCommandBuffer);
        PF2_DEVICE_FN(vkEndCommandBuffer);
        PF2_DEVICE_FN(vkCmdFillBuffer);
#undef PF2_DEVICE_FN
        if (vkGetDeviceQueue_ == nullptr || vkQueueSubmit_ == nullptr || vkQueueWaitIdle_ == nullptr) {
            return mode_ = Mode::Off;
        }
        device_ = device;
        vkGetDeviceQueue_(device, family, index, &queue_);
        if (queue_ == VK_NULL_HANDLE) {
            return mode_ = Mode::Off;
        }
        if (timeline && vkCreateSemaphore_ != nullptr && vkDestroySemaphore_ != nullptr && vkWaitSemaphores_ != nullptr) {
            VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            type.initialValue = 0;
            VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
            if (vkCreateSemaphore_(device, &create, nullptr, &semaphore_) == VK_SUCCESS) {
                {
                    std::lock_guard lock(mutex_);
                    stop_ = false;
                    failed_ = false;
                    signaled_ = completed_ = 0;
                    gpu_ms_.clear();
                    exclusive_ms_.clear();
                    last_done_ns_ = 0;
                }
                StartDevLoad(family);
                watcher_ = std::thread([this] { Watch(); });
                return mode_ = Mode::Timeline;
            }
            semaphore_ = VK_NULL_HANDLE;
        }
        return mode_ = Mode::Drain;
    }

    void Stop() {
        if (watcher_.joinable()) {
            {
                std::lock_guard lock(mutex_);
                stop_ = true;
            }
            cv_.notify_all();
            watcher_.join();
        }
        if (semaphore_ != VK_NULL_HANDLE && vkDestroySemaphore_ != nullptr) {
            vkDestroySemaphore_(device_, semaphore_, nullptr);
        }
        semaphore_ = VK_NULL_HANDLE;
        StopDevLoad();
        mode_ = Mode::Off;
    }

    // PF2_DEV_GPU_LOAD_MB: the MB each set's signal fills first (0: none).
    unsigned DevLoadMb() const noexcept { return load_cb_ != VK_NULL_HANDLE ? load_mb_ : 0u; }

    Mode CurrentMode() const noexcept { return mode_; }

    // Aurora's frame worker, in the submitted callback (Dawn's device lock not held). The value to
    // wait for, or 0 (drain mode, or the signal failed: the release then drains the queue).
    uint64_t Signal() {
        if (mode_ != Mode::Timeline) {
            return 0;
        }
        uint64_t value;
        {
            std::lock_guard lock(mutex_);
            if (failed_) {
                return 0;
            }
            value = signaled_ + 1;
            signal_ns_[value % kRing] = NowNs();
        }
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.signalSemaphoreValueCount = 1;
        timeline.pSignalSemaphoreValues = &value;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline};
        if (load_cb_ != VK_NULL_HANDLE) {
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &load_cb_;
        }
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &semaphore_;
        VkResult result;
        {
            void* guard = aurora_vulkan_direct_lock_queue();
            result = vkQueueSubmit_(queue_, 1, &submit, VK_NULL_HANDLE);
            aurora_vulkan_direct_unlock_queue(guard);
        }
        std::lock_guard lock(mutex_);
        if (result != VK_SUCCESS) {
            failed_ = true;
            cv_.notify_all();
            return 0;
        }
        signaled_ = value;
        cv_.notify_all();
        return value;
    }

    // XR thread, before releasing a set's images: true once the set that signalled `value` finished
    // (0: drain Dawn's queue instead). False on a timeout or a failure.
    bool Wait(uint64_t value, std::chrono::milliseconds timeout) {
        return WaitUntil(value, std::chrono::steady_clock::now() + timeout);
    }

    // As Wait, up to a point in time (timeline mode; drain mode waits for the queue regardless).
    bool WaitUntil(uint64_t value, std::chrono::steady_clock::time_point deadline) {
        if (mode_ == Mode::Off) {
            return true;
        }
        if (value == 0 || mode_ == Mode::Drain) {
            void* guard = aurora_vulkan_direct_lock_queue();
            const bool idle = vkQueueWaitIdle_(queue_) == VK_SUCCESS;
            aurora_vulkan_direct_unlock_queue(guard);
            return idle;
        }
        std::unique_lock lock(mutex_);
        return cv_.wait_until(lock, deadline, [&] { return completed_ >= value || failed_ || stop_; }) &&
               completed_ >= value;
    }

    // The signal-to-completion times recorded since the last call.
    void TakeGpuMs(std::vector<float>& out) {
        std::lock_guard lock(mutex_);
        out.insert(out.end(), gpu_ms_.begin(), gpu_ms_.end());
        gpu_ms_.clear();
    }

    // Each set's own GPU time since the last call: from its signal, or from the previous set's
    // completion when that came later (time spent queued behind the previous set's work is not
    // this set's).
    void TakeExclusiveMs(std::vector<float>& out) {
        std::lock_guard lock(mutex_);
        out.insert(out.end(), exclusive_ms_.begin(), exclusive_ms_.end());
        exclusive_ms_.clear();
    }

private:
    static constexpr uint64_t kRing = 64;

    static int64_t NowNs() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void Watch() {
        uint64_t next = 1;
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return stop_ || failed_ || signaled_ >= next; });
                if (stop_ || failed_) {
                    return;
                }
            }
            VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wait.semaphoreCount = 1;
            wait.pSemaphores = &semaphore_;
            wait.pValues = &next;
            const VkResult result = vkWaitSemaphores_(device_, &wait, 50'000'000ull);
            if (result == VK_TIMEOUT) {
                continue;
            }
            const int64_t now = NowNs();
            std::lock_guard lock(mutex_);
            if (result != VK_SUCCESS) {
                failed_ = true;
                cv_.notify_all();
                return;
            }
            const int64_t signaled_at = signal_ns_[next % kRing];
            if (gpu_ms_.size() < 4096) {
                gpu_ms_.push_back(static_cast<float>(static_cast<double>(now - signaled_at) / 1.0e6));
            }
            if (exclusive_ms_.size() < 4096) {
                exclusive_ms_.push_back(
                    static_cast<float>(static_cast<double>(now - std::max(signaled_at, last_done_ns_)) / 1.0e6));
            }
            last_done_ns_ = now;
            completed_ = next++;
            cv_.notify_all();
        }
    }

    // PF2_DEV_GPU_LOAD_MB: a scratch buffer and one command buffer that fills it (recorded once,
    // submitted with every signal).
    void StartDevLoad(uint32_t family) {
        const char* env = std::getenv("PF2_DEV_GPU_LOAD_MB");
        const long mb = env != nullptr ? std::strtol(env, nullptr, 10) : 0;
        if (mb <= 0 || mb > 4096 || vkCreateBuffer_ == nullptr || vkAllocateMemory_ == nullptr ||
            vkCreateCommandPool_ == nullptr || vkAllocateCommandBuffers_ == nullptr || vkCmdFillBuffer_ == nullptr ||
            vkBeginCommandBuffer_ == nullptr || vkEndCommandBuffer_ == nullptr || vkBindBufferMemory_ == nullptr ||
            vkGetBufferMemoryRequirements_ == nullptr) {
            return;
        }
        constexpr VkDeviceSize kChunk = 64ull << 20;
        const VkDeviceSize total = static_cast<VkDeviceSize>(mb) << 20;
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = std::min(total, kChunk);
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer_(device_, &buffer_info, nullptr, &load_buffer_) != VK_SUCCESS) {
            load_buffer_ = VK_NULL_HANDLE;
            return;
        }
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements_(device_, load_buffer_, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = 0;
        while (alloc.memoryTypeIndex < 32 && (req.memoryTypeBits & (1u << alloc.memoryTypeIndex)) == 0) {
            ++alloc.memoryTypeIndex;
        }
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.queueFamilyIndex = family;
        VkCommandBufferAllocateInfo cb_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb_info.commandBufferCount = 1;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        if (alloc.memoryTypeIndex >= 32 || vkAllocateMemory_(device_, &alloc, nullptr, &load_memory_) != VK_SUCCESS ||
            vkBindBufferMemory_(device_, load_buffer_, load_memory_, 0) != VK_SUCCESS ||
            vkCreateCommandPool_(device_, &pool_info, nullptr, &load_pool_) != VK_SUCCESS) {
            StopDevLoad();
            return;
        }
        cb_info.commandPool = load_pool_;
        if (vkAllocateCommandBuffers_(device_, &cb_info, &load_cb_) != VK_SUCCESS ||
            vkBeginCommandBuffer_(load_cb_, &begin) != VK_SUCCESS) {
            load_cb_ = VK_NULL_HANDLE;
            StopDevLoad();
            return;
        }
        for (VkDeviceSize done = 0; done < total; done += buffer_info.size) {
            vkCmdFillBuffer_(load_cb_, load_buffer_, 0, std::min(buffer_info.size, total - done),
                             static_cast<uint32_t>(done >> 20));
        }
        if (vkEndCommandBuffer_(load_cb_) != VK_SUCCESS) {
            load_cb_ = VK_NULL_HANDLE;
            StopDevLoad();
            return;
        }
        load_mb_ = static_cast<unsigned>(mb);
    }

    void StopDevLoad() {
        if (device_ == VK_NULL_HANDLE) return;
        if (load_pool_ != VK_NULL_HANDLE && vkDestroyCommandPool_ != nullptr) {
            vkDestroyCommandPool_(device_, load_pool_, nullptr);
        }
        if (load_buffer_ != VK_NULL_HANDLE && vkDestroyBuffer_ != nullptr) {
            vkDestroyBuffer_(device_, load_buffer_, nullptr);
        }
        if (load_memory_ != VK_NULL_HANDLE && vkFreeMemory_ != nullptr) {
            vkFreeMemory_(device_, load_memory_, nullptr);
        }
        load_pool_ = VK_NULL_HANDLE;
        load_cb_ = VK_NULL_HANDLE;
        load_buffer_ = VK_NULL_HANDLE;
        load_memory_ = VK_NULL_HANDLE;
        load_mb_ = 0;
    }

    Mode mode_ = Mode::Off;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSemaphore semaphore_ = VK_NULL_HANDLE;
    PFN_vkCreateBuffer vkCreateBuffer_ = nullptr;
    PFN_vkDestroyBuffer vkDestroyBuffer_ = nullptr;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements_ = nullptr;
    PFN_vkAllocateMemory vkAllocateMemory_ = nullptr;
    PFN_vkFreeMemory vkFreeMemory_ = nullptr;
    PFN_vkBindBufferMemory vkBindBufferMemory_ = nullptr;
    PFN_vkCreateCommandPool vkCreateCommandPool_ = nullptr;
    PFN_vkDestroyCommandPool vkDestroyCommandPool_ = nullptr;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers_ = nullptr;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer_ = nullptr;
    PFN_vkEndCommandBuffer vkEndCommandBuffer_ = nullptr;
    PFN_vkCmdFillBuffer vkCmdFillBuffer_ = nullptr;
    VkBuffer load_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory load_memory_ = VK_NULL_HANDLE;
    VkCommandPool load_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer load_cb_ = VK_NULL_HANDLE;
    unsigned load_mb_ = 0;
    PFN_vkGetDeviceQueue vkGetDeviceQueue_ = nullptr;
    PFN_vkQueueSubmit vkQueueSubmit_ = nullptr;
    PFN_vkQueueWaitIdle vkQueueWaitIdle_ = nullptr;
    PFN_vkCreateSemaphore vkCreateSemaphore_ = nullptr;
    PFN_vkDestroySemaphore vkDestroySemaphore_ = nullptr;
    PFN_vkWaitSemaphores vkWaitSemaphores_ = nullptr;
    std::thread watcher_;
    std::mutex mutex_;
    std::condition_variable cv_;
    uint64_t signaled_ = 0;
    uint64_t completed_ = 0;
    bool failed_ = false;
    bool stop_ = false;
    std::array<int64_t, kRing> signal_ns_{};
    std::vector<float> gpu_ms_;
    std::vector<float> exclusive_ms_;
    int64_t last_done_ns_ = 0;
};

// "p50/p95/max" of a window's samples, for the [pf2] line.
std::string SampleStat(std::vector<float>& values) {
    if (values.empty()) {
        return "-";
    }
    std::sort(values.begin(), values.end());
    const auto at = [&](double q) { return values[std::min(values.size() - 1, static_cast<size_t>(q * values.size()))]; };
    char text[64];
    std::snprintf(text, sizeof(text), "%.2f/%.2f/%.2f", at(0.5), at(0.95), values.back());
    return text;
}

// PF2_END_PHASE / PF2_END_DELAY_US (KartFrame lane X): when in SteamVR's refresh each eye set is
// released and its frame ended. Without either both happen right after xrWaitFrame returns (and
// the release fence), which on the Frame lands on the compositor's update edge, a phase that
// drifts per launch. The compositor grid (newest vsync and period, CLOCK_MONOTONIC ns = the
// steady_clock here; update start p5/p50 and end p50/p95, ns after the vsync) comes from the
// game's own SteamVR timing reader (steamvr_frame_timing.h), or with PF2_PHASE_FILE set from that
// 64-byte record (dev/tools/vrtiming.py: 'KFP1', count, vsync, period, the four percentiles, write
// time). The pacing thread looks at it at most every 50 ms and aims the end at
//   after[:us]  -> vsync + update end p95 + us (default 300),
//   before[:us] -> vsync + update start p50 - us (default 1500),
// of the vsync at or before the wake-up. Without a fresh grid (none, older than 0.5 s, or
// xrWaitFrame's period not 1-4 times the grid's within 1 %) the end waits PF2_END_DELAY_US after
// the wake-up, else the offset the lock last used at this refresh rate (vr_end_phase.txt in the
// user folder), else half the display's true period (never xrWaitFrame's doubled one): at 90 Hz 5.6 ms after the wake-up, past the Frame's update window (3.0-5.3 ms after the
// vsync) and next to where the lock ends (about 5.2 ms). Not later: the next packet is located
// only after this end and the game must hand it over before the next wake-up, so an end at 0.75
// of the period left the game 2.8 ms and cost the mock 15-20 fps; a fixed 4000 us ended inside
// the update window at 90 Hz.
// Every 10 s (MP_VR_LOG=1) a summary line: where the ends landed against the vsync and the window.
class EndPhaseLock {
public:
    enum class Mode { Off, Delay, After, Before };
    enum class Source { None, Reader, File };
    using Clock = std::chrono::steady_clock;
    using LogFn = std::function<void(const std::string&)>;

    void Configure(const pf2::Switches& switches, const std::string& runtime_name, LogFn log) {
        log_ = std::move(log);
        mode_ = Mode::Off;
        source_ = Source::None;
        have_ = locked_ = announced_ = started_ = false;
        first_frame_ = Clock::time_point{};
        fixed_us_ = switches.end_delay_us;
        learned_hz_ = 0;
        if (switches.end_phase.empty()) {
            if (switches.end_delay_us > 0) {
                mode_ = Mode::Delay;
                log_("end phase: PF2_END_DELAY_US=" + std::to_string(fixed_us_) +
                     ": each eye set released and its frame ended that long after xrWaitFrame returned");
            }
            return;
        }
        bool steamvr = runtime_name.find("SteamVR") != std::string::npos;
#ifdef PF2_STEAMVR_READER
        steamvr = steamvr || steamvr_timing::SteamVrClientLoaded();
#endif
        if (switches.end_phase_default && !steamvr) {
            off_reason_ = "the runtime ('" + runtime_name + "') is not SteamVR";
            return;
        }
        mode_ = switches.end_phase == "after" ? Mode::After : Mode::Before;
        offset_us_ = switches.end_phase_us >= 0 ? switches.end_phase_us : (mode_ == Mode::After ? 300 : 1500);
        if (const char* path = std::getenv("PF2_PHASE_FILE"); path != nullptr && *path != '\0') {
            source_ = Source::File;
            path_ = path;
        } else if (switches.timing_reader) {
#ifdef PF2_STEAMVR_READER
            source_ = Source::Reader;
            steamvr_timing::Start(log_);
#endif
        }
        log_("end phase: " + switches.end_phase + (switches.end_phase_default ? " (default)" : " (PF2_END_PHASE)") +
             ": each eye set released and its frame ended at vsync + " +
             (mode_ == Mode::After ? "update end p95 + " : "update start p50 - ") + std::to_string(offset_us_) +
             " us of SteamVR's compositor grid from " + SourceName() + "; without a grid " + FallbackName());
    }
    bool Enabled() const { return mode_ != Mode::Off; }

    // Each finished frame, phased or not: the start line once the end-phase state is known (the
    // first lock, or 3 s without one).
    void NoteFrame(int64_t period_ns) {
        if (started_ || period_ns <= 0) return;
        const Clock::time_point now = Clock::now();
        if (first_frame_ == Clock::time_point{}) first_frame_ = now;
        if (!locked_ && now - first_frame_ < std::chrono::seconds(3) && mode_ != Mode::Off && mode_ != Mode::Delay) {
            return;
        }
        started_ = true;
        const double hz = 1e9 / static_cast<double>(period_ns);
        const auto& switches = pf2::Get();
        char head[96];
        std::snprintf(head, sizeof head, "PF2 start: display %.1f Hz (period %.2f ms)", hz, period_ns / 1e6);
        std::string line = head;
        line += "; sim ";
        if (!switches.sim_lock) {
            line += "on the wall clock (PF2_SIM_LOCK off)";
        } else if (pf2::SimLockStride(hz) == 1) {
            line += "locked, 1 step per packet (up to PF2_SIM_LOCK_MAX_HZ=" + std::to_string(switches.sim_lock_max_hz) +
                    ")";
        } else {
            line += "locked, 1 step of 2 periods per 2 packets (above PF2_SIM_LOCK_MAX_HZ=" +
                    std::to_string(switches.sim_lock_max_hz) + ")";
        }
        line += "; end phase ";
        switch (mode_) {
        case Mode::Off:
            line += off_reason_.empty() ? "off" : "off (" + off_reason_ + ")";
            break;
        case Mode::Delay:
            line += "fixed delay " + std::to_string(fixed_us_) + " us";
            break;
        default:
            line += std::string(mode_ == Mode::After ? "after" : "before") + " +" + std::to_string(offset_us_) +
                    " us, " + (locked_ ? "LOCKED to the compositor grid" : "NOT locked (fallback " + FallbackName() + ")") +
                    ", grid from " + SourceName();
            break;
        }
        log_(line);
    }

    // The display's own refresh period: the compositor grid's when `nominal_ns` (the session's
    // refresh period as xrWaitFrame reported it on time, DisplayPeriod below) is a whole multiple of
    // it, else `nominal_ns`. Never xrWaitFrame's current period, which SteamVR doubles (22.2 ms at
    // 90 Hz) while the app misses refreshes.
    int64_t TruePeriod(int64_t nominal_ns) const {
        if (have_ && period_ns_ > 0 && nominal_ns > 0 && Multiple(nominal_ns, period_ns_) > 0) return period_ns_;
        return nominal_ns;
    }

    // k when `period` is k (1..4) times `base` within 1 %, else 0.
    static int Multiple(int64_t period, int64_t base) {
        if (base <= 0 || period <= 0) return 0;
        const int64_t k = std::llround(static_cast<double>(period) / static_cast<double>(base));
        return k >= 1 && k <= 4 && std::llabs(period - k * base) * 100 <= period ? static_cast<int>(k) : 0;
    }

    // When to release and end this cycle's frame. wake: xrWaitFrame's return; period: its display
    // period; nominal: the session's refresh period (see TruePeriod). Never later than one true
    // period after the wake-up less 1 ms (the next refresh's wake-up). A grid whose period divides
    // xrWaitFrame's (SteamVR reports 2 or more periods after a missed refresh) still locks: the end
    // stays on the display's own grid, so the next frame can be on time again (ending half of the
    // doubled period after the wake-up instead kept the game at half rate).
    Clock::time_point Target(Clock::time_point wake, int64_t period_ns, int64_t nominal_ns) {
        const Clock::time_point now = Clock::now();
        if (mode_ == Mode::After || mode_ == Mode::Before) Refresh(now);
        const int64_t true_ns = TruePeriod(nominal_ns > 0 ? nominal_ns : period_ns);
        const Clock::time_point deadline =
            wake + std::chrono::nanoseconds(std::max<int64_t>(true_ns - 1000000, 0));
        vsync_ns_ = 0;
        Clock::time_point target = wake + std::chrono::microseconds(FallbackUs(true_ns));
        if (mode_ == Mode::After || mode_ == Mode::Before) {
            const int64_t now_ns = now.time_since_epoch().count();
            const int multiple = Multiple(period_ns, period_ns_);
            const bool fresh = have_ && now_ns - written_ns_ < 500000000 && multiple > 0;
            if (fresh && multiple > 1) ++multiple_frames_;
            if (fresh != locked_ || !announced_) {
                log_(std::string("end phase: ") +
                     (fresh ? "locked to the compositor grid (" + SourceName() + ")"
                            : "fallback " + std::to_string(FallbackUs(true_ns)) + " us after the wake-up, " +
                                  FallbackName() + " (" +
                                  (have_ ? (now_ns - written_ns_ >= 500000000
                                                ? "grid older than 0.5 s"
                                                : "grid period not a whole multiple of xrWaitFrame's")
                                         : "no grid from " + SourceName()) +
                                  ")"));
                locked_ = fresh;
                announced_ = true;
            }
            if (locked_) {
                const int64_t wake_ns = wake.time_since_epoch().count();
                // The vsync at or before the wake-up (SteamVR returns about 0.4 ms after it; 1 ms of
                // slack for the grid's error). A late wake-up keeps its vsync, so its target may
                // already have passed.
                const double k =
                    std::floor(static_cast<double>(wake_ns - vsync_pub_ + 1000000) / static_cast<double>(period_ns_));
                vsync_ns_ = vsync_pub_ + static_cast<int64_t>(k * static_cast<double>(period_ns_));
                const int64_t at = mode_ == Mode::After ? vsync_ns_ + ue95_ + offset_us_ * 1000
                                                        : vsync_ns_ + us50_ - offset_us_ * 1000;
                target = Clock::time_point(Clock::duration(at));
                Learn(target - wake, wake_ns - vsync_ns_, period_ns);
            }
        }
        if (target > deadline) {
            ++capped_;
            target = deadline;
        }
        return target;
    }

    // The release and end just made, for the 10 s summary (only with MP_VR_LOG=1).
    void NoteEnd(Clock::time_point end, Clock::time_point wake) {
        ++frames_;
        e_.push_back(std::chrono::duration<double, std::milli>(end - wake).count());
        if (vsync_ns_ != 0) {
            const double d = static_cast<double>(end.time_since_epoch().count() - vsync_ns_) / 1e6;
            vs_.push_back(d);
            us_.push_back(d - us50_ / 1e6);
            ue_.push_back(d - ue95_ / 1e6);
        } else {
            ++fallback_frames_;
        }
        if (summary_at_ == Clock::time_point{}) summary_at_ = end;
        if (end - summary_at_ < std::chrono::seconds(10)) return;
        summary_at_ = end;
        const auto q = [](std::vector<double>& v, double p) {
            if (v.empty()) return std::nan("");
            const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())));
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i), v.end());
            return v[i];
        };
        char line[560];
        std::snprintf(line, sizeof line,
                      "end phase: 10 s, %u frames (%u on the fallback, %u capped at the next wake-up); end after "
                      "wake-up p5 %.2f p50 %.2f p95 %.2f ms (%u locked while xrWaitFrame's period was a multiple of the grid's); after vsync p50 %.2f p95 %.2f, minus update start p50 "
                      "%+.2f p95 %+.2f, minus update end p95: p50 %+.2f p95 %+.2f (grid start %.2f end %.2f/%.2f "
                      "p50/p95)",
                      frames_, fallback_frames_, capped_, q(e_, 0.05), q(e_, 0.5), q(e_, 0.95), multiple_frames_, q(vs_, 0.5),
                      q(vs_, 0.95), q(us_, 0.5), q(us_, 0.95), q(ue_, 0.5), q(ue_, 0.95), us50_ / 1e6, ue50_ / 1e6,
                      ue95_ / 1e6);
        log_(line);
        frames_ = fallback_frames_ = capped_ = multiple_frames_ = 0;
        vs_.clear(), us_.clear(), ue_.clear(), e_.clear();
    }

private:
    std::string SourceName() const {
        switch (source_) {
        case Source::Reader:
#ifdef PF2_STEAMVR_READER
            return "the in-process SteamVR timing reader (" + steamvr_timing::Describe() + ")";
#else
            return "nowhere";
#endif
        case Source::File:
            return path_ + " (PF2_PHASE_FILE)";
        default:
            return "nowhere (PF2_TIMING_READER=0)";
        }
    }

    std::string FallbackName() const {
        if (fixed_us_ > 0) return "PF2_END_DELAY_US=" + std::to_string(fixed_us_);
        if (learned_us_ > 0) return "the offset learned at " + std::to_string(learned_hz_) + " Hz";
        return "half the display period";
    }

    // The end's delay after the wake-up without a grid.
    long FallbackUs(int64_t period_ns) {
        if (fixed_us_ > 0) return fixed_us_;
        const int hz = static_cast<int>(std::lround(1e9 / static_cast<double>(std::max<int64_t>(period_ns, 1))));
        if (hz != learned_hz_) {
            learned_hz_ = hz;
#ifdef PF2_STEAMVR_READER
            learned_us_ = steamvr_timing::LearnedOffsetUs(hz);
#endif
            // Only a value inside the period's useful part.
            if (learned_us_ * 1000 < period_ns / 5 || learned_us_ * 1000 > period_ns - 1000000) learned_us_ = 0;
        }
        return learned_us_ > 0 ? learned_us_ : static_cast<long>(period_ns / 2 / 1000);
    }

    // The locked offsets after the wake-up: their median every 900 frames is kept for runs
    // without a grid. Only wake-ups on time (within 1 ms of their vsync) count: a late one (the
    // previous frame missed its slot) shortens the offset, which a fallback must not copy.
    void Learn(Clock::duration offset, int64_t wake_after_vsync_ns, int64_t period_ns) {
        if (wake_after_vsync_ns < 0 || wake_after_vsync_ns > 1000000) return;
        const long us = static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(offset).count());
        if (us <= 0 || us * 1000 >= period_ns) return;
        learn_.push_back(us);
        if (learn_.size() < 900) return;
        std::nth_element(learn_.begin(), learn_.begin() + 450, learn_.end());
        const long median = learn_[450];
        learn_.clear();
#ifdef PF2_STEAMVR_READER
        steamvr_timing::NoteLockedOffset(static_cast<int>(std::lround(1e9 / static_cast<double>(period_ns))), median);
#else
        (void)median;
#endif
    }

    void Refresh(Clock::time_point now) {
        if (now - read_at_ < std::chrono::milliseconds(50)) return;
        read_at_ = now;
        if (source_ == Source::Reader) {
#ifdef PF2_STEAMVR_READER
            steamvr_timing::Grid grid;
            if (!steamvr_timing::Latest(grid)) {
                have_ = false;
                return;
            }
            vsync_pub_ = grid.vsync_ns;
            period_ns_ = grid.period_ns;
            us50_ = grid.update_start_p50;
            ue50_ = grid.update_end_p50;
            ue95_ = grid.update_end_p95;
            written_ns_ = grid.written_ns;
            have_ = true;
#endif
            return;
        }
        if (source_ != Source::File) return;
        unsigned char b[64];
        std::FILE* file = std::fopen(path_.c_str(), "rb");
        if (file == nullptr) return;
        const size_t n = std::fread(b, 1, sizeof b, file);
        std::fclose(file);
        if (n != sizeof b || std::memcmp(b, "KFP1", 4) != 0) return;
        const auto i64 = [&](size_t o) { int64_t v; std::memcpy(&v, b + o, 8); return v; };
        const auto i32 = [&](size_t o) { int32_t v; std::memcpy(&v, b + o, 4); return static_cast<int64_t>(v); };
        vsync_pub_ = i64(8);
        period_ns_ = i64(16);
        us50_ = i32(28);
        ue50_ = i32(32);
        ue95_ = i32(36);
        written_ns_ = i64(40);
        have_ = true;
    }

    Mode mode_ = Mode::Off;
    Source source_ = Source::None;
    LogFn log_;
    long offset_us_ = 0;
    long fixed_us_ = 0;
    long learned_us_ = 0;
    int learned_hz_ = 0;
    std::vector<long> learn_;
    std::string path_;
    std::string off_reason_;
    Clock::time_point read_at_{};
    Clock::time_point first_frame_{};
    bool have_ = false;
    bool locked_ = false;
    bool announced_ = false;
    bool started_ = false;
    int64_t vsync_pub_ = 0, period_ns_ = 0, us50_ = 0, ue50_ = 0, ue95_ = 0, written_ns_ = 0;
    int64_t vsync_ns_ = 0;
    Clock::time_point summary_at_{};
    uint32_t frames_ = 0, fallback_frames_ = 0, capped_ = 0, multiple_frames_ = 0;
    std::vector<double> vs_, us_, ue_, e_;
};

} // namespace

class OpenXRVulkanDirectBackend::Impl final {
public:
    // The steady clock, as the end phase's.
    using Clock = std::chrono::steady_clock;

    explicit Impl(OpenXRLogCallback logger) : logger_(std::move(logger)) {}

    ~Impl() { Shutdown(); }

    struct EyeSwapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<XrSwapchainImageVulkan2KHR> images;
        uint32_t acquired_index = 0;
        bool acquired = false;
        bool waited = false;
        bool release_forbidden = false;
    };

    static uint32_t VkVersion(XrVersion version) noexcept {
        return VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(version), XR_VERSION_MINOR(version), XR_VERSION_PATCH(version));
    }

    // ---- Dawn's hooks (aurora/dawn_vulkan_abi.h): its Vulkan objects come from the runtime ----

    static int32_t CreateInstance(void* self, void* proc, const void* info, const void* allocator, void** out) {
        auto& owner = *static_cast<Impl*>(self);
        const auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(proc);
        auto vk_info = *static_cast<const VkInstanceCreateInfo*>(info);
        if (vk_info.pApplicationInfo == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        auto app = *vk_info.pApplicationInfo;
        // Dawn asks for Vulkan 1.1; take 1.2 where the loader and the runtime allow it, so the
        // device can enable timeline semaphores (core there) should the runtime use them.
        uint32_t loader_version = VK_API_VERSION_1_0;
        const auto enumerate = get_proc != nullptr ? reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
                                                         get_proc(nullptr, "vkEnumerateInstanceVersion"))
                                                   : nullptr;
        if (enumerate == nullptr || enumerate(&loader_version) != VK_SUCCESS) {
            loader_version = VK_API_VERSION_1_0;
        }
        const uint32_t preferred = std::min({static_cast<uint32_t>(VK_API_VERSION_1_2), loader_version,
                                             VkVersion(owner.requirements_.max_api_version)});
        app.apiVersion = std::max({app.apiVersion, VkVersion(owner.requirements_.min_api_version), preferred});
        if (app.apiVersion > VkVersion(owner.requirements_.max_api_version)) {
            return VK_ERROR_INCOMPATIBLE_DRIVER;
        }
        vk_info.pApplicationInfo = &app;
        XrVulkanInstanceCreateInfoKHR create{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
        create.systemId = owner.runtime_->SystemId();
        create.pfnGetInstanceProcAddr = get_proc;
        create.vulkanCreateInfo = &vk_info;
        create.vulkanAllocator = static_cast<const VkAllocationCallbacks*>(allocator);
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        const XrResult xr =
            owner.create_instance_(owner.runtime_->Instance(), &create, reinterpret_cast<VkInstance*>(out), &result);
        if (XR_SUCCEEDED(xr) && result == VK_SUCCESS) {
            owner.vk_instance_ = *reinterpret_cast<VkInstance*>(out);
            owner.instance_api_version_ = app.apiVersion;
            owner.get_instance_proc_ = get_proc;
        }
        return XR_SUCCEEDED(xr) ? result : VK_ERROR_INITIALIZATION_FAILED;
    }

    static int32_t GetPhysical(void* self, void* instance, void** out) {
        auto& owner = *static_cast<Impl*>(self);
        XrVulkanGraphicsDeviceGetInfoKHR get{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
        get.systemId = owner.runtime_->SystemId();
        get.vulkanInstance = static_cast<VkInstance>(instance);
        return XR_SUCCEEDED(owner.get_device_(owner.runtime_->Instance(), &get, reinterpret_cast<VkPhysicalDevice*>(out)))
                   ? VK_SUCCESS
                   : VK_ERROR_INITIALIZATION_FAILED;
    }

    static int32_t CreateDevice(void* self, void* proc, void* physical, const void* info, const void* allocator,
                                void** out) {
        auto& owner = *static_cast<Impl*>(self);
        const auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(proc);
        const auto vk_physical = static_cast<VkPhysicalDevice>(physical);
        auto vk_info = *static_cast<const VkDeviceCreateInfo*>(info);
        // A runtime that creates timeline semaphores on the application's device appends the
        // extension but not the feature; enable it (core from 1.2) when Dawn's chain does not.
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        if (get_proc != nullptr && owner.vk_instance_ != VK_NULL_HANDLE &&
            owner.instance_api_version_ >= VK_API_VERSION_1_2) {
            const auto properties_fn = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                get_proc(owner.vk_instance_, "vkGetPhysicalDeviceProperties"));
            const auto features_fn = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                get_proc(owner.vk_instance_, "vkGetPhysicalDeviceFeatures2"));
            VkPhysicalDeviceProperties properties{};
            if (properties_fn != nullptr) {
                properties_fn(vk_physical, &properties);
            }
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &timeline};
            if (features_fn != nullptr && properties.apiVersion >= VK_API_VERSION_1_2) {
                features_fn(vk_physical, &features);
            }
            bool chained = false;
            for (auto* next = static_cast<const VkBaseInStructure*>(vk_info.pNext); next != nullptr;
                 next = next->pNext) {
                chained |= next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES ||
                           next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            }
            if (timeline.timelineSemaphore == VK_TRUE && !chained) {
                timeline.pNext = const_cast<void*>(vk_info.pNext);
                vk_info.pNext = &timeline;
            } else {
                timeline.timelineSemaphore = VK_FALSE;
            }
        }
        XrVulkanDeviceCreateInfoKHR create{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
        create.systemId = owner.runtime_->SystemId();
        create.pfnGetInstanceProcAddr = get_proc;
        create.vulkanPhysicalDevice = vk_physical;
        create.vulkanCreateInfo = &vk_info;
        create.vulkanAllocator = static_cast<const VkAllocationCallbacks*>(allocator);
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        const XrResult xr =
            owner.create_device_(owner.runtime_->Instance(), &create, reinterpret_cast<VkDevice*>(out), &result);
        if (XR_SUCCEEDED(xr) && result == VK_SUCCESS) {
            owner.timeline_semaphores_ = timeline.timelineSemaphore == VK_TRUE;
        }
        return XR_SUCCEEDED(xr) ? result : VK_ERROR_INITIALIZATION_FAILED;
    }

    bool QueryGraphicsRequirements(OpenXRRuntime& runtime) {
        ClearError();
        if (!runtime.IsInitialized() || runtime.HasSession()) {
            return Fail("OpenXR must own an instance, but no session, before querying Vulkan requirements");
        }
        if (requirements_queried_ && runtime_ != &runtime) {
            return Fail("Vulkan graphics requirements were already queried from another OpenXR instance");
        }
        if (!aurora_vulkan_direct_available()) {
            return Fail(std::string("direct presentation needs PrimedGun's patched Dawn (") + kPatchedDawnScript +
                        "); this one cannot create its Vulkan device through OpenXR");
        }
        const auto& extensions = runtime.EnabledExtensions();
        if (std::find(extensions.begin(), extensions.end(), std::string(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) ==
            extensions.end()) {
            return Fail("direct presentation needs XR_KHR_vulkan_enable2");
        }
        runtime_ = &runtime;
        PFN_xrGetVulkanGraphicsRequirements2KHR get_requirements = nullptr;
        if (!runtime.LoadFunction("xrGetVulkanGraphicsRequirements2KHR", &get_requirements) ||
            get_requirements == nullptr || !runtime.LoadFunction("xrCreateVulkanInstanceKHR", &create_instance_) ||
            create_instance_ == nullptr || !runtime.LoadFunction("xrCreateVulkanDeviceKHR", &create_device_) ||
            create_device_ == nullptr || !runtime.LoadFunction("xrGetVulkanGraphicsDevice2KHR", &get_device_) ||
            get_device_ == nullptr) {
            runtime_ = nullptr;
            return Fail("the OpenXR runtime did not expose the XR_KHR_vulkan_enable2 functions");
        }
        XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
        const XrResult result = get_requirements(runtime.Instance(), runtime.SystemId(), &requirements);
        runtime.ObserveResult(result);
        if (XR_FAILED(result)) {
            runtime_ = nullptr;
            std::ostringstream message;
            message << "xrGetVulkanGraphicsRequirements2KHR failed (" << result << ')';
            return Fail(message.str());
        }
        requirements_ = {requirements.minApiVersionSupported, requirements.maxApiVersionSupported, true};
        const AuroraDawnVulkanHooks hooks{this, CreateInstance, CreateDevice, GetPhysical};
        if (!aurora_vulkan_direct_configure(&hooks)) {
            runtime_ = nullptr;
            return Fail("Aurora's Dawn did not accept the OpenXR Vulkan hooks");
        }
        hooks_installed_ = true;
        {
            std::lock_guard lock(submission_mutex_);
            shutting_down_ = false;
            awaiting_token_ = 0;
            submission_results_ = {};
        }
        requirements_queried_ = true;
        std::ostringstream message;
        message << "OpenXR Vulkan requirements: API " << XR_VERSION_MAJOR(requirements_.min_api_version) << '.'
                << XR_VERSION_MINOR(requirements_.min_api_version) << " to "
                << XR_VERSION_MAJOR(requirements_.max_api_version) << '.'
                << XR_VERSION_MINOR(requirements_.max_api_version)
                << " via XR_KHR_vulkan_enable2; Dawn creates its device through the runtime (direct presentation)";
        Log(OpenXRLogLevel::Info, message.str());
        return true;
    }

    bool BindAurora(OpenXRRuntime& runtime) {
        ClearError();
        if (!requirements_queried_ || runtime_ != &runtime || runtime.HasSession()) {
            return Fail("QueryGraphicsRequirements must succeed on this OpenXR instance before BindAurora");
        }
        if (bound_) {
            return Fail("OpenXR Vulkan backend is already bound");
        }
        AuroraDawnVulkanHandles handles{};
        int64_t format = 0;
        if (!aurora_vulkan_direct_get_handles(&handles, &format)) {
            return Fail("Aurora did not report a Dawn Vulkan device it can share with the session");
        }
        if (handles.instance != vk_instance_) {
            return Fail("Dawn's Vulkan instance was not created through the OpenXR runtime");
        }
        void* physical = nullptr;
        if (GetPhysical(this, handles.instance, &physical) != VK_SUCCESS || physical != handles.physicalDevice) {
            return Fail("Dawn's Vulkan GPU is not the OpenXR runtime's");
        }
        aurora_format_ = static_cast<VkFormat>(format);
        return BindSession(runtime, handles);
    }

    // The session on Dawn's device, its swapchains and the bridge (BindAurora, RecreateSession).
    bool BindSession(OpenXRRuntime& runtime, const AuroraDawnVulkanHandles& handles) {
        XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
        binding.instance = static_cast<VkInstance>(handles.instance);
        binding.physicalDevice = static_cast<VkPhysicalDevice>(handles.physicalDevice);
        binding.device = static_cast<VkDevice>(handles.device);
        binding.queueFamilyIndex = handles.queueFamily;
        binding.queueIndex = handles.queueIndex;
        // From here the runtime's calls that use the queue take Dawn's device lock.
        runtime.SetGraphicsQueueGuard(aurora_vulkan_direct_lock_queue, aurora_vulkan_direct_unlock_queue);
        if (!runtime.CreateSession(&binding)) {
            runtime.SetGraphicsQueueGuard(nullptr, nullptr);
            return Fail("OpenXR rejected Dawn's Vulkan device binding");
        }
        owns_session_ = true;
        packed_ = pf2::Get().packed;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            const auto& view = runtime.ViewConfiguration()[eye];
            eye_size_[eye] = {view.render_width, view.render_height};
        }
        requested_eye_size_ = eye_size_;
        eye_size_ = FitEyeSize(eye_size_);
        const auto abandon_session = [&] {
            DestroySwapchains();
            passthrough_.Destroy();
            runtime.DestroySession();
            runtime.SetGraphicsQueueGuard(nullptr, nullptr);
            owns_session_ = false;
        };
        if (!SelectSwapchainFormat() || !CreateSwapchains()) {
            abandon_session();
            return false;
        }
        if (runtime.ShouldExit()) {
            abandon_session();
            return Fail("OpenXR session became loss-pending while creating Vulkan swapchains");
        }
        // Before the bridge can fire a submitted callback, which signals the release fence.
        StartPf2(handles);
        if (!aurora_vulkan_direct_enable(&Impl::OnAuroraSubmitted, this)) {
            fence_.Stop();
            abandon_session();
            return Fail("Aurora could not enable its direct Vulkan stereo bridge");
        }
        bridge_enabled_ = true;
        bound_ = true;
        std::ostringstream message;
        message << "OpenXR Vulkan swapchains ready: VkFormat " << static_cast<int64_t>(swapchain_format_)
                << " (Aurora " << static_cast<int64_t>(aurora_format_) << "), eyes " << EyeWidth(eye_swapchains_, 0)
                << 'x' << EyeHeight(eye_swapchains_, 0) << " / " << EyeWidth(eye_swapchains_, 1) << 'x'
                << EyeHeight(eye_swapchains_, 1)
                << (packed_ ? " packed side by side in one swapchain per set" : "")
                << ", Vulkan " << VK_API_VERSION_MAJOR(instance_api_version_) << '.'
                << VK_API_VERSION_MINOR(instance_api_version_) << " instance, timeline semaphores "
                << (timeline_semaphores_ ? "enabled" : "not enabled")
                << "; direct presentation (eyes drawn into the swapchain on Dawn's queue)";
        Log(OpenXRLogLevel::Info, message.str());
        return true;
    }

    void SetRenderScale(float scale) {
        if (runtime_ == nullptr) {
            return;
        }
        std::array<OpenXREyeSize, kOpenXREyeCount> requested{};
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            requested[eye] = OpenXRScaledEyeSize(runtime_->ViewConfiguration()[eye].properties, scale);
        }
        // Only a change: a refused size stays refused while it is still the one asked for.
        if (requested != requested_eye_size_) {
            requested_eye_size_ = requested;
            eye_size_ = FitEyeSize(requested);
            eye_resize_gate_.Requested();
        }
    }

    // ---- Frame-first pacing (interpolation) ---------------------------------------------------

    OpenXRBeginStatus BeginFrame(const OpenXRPresentation& presentation, OpenXRBackendFrame& frame) {
        frame = {};
        frame.presentation = presentation;
        if (!bound_ || runtime_ == nullptr) {
            Fail("BeginFrame called before the Vulkan backend was bound");
            return OpenXRBeginStatus::Error;
        }
        if (frame_active_ || pending_packet_serial_ != 0 || queued_packet_serial_ != 0) {
            Fail("BeginFrame called while another OpenXR frame is active");
            return OpenXRBeginStatus::Error;
        }
        if (!ResizeWritablePair()) {
            return OpenXRBeginStatus::Error;
        }
        const OpenXRFrameStatus status = runtime_->WaitFrame(frame.xr_frame);
        if (status != OpenXRFrameStatus::Ready) {
            if (status == OpenXRFrameStatus::Error) {
                Fail(BeginStatusOperation(status));
            }
            return BeginStatusFor(status);
        }
        NoteDisplayTiming(frame.xr_frame);
        passthrough_.SetRunning(*runtime_, presentation.passthrough);
        if (!runtime_->BeginFrame(frame.xr_frame)) {
            Fail("xrBeginFrame failed");
            return OpenXRBeginStatus::Error;
        }
        frame_active_ = true;
        active_frame_serial_ = frame.xr_frame.serial;
        active_frame_ = frame.xr_frame;
        active_origin_ = CurrentPoseOrigin();
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            frame.render_width[eye] = EyeWidth(eye_swapchains_, eye);
            frame.render_height[eye] = EyeHeight(eye_swapchains_, eye);
        }
        if (!frame.xr_frame.should_render) {
            return OpenXRBeginStatus::Ready;
        }
        if (!runtime_->LocateViews(frame.xr_frame)) {
            Fail("xrLocateViews failed");
            EndActiveFrameWithoutLayers(frame.xr_frame);
            return OpenXRBeginStatus::Error;
        }
        active_frame_ = frame.xr_frame;
        active_pose_time_ = frame.xr_frame.predicted_display_time;
        if (!frame.xr_frame.views_valid) {
            return OpenXRBeginStatus::Ready;
        }
        const OpenXRBeginStatus prepared = PrepareTargets(frame, eye_swapchains_, panel_swapchain_);
        if (prepared != OpenXRBeginStatus::Ready) {
            EndActiveFrameWithoutLayers(frame.xr_frame);
        } else {
            active_packet_serial_ = frame.xr_frame.serial;
        }
        return prepared;
    }

    // Acquires the frame's images and hands them to Aurora; on failure nothing stays acquired.
    // `pair` and `panel_swapchain` are the swapchains the images come from: the writable ones, or
    // the retained ones for a packet queued behind the pending packet (ArmPacket).
    OpenXRBeginStatus PrepareTargets(OpenXRBackendFrame& frame, std::array<EyeSwapchain, kOpenXREyeCount>& pair,
                                     EyeSwapchain& panel_swapchain) {
        const uint32_t target_count = PresentationTargetCount(frame.presentation);
        if (target_count == 1) {
            frame.render_width[1] = frame.render_width[0];
            frame.render_height[1] = frame.render_height[0];
        }
        std::array<AuroraVulkanDirectTarget, kOpenXREyeCount> targets{};
        const diagnostics::Stopwatch acquire_timer;
        // PF2_FRAME_STAMP: the display time these images' poses were located for.
        const uint64_t stamp =
            pf2::Get().stamp ? static_cast<uint64_t>(frame.xr_frame.predicted_display_time) : 0;
        for (uint32_t eye = 0; eye < target_count; ++eye) {
            // Packed: both eyes are regions of the one image of pair[0], acquired once.
            auto& swapchain = EyeChain(pair, eye);
            if ((!packed_ || eye == 0) && !AcquireSwapchain(swapchain)) {
                ReleaseAcquired(pair, panel_swapchain);
                return OpenXRBeginStatus::Error;
            }
            targets[eye] = {
                reinterpret_cast<uint64_t>(swapchain.images[swapchain.acquired_index].image),
                EyeWidth(pair, eye),
                EyeHeight(pair, eye),
                static_cast<int64_t>(swapchain_format_),
                EyeX(pair, eye),
                packed_ ? swapchain.width : 0u,
                stamp,
            };
        }
        // The settings panel's layer image, rendered with the eyes while it is open.
        AuroraVulkanDirectTarget panel_target{};
        const bool panel = frame.presentation.panel.requested && EnsurePanelSwapchains();
        frame.presentation.panel.requested = panel;
        if (panel) {
            if (!AcquireSwapchain(panel_swapchain)) {
                ReleaseAcquired(pair, panel_swapchain);
                return OpenXRBeginStatus::Error;
            }
            panel_target = {
                reinterpret_cast<uint64_t>(panel_swapchain.images[panel_swapchain.acquired_index].image),
                panel_swapchain.width,
                panel_swapchain.height,
                static_cast<int64_t>(swapchain_format_),
            };
        }
        diagnostics::OnSwapchainAcquire(acquire_timer);
        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = frame.xr_frame.serial;
            ForgetResultLocked(frame.xr_frame.serial);
        }
        if (!aurora_vulkan_direct_set_targets(frame.xr_frame.serial, targets.data(), target_count,
                                              panel ? &panel_target : nullptr)) {
            {
                std::lock_guard lock(submission_mutex_);
                awaiting_token_ = 0;
            }
            ReleaseAcquired(pair, panel_swapchain);
            Fail("Aurora rejected the acquired OpenXR Vulkan swapchain images");
            return OpenXRBeginStatus::Error;
        }
        frame.expects_gpu_submission = true;
        return OpenXRBeginStatus::Ready;
    }

    OpenXRSubmissionStatus WaitForSubmission(const OpenXRBackendFrame& frame, uint32_t timeout_ms) {
        if (!frame.expects_gpu_submission) {
            return OpenXRSubmissionStatus::Success;
        }
        std::unique_lock lock(submission_mutex_);
        const uint64_t token = SubmissionToken(frame);
        const auto ready = [&] { return shutting_down_ || FindResultLocked(token) != nullptr; };
        if (timeout_ms == std::numeric_limits<uint32_t>::max()) {
            submission_cv_.wait(lock, ready);
        } else if (!submission_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
            return OpenXRSubmissionStatus::Timeout;
        }
        if (shutting_down_) {
            return OpenXRSubmissionStatus::ShuttingDown;
        }
        return FindResultLocked(token)->success ? OpenXRSubmissionStatus::Success : OpenXRSubmissionStatus::Failed;
    }

    // The token Aurora reports a frame's submission under: its packet's serial for the active
    // compositor frame of a packet, the frame's own serial otherwise.
    uint64_t SubmissionToken(const OpenXRBackendFrame& frame) const noexcept {
        return frame_active_ && frame.xr_frame.serial == active_frame_serial_ && active_packet_serial_ != 0
                   ? active_packet_serial_
                   : frame.xr_frame.serial;
    }

    bool TryCancelPendingFrame(OpenXRBackendFrame& frame) {
        if (!frame_active_ || !frame.expects_gpu_submission || frame.xr_frame.serial != active_frame_serial_) {
            return false;
        }
        if (!aurora_vulkan_direct_cancel(frame.xr_frame.serial)) {
            return false;
        }
        // A cancellation is serialized against Aurora's encode and fires no callback, so no GPU
        // work references the acquired images.
        std::lock_guard lock(submission_mutex_);
        if (awaiting_token_ == frame.xr_frame.serial) {
            awaiting_token_ = 0;
        }
        ForgetResultLocked(frame.xr_frame.serial);
        frame.expects_gpu_submission = false;
        return true;
    }

    // `overlap` (PF2_OVERLAP, render-first pacing on the pacing thread): locates, arms and publishes
    // the next packet (PrepareOverlapPacket) while this frame is still open, when waiting for this
    // frame's end would leave the next one too little time (OverlapLocateAt).
    bool FinishFrame(OpenXRBackendFrame& frame, bool submit_layer, const std::function<void()>& overlap = {}) {
        if (!frame_active_ || runtime_ == nullptr || frame.xr_frame.serial != active_frame_serial_) {
            return Fail("FinishFrame received a stale or inactive OpenXR frame token");
        }
        bool submission_unsafe = false;
        bool submission_done = false;
        uint64_t gpu_value = 0;
        int64_t submitted_ns = 0;
        {
            std::lock_guard lock(submission_mutex_);
            const SubmissionResult* result = FindResultLocked(SubmissionToken(frame));
            submission_unsafe = result != nullptr && result->unsafe;
            submission_done = result != nullptr && result->success;
            gpu_value = result != nullptr ? result->gpu_value : 0;
            submitted_ns = result != nullptr ? result->at_ns : 0;
        }
        if (submission_unsafe) {
            AbandonAcquiredSwapchains();
            Fail("Aurora's Vulkan stereo submission failed after GPU work may have been queued");
        }
        NoteOverlapSamples(active_packet_serial_, submitted_ns);
        // PF2_END_PHASE / PF2_END_DELAY_US: the release and the end together, at a phase of the
        // refresh; the layer keeps the display time and poses it was rendered for.
        const auto wake = pf2_wait_return_;
        end_phase_.NoteFrame(active_frame_.predicted_display_period);
        const bool phased = end_phase_.Enabled() && wake.time_since_epoch().count() != 0 &&
                            active_frame_.predicted_display_period > 0;
        const Clock::time_point target =
            phased ? end_phase_.Target(wake, active_frame_.predicted_display_period, nominal_period_)
                   : Clock::time_point{};
        // PF2_OVERLAP: the next packet before this frame's end, when it is due before it.
        const Clock::time_point locate_at = overlap && phased ? OverlapLocateAt(target) : Clock::time_point::max();
        bool overlapped = false;
        const auto run_overlap = [&] {
            overlapped = true;
            const Clock::time_point now = Clock::now();
            ++pf2_overlaps_;
            if (diagnostics::Enabled()) {
                pf2_overlap_lead_ms_.push_back(
                    static_cast<float>(std::chrono::duration<double, std::milli>(target - now).count()));
            }
            overlap();
        };
        if (submission_done) {
            // PF2_RELEASE_FENCE: not before the GPU finished writing the images.
            WaitGpu(gpu_value, locate_at, run_overlap);
        }
        if (!overlapped && locate_at < target) {
            std::this_thread::sleep_until(locate_at);
            run_overlap();
        }
        if (phased) {
            std::this_thread::sleep_until(target);
            if (diagnostics::Enabled()) {
                end_phase_.NoteEnd(std::chrono::steady_clock::now(), wake);
            }
        }
        const diagnostics::Stopwatch release_timer;
        const bool release_ok = ReleaseAcquiredSwapchains();
        if (frame.xr_frame.should_render && frame.xr_frame.views_valid) {
            diagnostics::OnSwapchainRelease(release_timer);
        }
        const bool position_valid = (frame.xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        const bool composition_pose_valid = frame.presentation.mode == OpenXRFrameMode::VirtualScreen || position_valid;
        const bool can_submit = submit_layer && release_ok && frame.xr_frame.should_render &&
                                frame.xr_frame.views_valid && frame.expects_gpu_submission && composition_pose_valid;
        if (submit_layer && !can_submit) {
            diagnostics::OnLayerRejected(
                diagnostics::ClassifyRejectedLayer(release_ok, frame.xr_frame.should_render, frame.xr_frame.views_valid));
        }
        // A packet queued behind this frame's holds the other pair's images: that pair becomes the
        // writable one whether or not this frame's layer is shown.
        const bool queued = queued_packet_serial_ != 0;
        if (can_submit || queued) {
            // xrEndFrame references the most recently released image of a swapchain, so keep the
            // displayed pair separate from the pair Aurora may write next.
            std::swap(eye_swapchains_, retained_swapchains_);
            if (frame.presentation.panel.requested || queued) {
                std::swap(panel_swapchain_, retained_panel_swapchain_);
            }
        }
        if (can_submit) {
            retained_panel_valid_ = frame.presentation.panel.requested;
            retained_frame_ = frame;
            retained_pose_time_ = active_pose_time_;
            retained_session_serial_ = active_origin_.session;
            retained_space_serial_ = active_origin_.space;
            have_retained_frame_ = true;
        } else if (queued) {
            retained_panel_valid_ = false;
        }
        const bool end_ok = EndRetainedFrame(can_submit);
        Pf2Tick();
        frame_active_ = false;
        active_frame_serial_ = 0;
        active_frame_ = {};
        active_pose_time_ = 0;
        frame.expects_gpu_submission = false;
        {
            std::lock_guard lock(submission_mutex_);
            if (awaiting_token_ == active_packet_serial_) {
                awaiting_token_ = 0;
            }
            ForgetResultLocked(active_packet_serial_);
        }
        active_packet_serial_ = 0;
        active_origin_ = {};
        if (queued) {
            pending_packet_serial_ = queued_packet_serial_;
            pending_origin_ = queued_origin_;
            queued_packet_serial_ = 0;
            queued_origin_ = {};
        }
        return release_ok && end_ok;
    }

    bool RepeatFrame(const OpenXRBackendFrame& frame) {
        if (!frame_active_ || runtime_ == nullptr || frame.xr_frame.serial != active_frame_serial_) {
            return Fail("RepeatFrame received a stale or inactive render token");
        }
        const bool end_ok = EndRetainedFrame(false);
        // EndFrame consumes the compositor token even when it fails.
        frame_active_ = false;
        if (!end_ok) {
            return Fail("OpenXR could not resubmit the retained frame");
        }
        if (runtime_->PollEvents() != OpenXREventStatus::Continue || !runtime_->IsSessionRunning() ||
            runtime_->ShouldExit()) {
            return Fail("OpenXR session stopped while waiting for stereo rendering");
        }
        if (runtime_->WaitFrame(active_frame_) != OpenXRFrameStatus::Ready || !runtime_->BeginFrame(active_frame_)) {
            return Fail("OpenXR could not start a retained-frame compositor cycle");
        }
        NoteDisplayTiming(active_frame_);
        frame_active_ = true;
        return true;
    }

    // ---- Render-first pacing (see openxr_vulkan_direct.h) -------------------------------------

    OpenXRBeginStatus PreparePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet) {
        if (pending_packet_serial_ != 0) {
            packet = {};
            packet.presentation = presentation;
            Fail("PreparePacket called with a packet pending");
            return OpenXRBeginStatus::Error;
        }
        // The eyes are ready after at most one game frame plus the encode and show at the first
        // display slot after that: two periods past the last predicted display time
        // (PF2_AHEAD_PERIODS replaces the two).
        const int ahead = pf2::Get().ahead_periods;
        const OpenXRBeginStatus located = LocatePacket(presentation, packet, ahead >= 0 ? static_cast<uint32_t>(ahead) : 2u);
        if (located != OpenXRBeginStatus::Ready || !packet.xr_frame.should_render || !packet.xr_frame.views_valid) {
            return located;
        }
        return ArmPacket(packet);
    }

    // The packet's poses only, `periods_ahead` display periods past the last compositor frame;
    // its images and Aurora's targets come with ArmPacket.
    OpenXRBeginStatus LocatePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet,
                                   uint32_t periods_ahead) {
        packet = {};
        packet.presentation = presentation;
        if (!bound_ || runtime_ == nullptr || frame_active_ || queued_packet_serial_ != 0) {
            Fail("LocatePacket called before binding, with a frame active or a packet already queued");
            return OpenXRBeginStatus::Error;
        }
        if (runtime_->ShouldExit()) {
            return OpenXRBeginStatus::ExitRequested;
        }
        if (!runtime_->IsSessionRunning()) {
            return OpenXRBeginStatus::SessionNotRunning;
        }
        // Before any frame ends on this presentation, the priming cycle below included.
        passthrough_.SetRunning(*runtime_, presentation.passthrough);
        if (timing_session_serial_ != runtime_->SessionRunSerial() || last_display_period_ <= 0) {
            // No display timing for this session yet: one compositor cycle learns it.
            const OpenXRBeginStatus primed = KeepAliveCycle();
            if (primed != OpenXRBeginStatus::Ready) {
                return primed;
            }
        }
        // A new size waits for a moment without a pending packet (its pair is in use).
        if (pending_packet_serial_ == 0 && !ResizeWritablePair()) {
            return OpenXRBeginStatus::Error;
        }
        // The pair this packet will be armed on: the writable one, or the retained one behind a
        // pending packet.
        const auto& pair = pending_packet_serial_ != 0 ? retained_swapchains_ : eye_swapchains_;
        packet.xr_frame.serial = next_packet_serial_++;
        // The display's own period, not xrWaitFrame's: after a missed refresh SteamVR reports 2 or
        // more periods, which located the poses that much further ahead (and stepped the game that
        // much at a time) although the next frame can be on time again.
        const XrDuration period = DisplayPeriod();
        packet.xr_frame.predicted_display_time = last_display_time_ + static_cast<XrDuration>(periods_ahead) * period;
        packet.xr_frame.predicted_display_period = period;
        packet.xr_frame.should_render = last_should_render_;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            packet.render_width[eye] = EyeWidth(pair, eye);
            packet.render_height[eye] = EyeHeight(pair, eye);
        }
        if (!packet.xr_frame.should_render) {
            return OpenXRBeginStatus::Ready;
        }
        if (!runtime_->LocateViewsAt(packet.xr_frame.predicted_display_time, packet.xr_frame)) {
            Fail("xrLocateViews failed for a packet");
            return OpenXRBeginStatus::Error;
        }
        // [pf2]: how long after the last xrWaitFrame returned the poses were located (PF2_LATE_LOCATE
        // moves this later; the display time they are for stays the same).
        if (diagnostics::Enabled() && pf2_wait_return_.time_since_epoch().count() != 0) {
            pf2_locate_ms_.push_back(static_cast<float>(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pf2_wait_return_).count()));
        }
        NoteLocated(packet.xr_frame.serial);
        if (!packet.xr_frame.views_valid) {
            return OpenXRBeginStatus::Ready;
        }
        located_origin_ = CurrentPoseOrigin();
        return OpenXRBeginStatus::Ready;
    }

    // PF2_OVERLAP: the next packet while this packet's compositor frame is still open (FinishFrame
    // runs this through its `overlap` callback). The poses are located for the same display time
    // the next plain cycle would have used (the last xrWaitFrame's, here this frame's, plus
    // PF2_AHEAD_PERIODS true periods), only earlier; the images come from the other swapchain pair
    // and the packet queues behind this frame's (it becomes the pending packet at its end), as a
    // pipelined packet does. Nothing is prepared (the packet expects no submission) while the eyes
    // resize, a pair has fewer than two images, a packet is already queued or there is nothing to
    // render; then the next cycle prepares its packet after the end, as before.
    OpenXRBeginStatus PrepareOverlapPacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet) {
        packet = {};
        packet.presentation = presentation;
        if (!bound_ || runtime_ == nullptr || !frame_active_ || active_packet_serial_ == 0 ||
            pending_packet_serial_ != 0 || queued_packet_serial_ != 0 || !last_should_render_ ||
            !SupportsPipelining() || EyesResizing() || !runtime_->IsSessionRunning() || runtime_->ShouldExit() ||
            timing_session_serial_ != runtime_->SessionRunSerial()) {
            return OpenXRBeginStatus::Ready;
        }
        const int ahead = pf2::Get().ahead_periods;
        const XrDuration period = DisplayPeriod();
        packet.xr_frame.serial = next_packet_serial_++;
        packet.xr_frame.predicted_display_time =
            last_display_time_ + static_cast<XrDuration>(ahead >= 0 ? ahead : 2) * period;
        packet.xr_frame.predicted_display_period = period;
        packet.xr_frame.should_render = true;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            packet.render_width[eye] = EyeWidth(retained_swapchains_, eye);
            packet.render_height[eye] = EyeHeight(retained_swapchains_, eye);
        }
        if (!runtime_->LocateViewsAt(packet.xr_frame.predicted_display_time, packet.xr_frame)) {
            Fail("xrLocateViews failed for an overlapped packet");
            return OpenXRBeginStatus::Error;
        }
        if (diagnostics::Enabled() && pf2_wait_return_.time_since_epoch().count() != 0) {
            pf2_locate_ms_.push_back(static_cast<float>(
                std::chrono::duration<double, std::milli>(Clock::now() - pf2_wait_return_).count()));
        }
        NoteLocated(packet.xr_frame.serial);
        if (!packet.xr_frame.views_valid) {
            return OpenXRBeginStatus::Ready;
        }
        located_origin_ = CurrentPoseOrigin();
        const OpenXRBeginStatus status = PrepareTargets(packet, retained_swapchains_, retained_panel_swapchain_);
        if (status == OpenXRBeginStatus::Ready) {
            queued_packet_serial_ = packet.xr_frame.serial;
            queued_origin_ = located_origin_;
        }
        return status;
    }

    // The located packet's images and Aurora's targets: on the writable pair with nothing
    // pending, otherwise on the retained pair behind the pending packet, whose eyes must have
    // been submitted (Aurora holds one packet's targets at a time).
    OpenXRBeginStatus ArmPacket(OpenXRBackendFrame& packet) {
        if (!bound_ || runtime_ == nullptr || frame_active_ || queued_packet_serial_ != 0) {
            Fail("ArmPacket called before binding, with a frame active or a packet already queued");
            return OpenXRBeginStatus::Error;
        }
        if (!packet.xr_frame.should_render || !packet.xr_frame.views_valid) {
            return OpenXRBeginStatus::Ready;
        }
        const bool behind = pending_packet_serial_ != 0;
        if (behind) {
            {
                std::lock_guard lock(submission_mutex_);
                if (FindResultLocked(pending_packet_serial_) == nullptr) {
                    Fail("ArmPacket called before the pending packet's eyes were submitted");
                    return OpenXRBeginStatus::Error;
                }
            }
            // While pipelined this is the only moment Aurora holds no targets, which retiring a
            // replaced pair needs; plain cycles count down in ResizeWritablePair instead.
            ReapRetiredPairs(false);
        }
        const OpenXRBeginStatus status = behind
                                             ? PrepareTargets(packet, retained_swapchains_, retained_panel_swapchain_)
                                             : PrepareTargets(packet, eye_swapchains_, panel_swapchain_);
        if (status == OpenXRBeginStatus::Ready) {
            (behind ? queued_packet_serial_ : pending_packet_serial_) = packet.xr_frame.serial;
            (behind ? queued_origin_ : pending_origin_) = located_origin_;
        }
        return status;
    }

    // Pipelined pacing acquires a queued packet's images from the pair on display, whose last
    // released image the compositor reads until this thread ends the next frame: a swapchain of
    // one image would never hand it out.
    bool SupportsPipelining() const {
        const auto two_images = [](const EyeSwapchain& swapchain) { return swapchain.images.size() >= 2; };
        // Packed: pair[0] alone holds both eyes.
        const auto pair_ok = [&](const std::array<EyeSwapchain, kOpenXREyeCount>& pair) {
            return two_images(pair[0]) && (packed_ || two_images(pair[1]));
        };
        return bound_ && pair_ok(eye_swapchains_) && pair_ok(retained_swapchains_) &&
               (!panel_swapchains_ready_ || (two_images(panel_swapchain_) && two_images(retained_panel_swapchain_)));
    }

    // A new size still waits for a pair (vr/openxr_eye_resize.h): pipelined pacing stays paused
    // meanwhile, since a pair is only rebuilt with no packet in flight.
    bool EyesResizing() const {
        bool differ = false;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            const OpenXREyeSize written{EyeWidth(eye_swapchains_, eye), EyeHeight(eye_swapchains_, eye)};
            const OpenXREyeSize shown{EyeWidth(retained_swapchains_, eye), EyeHeight(retained_swapchains_, eye)};
            differ = differ || written != eye_size_[eye] || shown != eye_size_[eye];
        }
        return differ && eye_resize_gate_.Pending();
    }

    bool TryCancelPendingPacket(OpenXRBackendFrame& packet) {
        const uint64_t serial = packet.xr_frame.serial;
        const bool queued = queued_packet_serial_ != 0 && serial == queued_packet_serial_;
        const bool pending = !queued && pending_packet_serial_ != 0 && serial == pending_packet_serial_;
        if ((!queued && !pending) || frame_active_ || !packet.expects_gpu_submission ||
            !aurora_vulkan_direct_cancel(serial)) {
            return false;
        }
        {
            std::lock_guard lock(submission_mutex_);
            if (awaiting_token_ == serial) {
                awaiting_token_ = 0;
            }
            ForgetResultLocked(serial);
        }
        packet.expects_gpu_submission = false;
        uint64_t& slot = queued ? queued_packet_serial_ : pending_packet_serial_;
        slot = 0;
        const diagnostics::Stopwatch release_timer;
        const bool released =
            queued ? ReleaseAcquired(retained_swapchains_, retained_panel_swapchain_) : ReleaseAcquiredSwapchains();
        diagnostics::OnSwapchainRelease(release_timer);
        // A release error is fatal; keep it visible to the next prepare rather than letting it
        // hand Aurora new targets over still-acquired images.
        if (!released) {
            slot = serial;
        }
        return true;
    }

    OpenXRBeginStatus BeginFrameForPacket(const OpenXRBackendFrame& packet, OpenXRBackendFrame& frame) {
        frame = {};
        if (!bound_ || runtime_ == nullptr || frame_active_ || pending_packet_serial_ == 0 ||
            packet.xr_frame.serial != pending_packet_serial_ || !packet.expects_gpu_submission ||
            WaitForSubmission(packet, 0) != OpenXRSubmissionStatus::Success) {
            Fail("BeginFrameForPacket requires the completed current Vulkan packet");
            return OpenXRBeginStatus::Error;
        }
        const OpenXRBeginStatus status = BeginCompositorCycle();
        if (status != OpenXRBeginStatus::Ready) {
            // The packet completed; a stopped session must not strand it, nor a packet queued
            // behind it, and block the next prepare after the session is ready again.
            if (status == OpenXRBeginStatus::SessionNotRunning) {
                WaitGpuForToken(packet.xr_frame.serial);
                bool released = ReleaseAcquiredSwapchains();
                if (queued_packet_serial_ != 0) {
                    released = DropQueuedPacket() && released;
                }
                pending_packet_serial_ = 0;
                pending_origin_ = {};
                std::lock_guard lock(submission_mutex_);
                awaiting_token_ = 0;
                submission_results_ = {};
                if (!released) {
                    return OpenXRBeginStatus::Error;
                }
            }
            return status;
        }
        frame = packet;
        active_pose_time_ = packet.xr_frame.predicted_display_time;
        NotePoseLead(packet.xr_frame.predicted_display_time, active_frame_);
        // The current compositor token and timing with the packet's render poses; the packet's
        // own serial stays the name of its submission (SubmissionToken).
        frame.xr_frame.serial = active_frame_.serial;
        frame.xr_frame.predicted_display_time = active_frame_.predicted_display_time;
        frame.xr_frame.predicted_display_period = active_frame_.predicted_display_period;
        frame.xr_frame.should_render = active_frame_.should_render;
        active_frame_serial_ = frame.xr_frame.serial;
        active_packet_serial_ = packet.xr_frame.serial;
        active_origin_ = pending_origin_;
        pending_packet_serial_ = 0;
        pending_origin_ = {};
        return OpenXRBeginStatus::Ready;
    }

    OpenXRSubmissionStatus CopyRenderedEyes(const OpenXRBackendFrame& frame) {
        if (!frame_active_ || frame.xr_frame.serial != active_frame_serial_) {
            Fail("CopyRenderedEyes received a stale Vulkan frame");
            return OpenXRSubmissionStatus::Failed;
        }
        // Aurora already drew the eyes into the images on the session's queue.
        return WaitForSubmission(frame, 0);
    }

    OpenXRBeginStatus KeepAliveCycle() {
        if (!bound_ || runtime_ == nullptr || frame_active_) {
            Fail("KeepAliveCycle called before binding or with an active frame");
            return OpenXRBeginStatus::Error;
        }
        const OpenXRBeginStatus status = BeginCompositorCycle();
        if (status != OpenXRBeginStatus::Ready) {
            return status;
        }
        const bool ended = EndRetainedFrame(false);
        frame_active_ = false;
        active_frame_ = {};
        if (!ended) {
            Fail("OpenXR could not resubmit the retained Vulkan frame");
            return OpenXRBeginStatus::Error;
        }
        return OpenXRBeginStatus::Ready;
    }

    bool Shutdown() {
        if (shutdown_unsafe_) {
            return false;
        }
        if (!ReleaseSession()) {
            return false;
        }
        if (hooks_installed_) {
            aurora_vulkan_direct_configure(nullptr);
            hooks_installed_ = false;
        }
        vk_instance_ = VK_NULL_HANDLE;
        instance_api_version_ = 0;
        timeline_semaphores_ = false;
        bound_ = false;
        requirements_queried_ = false;
        runtime_ = nullptr;
        return true;
    }

    // Session recovery (OpenXRIntegration::RecoverSessionOnProducer): the runtime ended the
    // session early, so it is released like Shutdown() does and a new one is created on the same
    // Dawn device. Dawn's instance, device and hooks stay as they are. On the game thread with
    // Aurora's worker idle, like Shutdown().
    bool RecreateSession(OpenXRRuntime& runtime) {
        ClearError();
        if (!bound_ || runtime_ != &runtime || shutdown_unsafe_) {
            return Fail("OpenXR Vulkan backend has no session to recreate");
        }
        if (!ReleaseSession()) {
            return false;
        }
        bound_ = false;
        if (!runtime.PrepareSessionRecreate()) {
            return Fail("OpenXR instance is lost; a new session needs a new instance");
        }
        {
            std::lock_guard lock(submission_mutex_);
            shutting_down_ = false;
            awaiting_token_ = 0;
            submission_results_ = {};
        }
        AuroraDawnVulkanHandles handles{};
        int64_t format = 0;
        if (!aurora_vulkan_direct_get_handles(&handles, &format) || handles.instance != vk_instance_) {
            return Fail("Aurora no longer reports the Dawn Vulkan device the session was made on");
        }
        return BindSession(runtime, handles);
    }

    // Everything of Shutdown() that belongs to the session: the bridge, the fence, the swapchains
    // and the session itself. False when Dawn's queue could not be drained.
    bool ReleaseSession() {
        {
            std::lock_guard lock(submission_mutex_);
            shutting_down_ = true;
        }
        submission_cv_.notify_all();

        bool bridge_drained = true;
        if (bridge_enabled_) {
            bridge_drained = aurora_vulkan_direct_disable();
            bridge_enabled_ = false;
        }
        if (!bridge_drained) {
            AbandonAcquiredSwapchains();
            AbandonAcquired(retained_swapchains_, retained_panel_swapchain_);
            shutdown_unsafe_ = true;
            Fail("Vulkan queue completion is unknown; retaining the OpenXR session and graphics owners");
            return false;
        }
        // The drain retired every copy into an acquired image, including one after a failed
        // submission.
        fence_.Stop();
        AllowAcquiredSwapchainsAfterGpuDrain();
        ReleaseAcquiredSwapchains();
        // A packet queued behind the pending one holds the retained pair's images.
        ReleaseAcquired(retained_swapchains_, retained_panel_swapchain_);
        queued_packet_serial_ = 0;
        active_packet_serial_ = 0;
        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = 0;
            submission_results_ = {};
        }
        if (frame_active_ && runtime_ != nullptr) {
            // On the XR owner thread with Aurora's worker idle: closing an abandoned frame is safe.
            if (runtime_->IsSessionRunning()) {
                runtime_->EndFrameWithoutLayers(active_frame_);
            }
            frame_active_ = false;
            active_frame_serial_ = 0;
            active_frame_ = {};
        }
        DestroySwapchains();
        pending_packet_serial_ = 0;
        last_display_period_ = 0;
        nominal_period_ = 0;
        // Its handles belong to the session.
        passthrough_.Destroy();
        if (owns_session_ && runtime_ != nullptr) {
            runtime_->DestroySession();
            owns_session_ = false;
        }
        if (runtime_ != nullptr) {
            runtime_->SetGraphicsQueueGuard(nullptr, nullptr);
        }
        return true;
    }

    bool IsBound() const { return bound_; }
    bool PanelLayerAvailable() const { return !panel_layer_failed_; }
    const OpenXRVulkanGraphicsRequirements& GraphicsRequirements() const { return requirements_; }
    int64_t SwapchainFormat() const { return static_cast<int64_t>(swapchain_format_); }
    const std::string& LastError() const { return last_error_; }

private:
    // The session run and reference-space change a packet's poses were located in: the retained
    // layer is dropped once either changes (EndRetainedFrame). They travel with the packet, since
    // a pipelined packet is located before the previous one's frame ends.
    struct PoseOrigin {
        uint64_t session = 0;
        uint64_t space = 0;
    };

    PoseOrigin CurrentPoseOrigin() const {
        return {runtime_->SessionRunSerial(), runtime_->LastReferenceSpaceChange().serial};
    }

    // The packet queued behind the pending one, given up before its compositor frame (the session
    // stopped). Aurora may be encoding it (the cancel fails then): its submission is awaited
    // briefly, since images left to xrDestroySession would also keep their pair from ever being
    // resized. False on a release error.
    bool DropQueuedPacket() {
        const uint64_t token = queued_packet_serial_;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        bool safe = false;
        for (;;) {
            if (aurora_vulkan_direct_cancel(token)) {
                safe = true;
                break;
            }
            // Retried every few milliseconds: the worker may have held the bridge for an encode
            // that declined the packet, which fires no callback.
            std::unique_lock lock(submission_mutex_);
            submission_cv_.wait_for(lock, std::chrono::milliseconds(2),
                                    [&] { return shutting_down_ || FindResultLocked(token) != nullptr; });
            if (const SubmissionResult* result = FindResultLocked(token)) {
                safe = result->success;
                break;
            }
            if (shutting_down_ || std::chrono::steady_clock::now() >= deadline) {
                break;
            }
        }
        queued_packet_serial_ = 0;
        queued_origin_ = {};
        if (!safe) {
            AbandonAcquired(retained_swapchains_, retained_panel_swapchain_);
            return true;
        }
        WaitGpuForToken(token);
        return ReleaseAcquired(retained_swapchains_, retained_panel_swapchain_);
    }

    // The display's true refresh period: the compositor grid's when it divides the session's,
    // else the session's (never xrWaitFrame's doubled period after a missed refresh).
    XrDuration DisplayPeriod() const {
        const XrDuration nominal = nominal_period_ > 0 ? nominal_period_ : last_display_period_;
        return end_phase_.Enabled() ? end_phase_.TruePeriod(nominal) : nominal;
    }

    void NoteDisplayTiming(const OpenXRFrame& frame) noexcept {
        last_display_time_ = frame.predicted_display_time;
        last_display_period_ = frame.predicted_display_period;
        last_should_render_ = frame.should_render;
        if (timing_session_serial_ != runtime_->SessionRunSerial()) {
            nominal_period_ = 0;
        }
        timing_session_serial_ = runtime_->SessionRunSerial();
        // The session's refresh period: the shortest xrWaitFrame reported, kept while later ones are
        // whole multiples of it (missed refreshes), replaced by any other (a new refresh rate).
        if (const int64_t p = frame.predicted_display_period; p > 0) {
            if (nominal_period_ <= 0 || p < nominal_period_ - nominal_period_ / 100 ||
                EndPhaseLock::Multiple(p, nominal_period_) == 0) {
                nominal_period_ = p;
            }
        }
        pf2_wait_return_ = std::chrono::steady_clock::now();
    }

    OpenXRBeginStatus BeginCompositorCycle() {
        const OpenXRFrameStatus status = runtime_->WaitFrame(active_frame_);
        if (status != OpenXRFrameStatus::Ready) {
            if (status == OpenXRFrameStatus::Error) {
                Fail(BeginStatusOperation(status));
            }
            return BeginStatusFor(status);
        }
        NoteDisplayTiming(active_frame_);
        if (!runtime_->BeginFrame(active_frame_)) {
            Fail("xrBeginFrame failed for a Vulkan compositor cycle");
            return OpenXRBeginStatus::Error;
        }
        frame_active_ = true;
        return OpenXRBeginStatus::Ready;
    }

    // fresh: the retained layer was completed for this call rather than repeated. An xrEndFrame
    // that SteamVR refuses with XR_ERROR_TIME_INVALID (seen once right after VISIBLE at session
    // start) is a dropped frame, not the session's end: logged (the first, then every 100th) and
    // tolerated unless 90 frames in a row are refused.
    bool EndRetainedFrame(bool fresh) {
        const bool ended = EndRetainedFrameOnce(fresh);
        if (ended) {
            time_invalid_run_ = 0;
            return true;
        }
        if (runtime_ == nullptr || runtime_->LastError().result != XR_ERROR_TIME_INVALID) {
            return false;
        }
        ++time_invalid_total_;
        ++time_invalid_run_;
        if (time_invalid_total_ == 1 || time_invalid_total_ % 100 == 0) {
            std::ostringstream message;
            message << "xrEndFrame refused the display time (XR_ERROR_TIME_INVALID): frame dropped ("
                    << time_invalid_total_ << " so far, " << time_invalid_run_ << " in a row; end display time "
                    << pf2_last_end_time_ << ", the frame's predicted " << active_frame_.predicted_display_time << ")";
            Log(OpenXRLogLevel::Warning, message.str());
        }
        return time_invalid_run_ < 90;
    }

    bool EndRetainedFrameOnce(bool fresh) {
        if (!runtime_->IsSessionRunning()) {
            return true;
        }
        // Old poses cannot be reused after the runtime changes their coordinate system, nor
        // content across session restarts.
        const bool session_changed = retained_session_serial_ != runtime_->SessionRunSerial();
        if (session_changed || retained_space_serial_ != runtime_->LastReferenceSpaceChange().serial) {
            if (have_retained_frame_) {
                diagnostics::OnRetainedLayerDiscarded(session_changed ? diagnostics::DiscardReason::SessionRestarted
                                                                      : diagnostics::DiscardReason::ReferenceSpaceChanged);
            }
            have_retained_frame_ = false;
        }
        if (!have_retained_frame_ || !active_frame_.should_render) {
            diagnostics::OnEmptyFrame(!active_frame_.should_render ? diagnostics::EmptyFrameReason::ShouldRenderOff
                                                                    : diagnostics::EmptyFrameReason::NoRetainedLayer);
            // Outside immersive play the room stays in view until there is an image to show (at
            // start and after a recenter), rather than flashing black.
            pf2_last_end_time_ = active_frame_.predicted_display_time;
            if (const XrCompositionLayerBaseHeader* passthrough = passthrough_.Layer()) {
                return runtime_->EndFrame(active_frame_, &passthrough, 1);
            }
            return runtime_->EndFrameWithoutLayers(active_frame_);
        }
        diagnostics::OnLayer(fresh);
        pf2_fresh_layers_ += fresh ? 1u : 0u;
        const auto& frame = retained_frame_;
        // PF2_AHEAD_PERIODS: a fresh layer ends with the display time its poses were located for.
        OpenXRFrame end_frame = active_frame_;
        // Only a time SteamVR can take: once the session's period is known, after the last end's
        // time, and within 1.25 periods of this frame's predicted one (a packet located before a
        // stall or before the waits that kept the compositor fed meanwhile is not). Otherwise the
        // frame keeps xrWaitFrame's time, as without PF2_AHEAD_PERIODS.
        if (fresh && pf2::Get().ahead_periods >= 0 && retained_pose_time_ > 0) {
            const XrDuration period = DisplayPeriod();
            const XrDuration delta = retained_pose_time_ - active_frame_.predicted_display_time;
            if (nominal_period_ > 0 && period > 0 && retained_pose_time_ > pf2_last_end_time_ &&
                std::llabs(delta) <= period + period / 4) {
                NoteEndOverride(delta);
                end_frame.predicted_display_time = retained_pose_time_;
            } else {
                ++pf2_end_kept_;
            }
        }
        if (frame.presentation.mode == OpenXRFrameMode::VirtualScreen) {
            XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.layerFlags = 0;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = retained_swapchains_[0].handle;
            // Only the picture, not the black bands letterboxing it into the eye-sized image: they
            // would frame it against the passthrough view. Packed: the left half holds it.
            const uint32_t image_width = EyeWidth(retained_swapchains_, 0);
            quad.subImage.imageRect = OpenXRVirtualScreenContentRect(image_width, EyeHeight(retained_swapchains_, 0),
                                                                     frame.presentation.quad_content_aspect);
            quad.subImage.imageArrayIndex = 0;
            if (frame.presentation.quad_anchored) {
                quad.space = runtime_->AppSpace();
                quad.pose = frame.presentation.quad_pose;
            } else {
                quad.space = runtime_->ViewSpace();
                quad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
                quad.pose.position = {0.0f, 0.0f, -std::max(0.25f, frame.presentation.quad_distance_meters)};
            }
            // The whole image would be quad_width_meters across: the crop keeps that size per pixel,
            // so the picture stays exactly where the pointer and the settings panel expect it.
            const float meters_per_pixel =
                std::max(0.25f, frame.presentation.quad_width_meters) / static_cast<float>(image_width);
            quad.size.width = meters_per_pixel * static_cast<float>(quad.subImage.imageRect.extent.width);
            quad.size.height = meters_per_pixel * static_cast<float>(quad.subImage.imageRect.extent.height);
            return EndFrameWithPanel(frame, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad), end_frame);
        }
        std::array<XrCompositionLayerProjectionView, kOpenXREyeCount> views{};
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            views[eye].pose.orientation = frame.xr_frame.views[eye].pose.orientation;
            views[eye].pose.position = frame.xr_frame.views[eye].pose.position;
            views[eye].fov = frame.xr_frame.views[eye].fov;
            views[eye].subImage.swapchain = EyeChain(retained_swapchains_, eye).handle;
            // The part of the image the eye was rendered into: all of it (packed: its half), except
            // for the immersive window's eyes, which are only the window (OpenXRPresentation::window_eyes).
            views[eye].subImage.imageRect = {
                {static_cast<int32_t>(EyeX(retained_swapchains_, eye)), 0},
                {static_cast<int32_t>(std::min(frame.render_width[eye], EyeWidth(retained_swapchains_, eye))),
                 static_cast<int32_t>(std::min(frame.render_height[eye], EyeHeight(retained_swapchains_, eye)))}};
            views[eye].subImage.imageArrayIndex = 0;
        }
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        // The immersive window's eyes are transparent outside the window (premultiplied alpha), so
        // the room shows around it; otherwise the world covers the whole view and alpha is ignored.
        projection.layerFlags =
            frame.presentation.immersive_window ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
        projection.space = runtime_->AppSpace();
        projection.viewCount = kOpenXREyeCount;
        projection.views = views.data();
        return EndFrameWithPanel(frame, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection), end_frame);
    }

    // Ends the compositor frame with the scene's layer: over the room's camera view while that
    // runs and the scene is the virtual screen or the immersive window (never under a fully
    // immersive projection, which covers the whole view), and, while the retained frame
    // rendered it, under the settings panel's layer.
    bool EndFrameWithPanel(const OpenXRBackendFrame& frame, const XrCompositionLayerBaseHeader* scene,
                           const OpenXRFrame& end_frame) {
        const auto& panel = frame.presentation.panel;
        std::array<XrCompositionLayerQuad, kOpenXRPanelMaxLayers> panel_quads{};
        const XrCompositionLayerBaseHeader* layers[2 + kOpenXRPanelMaxLayers] = {};
        uint32_t count = 0;
        if (const XrCompositionLayerBaseHeader* passthrough = passthrough_.Layer();
            passthrough != nullptr &&
            (frame.presentation.mode == OpenXRFrameMode::VirtualScreen || frame.presentation.immersive_window)) {
            layers[count++] = passthrough;
        }
        layers[count++] = scene;
        if (retained_panel_valid_ && panel.requested && panel.placed) {
            const uint32_t quads =
                OpenXRPanelQuadLayers(panel, runtime_->AppSpace(), retained_panel_swapchain_.handle, panel_quads);
            for (uint32_t i = 0; i < quads; ++i) {
                layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&panel_quads[i]);
            }
        }
        pf2_last_end_time_ = end_frame.predicted_display_time;
        return runtime_->EndFrame(end_frame, layers, count);
    }

    bool SelectSwapchainFormat() {
        const auto& formats = runtime_->SwapchainFormats();
        // Aurora's UNORM target holds gamma-encoded bytes; declaring the sRGB sibling makes the
        // compositor decode them instead of treating them as linear light. Copies between UNORM
        // and sRGB siblings are raw.
        const VkFormat srgb = SrgbSibling(aurora_format_);
        if (srgb != VK_FORMAT_UNDEFINED &&
            std::find(formats.begin(), formats.end(), static_cast<int64_t>(srgb)) != formats.end()) {
            swapchain_format_ = srgb;
            return true;
        }
        if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(aurora_format_)) != formats.end()) {
            swapchain_format_ = aurora_format_;
            return true;
        }
        const auto compatible = std::find_if(formats.begin(), formats.end(), [&](int64_t format) {
            return SameVkCopyFamily(aurora_format_, static_cast<VkFormat>(format));
        });
        if (compatible == formats.end()) {
            return Fail("OpenXR offered no swapchain format copy-compatible with Aurora's Vulkan colour format");
        }
        swapchain_format_ = static_cast<VkFormat>(*compatible);
        return true;
    }

    bool CreateSwapchains() { return CreateSwapchainPair(eye_swapchains_) && CreateSwapchainPair(retained_swapchains_); }

    bool CreateSwapchainPair(std::array<EyeSwapchain, kOpenXREyeCount>& pair) {
        if (packed_) {
            // Both eyes side by side (FitEyeSize made them one size), eye N at x = N * eye width.
            pair[1] = {};
            return CreateSwapchain(pair[0], 2 * eye_size_[0].width, eye_size_[0].height, "packed eyes");
        }
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            if (!CreateSwapchain(pair[eye], eye_size_[eye].width, eye_size_[eye].height,
                                 eye == 0 ? "left eye" : "right eye")) {
                return false;
            }
        }
        return true;
    }

    // As the D3D12 backend's. Aurora wraps each swapchain VkImage for Dawn, and the runtime may
    // hand the old handles out again, so the wraps go with the swapchains (ReapRetiredPairs).
    bool ResizeWritablePair() {
        ReapRetiredPairs(false);
        std::array<OpenXREyeSize, kOpenXREyeCount> current{};
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            if (eye_swapchains_[eye].acquired) {
                return true;
            }
            current[eye] = {EyeWidth(eye_swapchains_, eye), EyeHeight(eye_swapchains_, eye)};
        }
        if (current == eye_size_) {
            return true;
        }
        // Not while a replaced pair lives on, nor after this request was refused twice
        // (vr/openxr_eye_resize.h).
        if (!eye_resize_gate_.MayRebuild(!retired_pairs_.empty())) {
            return true;
        }
        std::array<EyeSwapchain, kOpenXREyeCount> replacement{};
        if (!CreateSwapchainPair(replacement)) {
            DestroySwapchainPair(replacement);
            std::ostringstream message;
            message << "OpenXR Vulkan eyes stay " << current[0].width << 'x' << current[0].height
                    << ": the runtime could not make " << eye_size_[0].width << 'x' << eye_size_[0].height
                    << " swapchains (" << last_error_ << ')';
            Log(OpenXRLogLevel::Warning, message.str());
            ClearError();
            // Back to this pair's size, which also returns the other pair to it if it was rebuilt.
            eye_size_ = current;
            eye_resize_gate_.Refused();
            return true;
        }
        // The compositor may still be reading the old pair (kOpenXRRetiredSwapchainCycles).
        retired_pairs_.push_back({std::move(eye_swapchains_), kOpenXRRetiredSwapchainCycles});
        eye_swapchains_ = std::move(replacement);
        std::ostringstream message;
        message << "OpenXR Vulkan eyes resized: " << eye_size_[0].width << 'x' << eye_size_[0].height << " / "
                << eye_size_[1].width << 'x' << eye_size_[1].height;
        Log(OpenXRLogLevel::Info, message.str());
        return true;
    }

    // Aurora drains Dawn's queue and drops its wraps of the images (under Dawn's device lock, so
    // before the lock is taken to destroy them).
    void ReapRetiredPairs(bool all) {
        for (auto it = retired_pairs_.begin(); it != retired_pairs_.end();) {
            if (!all && --it->cycles_left != 0) {
                ++it;
                continue;
            }
            std::vector<uint64_t> images;
            for (const EyeSwapchain& swapchain : it->swapchains) {
                for (const auto& image : swapchain.images) {
                    images.push_back(reinterpret_cast<uint64_t>(image.image));
                }
            }
            if (aurora_vulkan_direct_forget_targets(images.data(), static_cast<uint32_t>(images.size()))) {
                DestroySwapchainPair(it->swapchains);
            } else {
                Log(OpenXRLogLevel::Warning,
                    "Aurora could not retire its copies into a replaced Vulkan eye swapchain pair; "
                    "deferring its destruction to xrDestroySession");
            }
            it = retired_pairs_.erase(it);
        }
    }

    bool CreateSwapchain(EyeSwapchain& swapchain, uint32_t width, uint32_t height, const char* what) {
        swapchain.width = width;
        swapchain.height = height;
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        create.format = static_cast<int64_t>(swapchain_format_);
        create.sampleCount = 1;
        create.width = swapchain.width;
        create.height = swapchain.height;
        create.faceCount = 1;
        create.arraySize = 1;
        create.mipCount = 1;
        XrResult result;
        {
            // Mid-session (the panel's swapchains, a new render resolution) Dawn's worker is
            // submitting on the queue a runtime may use to prepare the new images.
            const auto queue_guard = runtime_->LockGraphicsQueue();
            result = xrCreateSwapchain(runtime_->Session(), &create, &swapchain.handle);
        }
        ObserveResult(result);
        if (XR_FAILED(result)) {
            std::ostringstream message;
            message << "xrCreateSwapchain failed for the Vulkan " << what << " swapchain (" << result << ')';
            return Fail(message.str());
        }
        uint32_t count = 0;
        result = xrEnumerateSwapchainImages(swapchain.handle, 0, &count, nullptr);
        ObserveResult(result);
        if (XR_FAILED(result) || count == 0) {
            return Fail("OpenXR returned no Vulkan swapchain images");
        }
        swapchain.images.resize(count);
        for (auto& image : swapchain.images) {
            image = {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR};
        }
        result = xrEnumerateSwapchainImages(swapchain.handle, count, &count,
                                            reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data()));
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrEnumerateSwapchainImages failed for a Vulkan eye swapchain");
        }
        return true;
    }

    bool AcquireSwapchain(EyeSwapchain& swapchain) {
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult result;
        {
            const auto queue_guard = runtime_->LockGraphicsQueue();
            result = xrAcquireSwapchainImage(swapchain.handle, &acquire, &swapchain.acquired_index);
        }
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrAcquireSwapchainImage failed for a Vulkan eye swapchain");
        }
        swapchain.acquired = true;
        swapchain.waited = false;
        swapchain.release_forbidden = false;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        // The runtime does not use the queue here: Dawn keeps submitting while the compositor
        // frees the image.
        result = xrWaitSwapchainImage(swapchain.handle, &wait);
        ObserveResult(result);
        if (result == XR_TIMEOUT_EXPIRED) {
            return Fail("xrWaitSwapchainImage unexpectedly timed out for a Vulkan eye swapchain");
        }
        if (XR_FAILED(result)) {
            return Fail("xrWaitSwapchainImage failed for a Vulkan eye swapchain");
        }
        swapchain.waited = true;
        if (swapchain.acquired_index >= swapchain.images.size()) {
            return Fail("OpenXR returned an out-of-range Vulkan swapchain image index");
        }
        return true;
    }

    bool ReleaseAcquiredSwapchains() { return ReleaseAcquired(eye_swapchains_, panel_swapchain_); }

    bool ReleaseAcquired(std::array<EyeSwapchain, kOpenXREyeCount>& pair, EyeSwapchain& panel_swapchain) {
        const auto queue_guard =
            runtime_ != nullptr ? runtime_->LockGraphicsQueue() : OpenXRRuntime::GraphicsQueueGuard{nullptr, nullptr};
        bool success = true;
        for (auto& swapchain : pair) {
            success = ReleaseSwapchain(swapchain) && success;
        }
        return ReleaseSwapchain(panel_swapchain) && success;
    }

    bool ReleaseSwapchain(EyeSwapchain& swapchain) {
        if (!swapchain.acquired || swapchain.handle == XR_NULL_HANDLE) {
            return true;
        }
        if (!swapchain.waited) {
            // OpenXR only permits release after a successful wait. Keep the image acquired and let
            // session teardown destroy the child.
            Log(OpenXRLogLevel::Warning, "cannot release an OpenXR Vulkan image whose wait did not complete");
            return false;
        }
        if (swapchain.release_forbidden) {
            // Aurora reported a failed submission after it may have queued GPU work: without a
            // trustworthy fence the release could race it, so leave the image for xrDestroySession.
            Log(OpenXRLogLevel::Warning, "deferring an OpenXR Vulkan image after an unsafe GPU submission");
            return false;
        }
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const XrResult result = xrReleaseSwapchainImage(swapchain.handle, &release);
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrReleaseSwapchainImage failed for a Vulkan swapchain");
        }
        swapchain.acquired = false;
        swapchain.waited = false;
        return true;
    }

    void AbandonAcquiredSwapchains() noexcept { AbandonAcquired(eye_swapchains_, panel_swapchain_); }

    void AbandonAcquired(std::array<EyeSwapchain, kOpenXREyeCount>& pair, EyeSwapchain& panel_swapchain) noexcept {
        for (auto* swapchain : {&pair[0], &pair[1], &panel_swapchain}) {
            if (swapchain->acquired) {
                swapchain->release_forbidden = true;
            }
        }
    }

    void AllowAcquiredSwapchainsAfterGpuDrain() noexcept {
        for (auto* swapchain : {&eye_swapchains_[0], &eye_swapchains_[1], &retained_swapchains_[0],
                                &retained_swapchains_[1], &panel_swapchain_, &retained_panel_swapchain_}) {
            if (swapchain->acquired) {
                swapchain->release_forbidden = false;
            }
        }
    }

    // The settings panel's swapchain pair, made the first time the panel opens. A failure is
    // logged once and the VR menu stays closed: it has no other way into the headset.
    bool EnsurePanelSwapchains() {
        if (panel_swapchains_ready_) {
            return true;
        }
        if (panel_layer_failed_) {
            return false;
        }
        if (CreateSwapchain(panel_swapchain_, kOpenXRPanelLayerWidth, kOpenXRPanelLayerHeight, "settings panel") &&
            CreateSwapchain(retained_panel_swapchain_, kOpenXRPanelLayerWidth, kOpenXRPanelLayerHeight,
                            "settings panel")) {
            panel_swapchains_ready_ = true;
            Log(OpenXRLogLevel::Info, "OpenXR VR menu layer ready");
            return true;
        }
        DestroyPanelSwapchains();
        panel_layer_failed_ = true;
        Log(OpenXRLogLevel::Warning, "the VR menu could not get its own OpenXR layer; it cannot be shown");
        return false;
    }

    void DestroySwapchainHandle(EyeSwapchain& swapchain, const char* what) {
        if (swapchain.handle != XR_NULL_HANDLE && !swapchain.acquired) {
            const auto queue_guard = runtime_ != nullptr ? runtime_->LockGraphicsQueue()
                                                         : OpenXRRuntime::GraphicsQueueGuard{nullptr, nullptr};
            xrDestroySwapchain(swapchain.handle);
        } else if (swapchain.acquired) {
            std::ostringstream message;
            message << "Vulkan " << what
                    << " swapchain still owns an acquired image; deferring its destruction to xrDestroySession";
            Log(OpenXRLogLevel::Warning, message.str());
        }
        swapchain = {};
    }

    void DestroyPanelSwapchains() {
        DestroySwapchainHandle(panel_swapchain_, "panel");
        DestroySwapchainHandle(retained_panel_swapchain_, "panel");
        panel_swapchains_ready_ = false;
        retained_panel_valid_ = false;
    }

    void DestroySwapchains() {
        DestroyPanelSwapchains();
        ReapRetiredPairs(true);
        DestroySwapchainPair(eye_swapchains_);
        DestroySwapchainPair(retained_swapchains_);
        have_retained_frame_ = false;
        retained_frame_ = {};
        swapchain_format_ = VK_FORMAT_UNDEFINED;
    }

    void DestroySwapchainPair(std::array<EyeSwapchain, kOpenXREyeCount>& pair) {
        for (auto& swapchain : pair) {
            DestroySwapchainHandle(swapchain, "eye");
        }
    }

    void EndActiveFrameWithoutLayers(const OpenXRFrame& frame) {
        if (frame_active_ && runtime_ != nullptr && runtime_->IsSessionRunning()) {
            runtime_->EndFrameWithoutLayers(frame);
        }
        frame_active_ = false;
        active_frame_serial_ = 0;
        active_frame_ = {};
    }

    void ObserveResult(XrResult result) noexcept {
        if (runtime_ != nullptr) {
            runtime_->ObserveResult(result);
        }
    }

    static void OnAuroraSubmitted(uint64_t token, bool success, void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        if (self == nullptr) {
            return;
        }
        // PF2_RELEASE_FENCE: marks the queue right after the set's work (Dawn's queue lock).
        const uint64_t gpu_value =
            success && self->fence_.CurrentMode() == GpuFence::Mode::Timeline ? self->fence_.Signal() : 0;
        {
            std::lock_guard lock(self->submission_mutex_);
            if (token != self->awaiting_token_) {
                return;
            }
            self->awaiting_token_ = 0;
            self->submission_results_[1] = self->submission_results_[0];
            self->submission_results_[0] = {token, success, !success, gpu_value,
                                            std::chrono::steady_clock::now().time_since_epoch().count()};
        }
        self->submission_cv_.notify_all();
    }

    // The results of the last two submissions, by token (under submission_mutex_): a pipelined
    // packet's may arrive while the previous packet's is still awaited.
    struct SubmissionResult {
        uint64_t token = 0;
        bool success = false;
        bool unsafe = false;
        // PF2_RELEASE_FENCE: the timeline value that completes after the set's GPU work (0: none).
        uint64_t gpu_value = 0;
        // When Aurora's submission reported it (steady clock ns; PF2_OVERLAP's CPU estimate).
        int64_t at_ns = 0;
    };

    const SubmissionResult* FindResultLocked(uint64_t token) const noexcept {
        for (const auto& result : submission_results_) {
            if (token != 0 && result.token == token) {
                return &result;
            }
        }
        return nullptr;
    }

    void ForgetResultLocked(uint64_t token) noexcept {
        for (auto& result : submission_results_) {
            if (token != 0 && result.token == token) {
                result = {};
            }
        }
    }

    // ---- PrimedFrame 2.0 headset A/B switches (vr/pf2_switches.h) -----------------------------

    // Per eye, the swapchain, region and size of a pair: with packed eyes pair[0] holds both, side
    // by side, eye N at x = N * eye width.
    EyeSwapchain& EyeChain(std::array<EyeSwapchain, kOpenXREyeCount>& pair, uint32_t eye) const noexcept {
        return packed_ ? pair[0] : pair[eye];
    }
    const EyeSwapchain& EyeChain(const std::array<EyeSwapchain, kOpenXREyeCount>& pair, uint32_t eye) const noexcept {
        return packed_ ? pair[0] : pair[eye];
    }
    uint32_t EyeWidth(const std::array<EyeSwapchain, kOpenXREyeCount>& pair, uint32_t eye) const noexcept {
        return packed_ ? pair[0].width / 2 : pair[eye].width;
    }
    uint32_t EyeHeight(const std::array<EyeSwapchain, kOpenXREyeCount>& pair, uint32_t eye) const noexcept {
        return packed_ ? pair[0].height : pair[eye].height;
    }
    uint32_t EyeX(const std::array<EyeSwapchain, kOpenXREyeCount>& pair, uint32_t eye) const noexcept {
        return packed_ ? eye * (pair[0].width / 2) : 0u;
    }

    // Packed eyes share one size, the larger of the two, at most half of kPackedMaxWidth wide
    // (scaled down keeping the aspect). Per-eye swapchains keep the runtime's sizes.
    std::array<OpenXREyeSize, kOpenXREyeCount> FitEyeSize(const std::array<OpenXREyeSize, kOpenXREyeCount>& size) {
        if (!packed_) {
            return size;
        }
        uint32_t width = std::max(size[0].width, size[1].width);
        uint32_t height = std::max(size[0].height, size[1].height);
        constexpr uint32_t kMaxEye = pf2::kPackedMaxWidth / 2;
        if (width > kMaxEye) {
            height = std::max(1u, static_cast<uint32_t>(static_cast<uint64_t>(height) * kMaxEye / width));
            width = kMaxEye;
            std::ostringstream message;
            message << "PF2_STEREO_PACKED: eyes " << size[0].width << 'x' << size[0].height << " scaled to " << width
                    << 'x' << height << " so the packed image stays " << pf2::kPackedMaxWidth << " wide";
            Log(OpenXRLogLevel::Info, message.str());
        }
        return {OpenXREyeSize{width, height}, OpenXREyeSize{width, height}};
    }

    // At bind time: the switches line, and the release fence's GPU completion tracking.
    void StartPf2(const AuroraDawnVulkanHandles& handles) {
        const auto& switches = pf2::Get();
        std::ostringstream message;
        message << switches.Describe();
        if (switches.fence) {
            const GpuFence::Mode mode =
                fence_.Start(get_instance_proc_, static_cast<VkInstance>(handles.instance),
                             static_cast<VkDevice>(handles.device), handles.queueFamily, handles.queueIndex,
                             timeline_semaphores_);
            message << "; release fence "
                    << (mode == GpuFence::Mode::Timeline ? "on a timeline semaphore"
                        : mode == GpuFence::Mode::Drain  ? "by draining the queue (no timeline semaphores)"
                                                         : "UNAVAILABLE (no Vulkan queue functions)");
            if (fence_.DevLoadMb() > 0) {
                message << "; PF2_DEV_GPU_LOAD_MB=" << fence_.DevLoadMb() << ": each set also fills that many MB (dev load)";
            }
        }
        if (packed_) {
            message << "; packed " << eye_swapchains_[0].width << 'x' << eye_swapchains_[0].height
                    << " per set (each eye drawn into its half; with multiview each layer of Aurora's eye target "
                       "is blitted into its half, so multiview stays as set)";
        }
        Log(OpenXRLogLevel::Info, message.str());
#ifdef PF2_STEAMVR_READER
        // Valve's FDM injection layer (an implicit OpenXR API layer that adds a Vulkan layer) blurs
        // distant HUDs: platform main turns it off unless PRIMEDFRAME_FDM=1 (src/MetroidPrime/main.cpp).
        {
            const char* disable = std::getenv("DISABLE_VULKAN_FDM_INJECTION_LAYER");
            Log(OpenXRLogLevel::Info,
                std::string("Vulkan FDM injection layer (VK_LAYER_VALVE_fdm_injection): ") +
                    (steamvr_timing::LibraryLoaded("VALVE_fdm_injection") ? "LOADED" : "not loaded") +
                    " (DISABLE_VULKAN_FDM_INJECTION_LAYER=" + (disable != nullptr ? disable : "unset") +
                    (std::getenv("PRIMEDFRAME_FDM") != nullptr ? ", PRIMEDFRAME_FDM=" + std::string(std::getenv("PRIMEDFRAME_FDM")) : std::string()) +
                    ")");
        }
#endif
        end_phase_.Configure(switches, runtime_ != nullptr ? runtime_->RuntimeInfo().runtime_name : std::string(),
                             [this](const std::string& line) { Log(OpenXRLogLevel::Info, line); });
    }

    // PF2_RELEASE_FENCE: waits (up to 100 ms) for the GPU work of the set that signalled `value`.
    // With `at` before that, `at_fn` runs at `at` if the GPU is still busy then (PF2_OVERLAP); its
    // time does not count as fence wait.
    void WaitGpu(uint64_t value, Clock::time_point at = Clock::time_point::max(),
                 const std::function<void()>& at_fn = {}) {
        if (fence_.CurrentMode() == GpuFence::Mode::Off) {
            return;
        }
        const auto start = Clock::now();
        bool done = false;
        Clock::duration hook_time{};
        if (at_fn && at < start + std::chrono::milliseconds(100)) {
            done = diagnostics::Measure(diagnostics::Stage::FenceWait, [&] { return fence_.WaitUntil(value, at); });
            if (!done) {
                const auto hook_start = Clock::now();
                at_fn();
                hook_time = Clock::now() - hook_start;
            }
        }
        if (!done) {
            done = diagnostics::Measure(diagnostics::Stage::FenceWait,
                                        [&] { return fence_.Wait(value, std::chrono::milliseconds(100)); });
        }
        if (diagnostics::Enabled()) {
            pf2_fence_ms_.push_back(static_cast<float>(
                std::chrono::duration<double, std::milli>(Clock::now() - start - hook_time).count()));
        }
        if (!done) {
            ++pf2_fence_timeouts_;
            if (pf2_fence_timeouts_ == 1 || pf2_fence_timeouts_ % 100 == 0) {
                std::ostringstream message;
                message << "PF2_RELEASE_FENCE: an eye set's GPU work did not finish within 100 ms ("
                        << pf2_fence_timeouts_ << " so far); released anyway";
                Log(OpenXRLogLevel::Warning, message.str());
            }
        }
    }

    void WaitGpuForToken(uint64_t token) {
        uint64_t value = 0;
        bool done = false;
        {
            std::lock_guard lock(submission_mutex_);
            if (const SubmissionResult* result = FindResultLocked(token)) {
                done = result->success;
                value = result->gpu_value;
            }
        }
        if (done) {
            WaitGpu(value);
        }
    }

    // Where a fresh packet's poses were located against the compositor frame that shows it, in
    // display periods (cause 3 of the H1 doubling: located 2 periods ahead, shown at the next).
    void NotePoseLead(XrTime pose_time, const OpenXRFrame& shown) {
        if (!diagnostics::Enabled() || shown.predicted_display_period <= 0) {
            return;
        }
        const XrDuration period = DisplayPeriod() > 0 ? DisplayPeriod() : shown.predicted_display_period;
        const double periods =
            static_cast<double>(pose_time - shown.predicted_display_time) / static_cast<double>(period);
        const long rounded = std::lround(periods);
        const size_t bucket = rounded <= -1 ? 0 : rounded == 0 ? 1 : rounded == 1 ? 2 : rounded == 2 ? 3 : 4;
        ++pf2_lead_[bucket];
    }

    // PF2_AHEAD_PERIODS: a fresh layer ended at its poses' time instead of its frame's.
    void NoteEndOverride(XrDuration delta) {
        if (delta == 0) {
            ++pf2_end_exact_;
        } else {
            ++pf2_end_moved_;
        }
        if (pf2_last_end_time_ != 0 && retained_pose_time_ <= pf2_last_end_time_) {
            ++pf2_end_backwards_;
        }
    }

    // ---- PF2_OVERLAP ---------------------------------------------------------------------------

    void NoteLocated(uint64_t serial) {
        located_ns_[serial % located_ns_.size()] = {serial, Clock::now().time_since_epoch().count()};
    }

    // Per finished packet: its CPU time (located to Aurora's submission: the game's step, draw,
    // encode and submit) and the GPU time of the sets finished since (each set's own, without the
    // time it queued behind the previous set's).
    void NoteOverlapSamples(uint64_t serial, int64_t submitted_ns) {
        const auto& located = located_ns_[serial % located_ns_.size()];
        if (serial != 0 && located.first == serial && submitted_ns > located.second) {
            PushSample(cpu_ms_, static_cast<float>(static_cast<double>(submitted_ns - located.second) / 1e6));
        }
        std::vector<float> gpu;
        fence_.TakeExclusiveMs(gpu);
        for (const float ms : gpu) {
            PushSample(gpu_own_ms_, ms);
            if (diagnostics::Enabled() && pf2_gpu_own_ms_.size() < 4096) pf2_gpu_own_ms_.push_back(ms);
        }
    }

    static void PushSample(std::vector<float>& ring, float value) {
        constexpr size_t kWindow = 45;  // half a second at 90 Hz
        if (ring.size() >= kWindow) ring.erase(ring.begin());
        ring.push_back(value);
    }

    // The 90th percentile of a window.
    static float P90(std::vector<float> values) {
        const size_t i = values.size() * 9 / 10;
        std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(i), values.end());
        return values[i];
    }

    // When the next packet must be located for its eyes to be finished by its own end, one true
    // period after this frame's (`target`): that end less the recent CPU and GPU times (90th
    // percentiles of the last half second) and PF2_OVERLAP's margin. Later than this frame's end
    // (the usual case with time to spare) changes nothing: the next cycle locates after the end,
    // as without PF2_OVERLAP.
    Clock::time_point OverlapLocateAt(Clock::time_point target) const {
        const long margin_us = pf2::Get().overlap_margin_us;
        if (margin_us <= 0 || cpu_ms_.size() < 16 || gpu_own_ms_.size() < 16) {
            return Clock::time_point::max();
        }
        const double need_ms = static_cast<double>(P90(cpu_ms_)) + static_cast<double>(P90(gpu_own_ms_)) +
                               static_cast<double>(margin_us) / 1000.0;
        const auto period = std::chrono::nanoseconds(DisplayPeriod());
        return target + period - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(need_ms));
    }

    // The [pf2] line, once a second with MP_VR_LOG=1 (on the pacing thread, after xrEndFrame).
    void Pf2Tick() {
        if (!diagnostics::Enabled()) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (pf2_window_start_.time_since_epoch().count() == 0) {
            pf2_window_start_ = now;
            return;
        }
        if (now - pf2_window_start_ < std::chrono::seconds(1)) {
            return;
        }
        pf2_window_start_ = now;
        std::vector<float> gpu;
        fence_.TakeGpuMs(gpu);
        std::ostringstream line;
        line << "[pf2] packed=" << (packed_ ? 1 : 0) << " fence=" << (fence_.CurrentMode() != GpuFence::Mode::Off ? 1 : 0)
             << " ahead=" << pf2::Get().ahead_periods << " | fence-wait ms p50/p95/max " << SampleStat(pf2_fence_ms_)
             << " timeouts " << pf2_fence_timeouts_ << " | gpu/set ms " << SampleStat(gpu) << " (" << gpu.size()
             << " sets), own " << SampleStat(pf2_gpu_own_ms_) << " | early locate " << pf2_overlaps_
             << " (before the end ms p50/p95/max " << SampleStat(pf2_overlap_lead_ms_) << "; cpu p90 "
             << (cpu_ms_.empty() ? 0.0f : P90(cpu_ms_)) << ") | pose lead vs shown frame (periods) <=-1:" << pf2_lead_[0] << " 0:" << pf2_lead_[1]
             << " +1:" << pf2_lead_[2] << " +2:" << pf2_lead_[3] << " more:" << pf2_lead_[4];
        // Simulation steps against fresh layers: fewer steps than layers means layers between steps
        // show interpolated (or repeated) game states.
        const uint32_t steps = pf2::g_sim_steps.exchange(0, std::memory_order_relaxed);
        const uint32_t ticks = pf2::g_sim_ticks.exchange(0, std::memory_order_relaxed);
        // True repeats: headset draws with no step since the previous draw and the same alpha (the
        // same game image again); alpha by display time or wall clock (PF2_ALPHA_DISPLAY).
        const uint32_t draws = pf2::g_draws.exchange(0, std::memory_order_relaxed);
        const uint32_t repeats = pf2::g_true_repeats.exchange(0, std::memory_order_relaxed);
        const uint32_t by_display = pf2::g_alpha_display.exchange(0, std::memory_order_relaxed);
        const uint32_t by_wall = pf2::g_alpha_wall.exchange(0, std::memory_order_relaxed);
        line << " | sim steps " << steps << " (" << ticks << " ticks of 1/600 s) for " << pf2_fresh_layers_
             << " fresh layers, true repeats " << repeats << " of " << draws << " draws (steps per draw 0:"
             << pf2::g_draw_steps[0].exchange(0, std::memory_order_relaxed)
             << " 1:" << pf2::g_draw_steps[1].exchange(0, std::memory_order_relaxed)
             << " 2+:" << pf2::g_draw_steps[2].exchange(0, std::memory_order_relaxed)
             << "; sim lock " << pf2::g_locked_draws.exchange(0, std::memory_order_relaxed) << "; alpha by display time "
             << by_display << ", wall clock " << by_wall << ") | locate after wait ms p50/p95/max "
             << SampleStat(pf2_locate_ms_);
        pf2_fresh_layers_ = 0;
        pf2_locate_ms_.clear();
        if (pf2::Get().ahead_periods >= 0) {
            line << " | ended at pose time: same as frame " << pf2_end_exact_ << ", moved " << pf2_end_moved_
                 << ", not after the previous end " << pf2_end_backwards_ << ", kept the frame's time "
                 << pf2_end_kept_;
        }
        diagnostics::detail::Info(line.str());
        pf2_fence_ms_.clear();
        pf2_gpu_own_ms_.clear();
        pf2_overlap_lead_ms_.clear();
        pf2_overlaps_ = 0;
        pf2_lead_ = {};
        pf2_end_exact_ = pf2_end_moved_ = pf2_end_backwards_ = pf2_end_kept_ = 0;
    }

    bool Fail(std::string message) {
        last_error_ = std::move(message);
        Log(OpenXRLogLevel::Error, last_error_);
        return false;
    }

    void ClearError() { last_error_.clear(); }

    void Log(OpenXRLogLevel level, std::string_view message) const noexcept {
        if (!logger_) {
            return;
        }
        try {
            logger_(level, message);
        } catch (...) {
        }
    }

    PFN_vkGetInstanceProcAddr get_instance_proc_ = nullptr;
    // PF2 switches (vr/pf2_switches.h): packed eyes for this session, the release fence, and the
    // [pf2] line's window (pacing thread).
    bool packed_ = false;
    GpuFence fence_;
    EndPhaseLock end_phase_;
    // The display time the active frame's poses were located for, and the retained layer's.
    XrTime active_pose_time_ = 0;
    XrTime retained_pose_time_ = 0;
    XrTime pf2_last_end_time_ = 0;
    std::chrono::steady_clock::time_point pf2_window_start_{};
    std::vector<float> pf2_fence_ms_;
    std::vector<float> pf2_locate_ms_;
    // PF2_OVERLAP: recent CPU and own GPU times per packet, locate times by serial, [pf2] figures.
    std::vector<float> cpu_ms_;
    std::vector<float> gpu_own_ms_;
    std::array<std::pair<uint64_t, int64_t>, 8> located_ns_{};
    std::vector<float> pf2_gpu_own_ms_;
    std::vector<float> pf2_overlap_lead_ms_;
    uint32_t pf2_overlaps_ = 0;
    // Fresh layers that kept xrWaitFrame's display time (EndRetainedFrame), and xrEndFrame calls
    // refused with XR_ERROR_TIME_INVALID (all, and in a row).
    uint32_t pf2_end_kept_ = 0;
    uint64_t time_invalid_total_ = 0;
    uint32_t time_invalid_run_ = 0;
    std::chrono::steady_clock::time_point pf2_wait_return_{};
    uint32_t pf2_fresh_layers_ = 0;
    uint64_t pf2_fence_timeouts_ = 0;
    std::array<uint32_t, 5> pf2_lead_{};
    uint32_t pf2_end_exact_ = 0;
    uint32_t pf2_end_moved_ = 0;
    uint32_t pf2_end_backwards_ = 0;
    PFN_xrCreateVulkanInstanceKHR create_instance_ = nullptr;
    PFN_xrCreateVulkanDeviceKHR create_device_ = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR get_device_ = nullptr;
    VkInstance vk_instance_ = VK_NULL_HANDLE;
    uint32_t instance_api_version_ = 0;
    bool timeline_semaphores_ = false;
    bool hooks_installed_ = false;
    OpenXRRuntime* runtime_ = nullptr;
    OpenXRLogCallback logger_;
    // The room around the virtual screen, started and paused as each presentation arrives.
    OpenXRPassthrough passthrough_{logger_};
    OpenXRVulkanGraphicsRequirements requirements_{};
    std::array<EyeSwapchain, kOpenXREyeCount> eye_swapchains_{};
    std::array<EyeSwapchain, kOpenXREyeCount> retained_swapchains_{};
    // As the D3D12 backend's.
    std::array<OpenXREyeSize, kOpenXREyeCount> eye_size_{};
    std::array<OpenXREyeSize, kOpenXREyeCount> requested_eye_size_{};
    struct RetiredPair {
        std::array<EyeSwapchain, kOpenXREyeCount> swapchains;
        uint32_t cycles_left;
    };
    std::vector<RetiredPair> retired_pairs_;
    OpenXREyeResizeGate eye_resize_gate_;
    // The settings panel's layer: written like the eyes into panel_swapchain_, shown from
    // retained_panel_swapchain_ (see FinishFrame).
    EyeSwapchain panel_swapchain_{};
    EyeSwapchain retained_panel_swapchain_{};
    bool panel_swapchains_ready_ = false;
    bool panel_layer_failed_ = false;
    // The retained frame rendered the panel's image into retained_panel_swapchain_.
    bool retained_panel_valid_ = false;
    OpenXRBackendFrame retained_frame_{};
    uint64_t retained_session_serial_ = 0;
    uint64_t retained_space_serial_ = 0;
    bool have_retained_frame_ = false;
    VkFormat aurora_format_ = VK_FORMAT_UNDEFINED;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    std::string last_error_;

    std::mutex submission_mutex_;
    std::condition_variable submission_cv_;
    uint64_t awaiting_token_ = 0;
    std::array<SubmissionResult, 2> submission_results_{};
    bool shutting_down_ = false;

    // The packet whose eyes Aurora renders or rendered next, and the one armed behind it on the
    // other swapchain pair (ArmPacket), promoted when the first's compositor frame ends; the
    // packet of the active compositor frame. Each with the origin of its poses, and the last
    // located packet's.
    uint64_t pending_packet_serial_ = 0;
    uint64_t queued_packet_serial_ = 0;
    uint64_t active_packet_serial_ = 0;
    PoseOrigin located_origin_{};
    PoseOrigin pending_origin_{};
    PoseOrigin queued_origin_{};
    PoseOrigin active_origin_{};
    // Packet tokens never collide with the runtime's frame serials.
    uint64_t next_packet_serial_ = 1ull << 40;
    uint64_t timing_session_serial_ = 0;
    XrTime last_display_time_ = 0;
    XrDuration last_display_period_ = 0;
    // The session's refresh period (NoteDisplayTiming), and the display's true one (DisplayPeriod).
    XrDuration nominal_period_ = 0;
    bool last_should_render_ = false;
    uint64_t active_frame_serial_ = 0;
    OpenXRFrame active_frame_{};
    bool requirements_queried_ = false;
    bool owns_session_ = false;
    bool bridge_enabled_ = false;
    bool bound_ = false;
    bool frame_active_ = false;
    bool shutdown_unsafe_ = false;
};

OpenXRVulkanDirectBackend::OpenXRVulkanDirectBackend(OpenXRLogCallback logger)
: m_impl(std::make_unique<Impl>(std::move(logger))) {}
OpenXRVulkanDirectBackend::~OpenXRVulkanDirectBackend() = default;
bool OpenXRVulkanDirectBackend::QueryGraphicsRequirements(OpenXRRuntime& runtime) {
    return m_impl->QueryGraphicsRequirements(runtime);
}
bool OpenXRVulkanDirectBackend::BindAurora(OpenXRRuntime& runtime) { return m_impl->BindAurora(runtime); }
void OpenXRVulkanDirectBackend::SetRenderScale(float scale) { m_impl->SetRenderScale(scale); }
OpenXRBeginStatus OpenXRVulkanDirectBackend::BeginFrame(const OpenXRPresentation& presentation,
                                                        OpenXRBackendFrame& frame) {
    return m_impl->BeginFrame(presentation, frame);
}
OpenXRSubmissionStatus OpenXRVulkanDirectBackend::WaitForSubmission(const OpenXRBackendFrame& frame,
                                                                    uint32_t timeout_ms) {
    return m_impl->WaitForSubmission(frame, timeout_ms);
}
bool OpenXRVulkanDirectBackend::TryCancelPendingFrame(OpenXRBackendFrame& frame) {
    return m_impl->TryCancelPendingFrame(frame);
}
bool OpenXRVulkanDirectBackend::RepeatFrame(const OpenXRBackendFrame& frame) { return m_impl->RepeatFrame(frame); }
bool OpenXRVulkanDirectBackend::FinishFrame(OpenXRBackendFrame& frame, bool submit_layer) {
    return m_impl->FinishFrame(frame, submit_layer);
}
bool OpenXRVulkanDirectBackend::FinishFrame(OpenXRBackendFrame& frame, bool submit_layer,
                                            const std::function<void()>& overlap) {
    return m_impl->FinishFrame(frame, submit_layer, overlap);
}
OpenXRBeginStatus OpenXRVulkanDirectBackend::PrepareOverlapPacket(const OpenXRPresentation& presentation,
                                                                  OpenXRBackendFrame& packet) {
    return m_impl->PrepareOverlapPacket(presentation, packet);
}
OpenXRBeginStatus OpenXRVulkanDirectBackend::PreparePacket(const OpenXRPresentation& presentation,
                                                           OpenXRBackendFrame& packet) {
    return m_impl->PreparePacket(presentation, packet);
}
bool OpenXRVulkanDirectBackend::TryCancelPendingPacket(OpenXRBackendFrame& packet) {
    return m_impl->TryCancelPendingPacket(packet);
}
OpenXRBeginStatus OpenXRVulkanDirectBackend::BeginFrameForPacket(const OpenXRBackendFrame& packet,
                                                                 OpenXRBackendFrame& frame) {
    return m_impl->BeginFrameForPacket(packet, frame);
}
OpenXRSubmissionStatus OpenXRVulkanDirectBackend::CopyRenderedEyes(const OpenXRBackendFrame& frame) {
    return m_impl->CopyRenderedEyes(frame);
}
OpenXRBeginStatus OpenXRVulkanDirectBackend::KeepAliveCycle() { return m_impl->KeepAliveCycle(); }
OpenXRBeginStatus OpenXRVulkanDirectBackend::LocatePacket(const OpenXRPresentation& presentation,
                                                          OpenXRBackendFrame& packet, uint32_t periods_ahead) {
    return m_impl->LocatePacket(presentation, packet, periods_ahead);
}
OpenXRBeginStatus OpenXRVulkanDirectBackend::ArmPacket(OpenXRBackendFrame& packet) { return m_impl->ArmPacket(packet); }
bool OpenXRVulkanDirectBackend::SupportsPipelining() const { return m_impl->SupportsPipelining(); }
bool OpenXRVulkanDirectBackend::EyesResizing() const { return m_impl->EyesResizing(); }
bool OpenXRVulkanDirectBackend::Shutdown() { return m_impl->Shutdown(); }
bool OpenXRVulkanDirectBackend::RecreateSession(OpenXRRuntime& runtime) { return m_impl->RecreateSession(runtime); }
bool OpenXRVulkanDirectBackend::IsBound() const { return m_impl->IsBound(); }
bool OpenXRVulkanDirectBackend::PanelLayerAvailable() const { return m_impl->PanelLayerAvailable(); }
const OpenXRVulkanGraphicsRequirements& OpenXRVulkanDirectBackend::GraphicsRequirements() const {
    return m_impl->GraphicsRequirements();
}
int64_t OpenXRVulkanDirectBackend::SwapchainFormat() const { return m_impl->SwapchainFormat(); }
const std::string& OpenXRVulkanDirectBackend::LastError() const { return m_impl->LastError(); }

#if defined(__ANDROID__)
// ---- OpenXRQuestVulkanBackend --------------------------------------------------------------------

OpenXRQuestVulkanBackend::OpenXRQuestVulkanBackend(OpenXRLogCallback logger) : logger_(std::move(logger)) {}
OpenXRQuestVulkanBackend::~OpenXRQuestVulkanBackend() = default;

bool OpenXRQuestVulkanBackend::QueryGraphicsRequirements(OpenXRRuntime& runtime) {
    if (direct_ != nullptr) {
        return direct_->QueryGraphicsRequirements(runtime);
    }
    if (shared_ != nullptr) {
        return shared_->QueryGraphicsRequirements(runtime);
    }
    if (GetVrSettings().direct_present) {
        auto direct = std::make_unique<OpenXRVulkanDirectBackend>(logger_);
        if (direct->QueryGraphicsRequirements(runtime)) {
            direct_ = std::move(direct);
            return true;
        }
        if (logger_) {
            logger_(OpenXRLogLevel::Warning,
                    "direct presentation unavailable (" + direct->LastError() + "); sharing the eyes through AHardwareBuffers");
        }
    }
    shared_ = std::make_unique<OpenXRVulkanBackend>(logger_);
    return shared_->QueryGraphicsRequirements(runtime);
}

#define PG_QUEST_BACKEND_CALL(call, otherwise) \
    (direct_ != nullptr ? direct_->call : shared_ != nullptr ? shared_->call : (otherwise))

bool OpenXRQuestVulkanBackend::BindAurora(OpenXRRuntime& runtime) {
    return PG_QUEST_BACKEND_CALL(BindAurora(runtime), false);
}
void OpenXRQuestVulkanBackend::SetRenderScale(float scale) {
    if (direct_ != nullptr) {
        direct_->SetRenderScale(scale);
    } else if (shared_ != nullptr) {
        shared_->SetRenderScale(scale);
    }
}
OpenXRBeginStatus OpenXRQuestVulkanBackend::BeginFrame(const OpenXRPresentation& presentation,
                                                       OpenXRBackendFrame& frame) {
    return PG_QUEST_BACKEND_CALL(BeginFrame(presentation, frame), OpenXRBeginStatus::Error);
}
OpenXRSubmissionStatus OpenXRQuestVulkanBackend::WaitForSubmission(const OpenXRBackendFrame& frame,
                                                                   uint32_t timeout_ms) {
    return PG_QUEST_BACKEND_CALL(WaitForSubmission(frame, timeout_ms), OpenXRSubmissionStatus::Failed);
}
bool OpenXRQuestVulkanBackend::TryCancelPendingFrame(OpenXRBackendFrame& frame) {
    return PG_QUEST_BACKEND_CALL(TryCancelPendingFrame(frame), false);
}
bool OpenXRQuestVulkanBackend::RepeatFrame(const OpenXRBackendFrame& frame) {
    return PG_QUEST_BACKEND_CALL(RepeatFrame(frame), false);
}
bool OpenXRQuestVulkanBackend::FinishFrame(OpenXRBackendFrame& frame, bool submit_layer) {
    return PG_QUEST_BACKEND_CALL(FinishFrame(frame, submit_layer), false);
}
OpenXRBeginStatus OpenXRQuestVulkanBackend::PreparePacket(const OpenXRPresentation& presentation,
                                                          OpenXRBackendFrame& packet) {
    return PG_QUEST_BACKEND_CALL(PreparePacket(presentation, packet), OpenXRBeginStatus::Error);
}
bool OpenXRQuestVulkanBackend::TryCancelPendingPacket(OpenXRBackendFrame& packet) {
    return PG_QUEST_BACKEND_CALL(TryCancelPendingPacket(packet), false);
}
OpenXRBeginStatus OpenXRQuestVulkanBackend::BeginFrameForPacket(const OpenXRBackendFrame& packet,
                                                                OpenXRBackendFrame& frame) {
    return PG_QUEST_BACKEND_CALL(BeginFrameForPacket(packet, frame), OpenXRBeginStatus::Error);
}
OpenXRSubmissionStatus OpenXRQuestVulkanBackend::CopyRenderedEyes(const OpenXRBackendFrame& frame) {
    return PG_QUEST_BACKEND_CALL(CopyRenderedEyes(frame), OpenXRSubmissionStatus::Failed);
}
OpenXRBeginStatus OpenXRQuestVulkanBackend::KeepAliveCycle() {
    return PG_QUEST_BACKEND_CALL(KeepAliveCycle(), OpenXRBeginStatus::Error);
}
OpenXRBeginStatus OpenXRQuestVulkanBackend::LocatePacket(const OpenXRPresentation& presentation,
                                                         OpenXRBackendFrame& packet, uint32_t periods_ahead) {
    return PG_QUEST_BACKEND_CALL(LocatePacket(presentation, packet, periods_ahead), OpenXRBeginStatus::Error);
}
OpenXRBeginStatus OpenXRQuestVulkanBackend::ArmPacket(OpenXRBackendFrame& packet) {
    return PG_QUEST_BACKEND_CALL(ArmPacket(packet), OpenXRBeginStatus::Error);
}
bool OpenXRQuestVulkanBackend::SupportsPipelining() const { return PG_QUEST_BACKEND_CALL(SupportsPipelining(), false); }
bool OpenXRQuestVulkanBackend::EyesResizing() const { return PG_QUEST_BACKEND_CALL(EyesResizing(), false); }
bool OpenXRQuestVulkanBackend::Shutdown() { return PG_QUEST_BACKEND_CALL(Shutdown(), true); }
bool OpenXRQuestVulkanBackend::IsBound() const { return PG_QUEST_BACKEND_CALL(IsBound(), false); }
bool OpenXRQuestVulkanBackend::PanelLayerAvailable() const {
    return PG_QUEST_BACKEND_CALL(PanelLayerAvailable(), false);
}
const OpenXRVulkanGraphicsRequirements& OpenXRQuestVulkanBackend::GraphicsRequirements() const {
    return PG_QUEST_BACKEND_CALL(GraphicsRequirements(), kNoRequirements);
}
int64_t OpenXRQuestVulkanBackend::SwapchainFormat() const {
    return PG_QUEST_BACKEND_CALL(SwapchainFormat(), int64_t{0});
}
const std::string& OpenXRQuestVulkanBackend::LastError() const {
    return PG_QUEST_BACKEND_CALL(LastError(), kNoBackend);
}

#undef PG_QUEST_BACKEND_CALL
#endif // defined(__ANDROID__)

} // namespace PortVr

#endif // defined(MP_ENABLE_OPENXR) && (defined(__ANDROID__) || defined(_WIN32))
