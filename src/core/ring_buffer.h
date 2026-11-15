// Lock-free single-producer / single-consumer ring buffer.
//
// Exactly one thread may call write() and exactly one thread may call
// read()/skip(). Capacity is rounded up to a power of two. The consumer can
// block on waitReadable(); the producer never blocks (it drops and the caller
// counts the overrun), which is the behaviour a real-time hardware callback
// needs.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

namespace osx {

template <typename T>
class RingBuffer {
    static_assert(std::is_trivially_copyable_v<T>, "RingBuffer needs trivially copyable T");

public:
    explicit RingBuffer(size_t minCapacity) {
        size_t cap = 2;
        while (cap < minCapacity) cap <<= 1;
        cap_ = cap;
        mask_ = cap - 1;
        buf_ = std::make_unique<T[]>(cap);
    }

    size_t capacity() const { return cap_; }

    size_t readable() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }
    size_t writable() const { return cap_ - readable(); }

    // Producer side. Writes up to n elements, returns the number written.
    size_t write(const T* src, size_t n) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        n = std::min(n, cap_ - (head - tail));
        if (n == 0) return 0;
        const size_t pos = head & mask_;
        const size_t first = std::min(n, cap_ - pos);
        std::memcpy(&buf_[pos], src, first * sizeof(T));
        std::memcpy(&buf_[0], src + first, (n - first) * sizeof(T));
        head_.store(head + n, std::memory_order_release);
        signal_.fetch_add(1, std::memory_order_release);
        signal_.notify_one();
        return n;
    }

    // Consumer side. Reads up to n elements, returns the number read.
    size_t read(T* dst, size_t n) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        n = std::min(n, head - tail);
        if (n == 0) return 0;
        const size_t pos = tail & mask_;
        const size_t first = std::min(n, cap_ - pos);
        std::memcpy(dst, &buf_[pos], first * sizeof(T));
        std::memcpy(dst + first, &buf_[0], (n - first) * sizeof(T));
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

    // Consumer side. Blocks until at least `n` elements are readable or
    // wake() is called. Returns the readable count.
    size_t waitReadable(size_t n = 1) {
        for (;;) {
            const uint32_t s = signal_.load(std::memory_order_acquire);
            const size_t r = readable();
            if (r >= n || woken_.exchange(false, std::memory_order_acq_rel)) return r;
            signal_.wait(s, std::memory_order_acquire);
        }
    }

    // Any thread: unblock a consumer sitting in waitReadable().
    void wake() {
        woken_.store(true, std::memory_order_release);
        signal_.fetch_add(1, std::memory_order_release);
        signal_.notify_all();
    }

    // Only safe while neither producer nor consumer is running.
    void reset() {
        head_.store(0);
        tail_.store(0);
        woken_.store(false);
    }

private:
    size_t cap_ = 0, mask_ = 0;
    std::unique_ptr<T[]> buf_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) std::atomic<uint32_t> signal_{0};
    std::atomic<bool> woken_{false};
};

}  // namespace osx
