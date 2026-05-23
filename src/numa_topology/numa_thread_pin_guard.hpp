#pragma once

#include <sched.h>

class numa_thread_pin_guard {
public:
    explicit numa_thread_pin_guard(int node_id) noexcept;
    ~numa_thread_pin_guard() noexcept;

    numa_thread_pin_guard(const numa_thread_pin_guard&) = delete;
    numa_thread_pin_guard& operator=(const numa_thread_pin_guard&) = delete;
    numa_thread_pin_guard(numa_thread_pin_guard&&) = delete;
    numa_thread_pin_guard& operator=(numa_thread_pin_guard&&) = delete;

    bool pinned() const noexcept {
        return active_;
    }

    int node_id() const noexcept {
        return node_id_;
    }

    bool restore() noexcept;
    void release() noexcept {
        active_ = false;
    }

private:
    int node_id_;
    cpu_set_t previous_affinity_{};
    bool active_ = false;
};
