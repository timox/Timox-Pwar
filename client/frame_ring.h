#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace pwar {
// One producer and one consumer; cursors are never reset while running.
class FrameRing {
public:
    static constexpr std::uint32_t capacity = 32768;
    std::uint32_t write(const float* stereo, std::uint32_t frames, std::uint64_t enqueueMs=0) noexcept {
        const auto w=write_.load(std::memory_order_relaxed);
        const auto r=read_.load(std::memory_order_acquire);
        if (!stereo || frames>capacity || frames>capacity-(w-r)) return 0;
        for(std::uint32_t f=0;f<frames;++f) {
            const auto slot=((w+f)&(capacity-1))*2;
            samples_[slot]=stereo[f*2]; samples_[slot+1]=stereo[f*2+1];
            enqueuedMs_[slot/2]=enqueueMs;
        }
        write_.store(w+frames,std::memory_order_release);
        return frames;
    }
    std::uint32_t read(float* stereo, std::uint32_t frames) noexcept {
        const auto r=read_.load(std::memory_order_relaxed);
        const auto w=write_.load(std::memory_order_acquire);
        if (!stereo || frames>capacity) return 0;
        if (frames>w-r) frames=w-r;
        for(std::uint32_t f=0;f<frames;++f) {
            const auto slot=((r+f)&(capacity-1))*2;
            stereo[f*2]=samples_[slot]; stereo[f*2+1]=samples_[slot+1];
        }
        read_.store(r+frames,std::memory_order_release);
        return frames;
    }
    std::uint32_t available() const noexcept {
        return write_.load(std::memory_order_acquire)-read_.load(std::memory_order_relaxed);
    }
    // Consumer only, using the producer's monotonic clock (not a peer clock).
    std::uint32_t discardExpired(std::uint64_t nowMs,std::uint64_t maxAgeMs) noexcept {
        const auto r=read_.load(std::memory_order_relaxed);
        const auto w=write_.load(std::memory_order_acquire);
        std::uint32_t dropped=0;
        while(dropped<w-r) {
            const auto enqueued=enqueuedMs_[(r+dropped)&(capacity-1)];
            if(nowMs<enqueued || nowMs-enqueued<=maxAgeMs) break;
            ++dropped;
        }
        read_.store(r+dropped,std::memory_order_release);
        return dropped;
    }
private:
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    std::array<float,capacity*2> samples_{};
    std::array<std::uint64_t,capacity> enqueuedMs_{};
    alignas(64) std::atomic<std::uint32_t> read_{0};
    alignas(64) std::atomic<std::uint32_t> write_{0};
};
}
