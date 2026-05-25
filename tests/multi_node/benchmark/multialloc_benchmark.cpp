#include "numa_memory_resource.hpp"
#include "numa_topology/numa_thread_pin_guard.hpp"
#include "numa_topology/numa_topology.hpp"
#include <atomic>
#include <barrier>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <signal.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct Options {
    std::size_t threads = 1;
    std::size_t allocations_per_thread = 10000;
    std::size_t waves = 1;
    bool pin_threads = false;
    bool progress = true;
    bool vm_trace = false;
    std::string backend = "all";
    std::string size_profile = "mixed";
    std::string custom_sizes;
    std::string json_path = "-";
};

struct BackendInstance {
    std::unique_ptr<std::pmr::memory_resource> owned_resource;
    std::pmr::memory_resource* resource = nullptr;
};

struct BackendSpec {
    std::string name;
    std::function<BackendInstance()> create;
};

struct AllocationRecord {
    void* ptr = nullptr;
    std::size_t size = 0;
};

struct WorkloadResult {
    std::uint64_t duration_ns = 0;
    std::uint64_t allocation_count = 0;
    std::uint64_t pmr_operation_count = 0;
    std::uint64_t logical_total_allocated_bytes = 0;
    std::string error;
};

struct VmTraceStats {
    bool enabled = false;
    bool complete = false;
    bool counting = false;
    std::string error;
    std::uint64_t trace_start_count = 0;
    std::uint64_t trace_stop_count = 0;
    std::uint64_t mmap_count = 0;
    std::uint64_t munmap_count = 0;
    std::uint64_t mremap_count = 0;
    std::uint64_t brk_count = 0;
    std::uint64_t total_mapped_bytes = 0;
    std::uint64_t total_unmapped_bytes = 0;
    std::uint64_t current_mapped_bytes = 0;
    std::uint64_t total_brk_growth_bytes = 0;
    std::uint64_t total_brk_shrink_bytes = 0;
    std::uint64_t current_brk_growth_bytes = 0;
    unsigned long last_brk = 0;
};

struct SyscallTraceState {
    bool in_syscall = false;
    long number = -1;
    unsigned long arg0 = 0;
    unsigned long arg1 = 0;
    unsigned long arg2 = 0;
};

class tracking_memory_resource : public std::pmr::memory_resource {
public:
    explicit tracking_memory_resource(std::pmr::memory_resource* upstream)
        : upstream_(upstream) {
        if (upstream_ == nullptr) {
            throw std::invalid_argument("tracking_memory_resource requires an upstream resource");
        }
    }

    std::size_t current_bytes() const noexcept {
        return current_bytes_.load(std::memory_order_relaxed);
    }

    std::size_t total_allocated_bytes() const noexcept {
        return total_allocated_bytes_.load(std::memory_order_relaxed);
    }

    std::size_t allocation_count() const noexcept {
        return allocation_count_.load(std::memory_order_relaxed);
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        void* ptr = upstream_->allocate(bytes, alignment);
        const std::size_t logical_bytes = bytes == 0 ? 1 : bytes;
        current_bytes_.fetch_add(logical_bytes, std::memory_order_relaxed);
        total_allocated_bytes_.fetch_add(logical_bytes, std::memory_order_relaxed);
        allocation_count_.fetch_add(1, std::memory_order_relaxed);
        return ptr;
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        if (p == nullptr) {
            return;
        }

        upstream_->deallocate(p, bytes, alignment);
        const std::size_t logical_bytes = bytes == 0 ? 1 : bytes;
        current_bytes_.fetch_sub(logical_bytes, std::memory_order_relaxed);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_;
    std::atomic<std::size_t> current_bytes_{0};
    std::atomic<std::size_t> total_allocated_bytes_{0};
    std::atomic<std::size_t> allocation_count_{0};
};

class non_owning_memory_resource final : public std::pmr::memory_resource {
public:
    explicit non_owning_memory_resource(std::pmr::memory_resource* upstream)
        : upstream_(upstream) {}

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        return upstream_->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        upstream_->deallocate(p, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return upstream_->is_equal(other);
    }

    std::pmr::memory_resource* upstream_;
};

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (char ch : value) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += ch;
                break;
        }
    }
    return out;
}

std::size_t parse_size(std::string_view text, std::string_view name) {
    try {
        std::size_t consumed = 0;
        const std::size_t value = std::stoull(std::string(text), &consumed, 10);
        if (consumed != text.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return value;
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid value for " + std::string(name) + ": " + std::string(text));
    }
}

bool parse_bool(std::string_view text, std::string_view name) {
    if (text == "true" || text == "1" || text == "yes") {
        return true;
    }
    if (text == "false" || text == "0" || text == "no") {
        return false;
    }

    throw std::invalid_argument("invalid boolean for " + std::string(name) + ": " + std::string(text));
}

std::vector<std::size_t> parse_size_list(std::string_view text) {
    std::vector<std::size_t> sizes;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find(',', begin);
        const std::string_view token = text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin);
        if (!token.empty()) {
            sizes.push_back(parse_size(token, "--sizes"));
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }

    if (sizes.empty()) {
        throw std::invalid_argument("--sizes must contain at least one size");
    }

    return sizes;
}

Options parse_options(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto require_value = [&](std::string_view name) -> std::string_view {
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value for " + std::string(name));
            }
            return argv[++i];
        };

        if (arg == "--threads") {
            options.threads = parse_size(require_value(arg), arg);
        } else if (arg == "--allocations-per-thread") {
            options.allocations_per_thread = parse_size(require_value(arg), arg);
        } else if (arg == "--waves") {
            options.waves = parse_size(require_value(arg), arg);
        } else if (arg == "--pin-threads") {
            options.pin_threads = parse_bool(require_value(arg), arg);
        } else if (arg == "--progress") {
            options.progress = parse_bool(require_value(arg), arg);
        } else if (arg == "--vm-trace") {
            options.vm_trace = parse_bool(require_value(arg), arg);
        } else if (arg == "--backend") {
            options.backend = std::string(require_value(arg));
        } else if (arg == "--size-profile") {
            options.size_profile = std::string(require_value(arg));
        } else if (arg == "--sizes") {
            options.custom_sizes = std::string(require_value(arg));
            options.size_profile = "custom";
        } else if (arg == "--json") {
            options.json_path = std::string(require_value(arg));
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: multi_node_bench_multialloc [options]\n"
                << "  --threads N\n"
                << "  --allocations-per-thread N\n"
                << "  --waves N                    (default: 1)\n"
                << "  --pin-threads true|false\n"
                << "  --progress true|false        (default: true, writes to stderr)\n"
                << "  --backend all|numa|new_delete|sync_pool_new_delete\n"
                << "  --size-profile mixed|small|large|custom\n"
                << "  --sizes 64,256,4096       (sets --size-profile custom)\n"
                << "  --vm-trace true|false     (slow syscall trace for mmap/munmap/mremap/brk)\n"
                << "  --json PATH|-             (default: -)\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown option: " + std::string(arg));
        }
    }

    if (options.threads == 0) {
        throw std::invalid_argument("--threads must be greater than zero");
    }
    if (options.allocations_per_thread == 0) {
        throw std::invalid_argument("--allocations-per-thread must be greater than zero");
    }
    if (options.waves == 0) {
        throw std::invalid_argument("--waves must be greater than zero");
    }

    return options;
}

std::vector<std::size_t> make_size_profile(const Options& options) {
    if (options.size_profile == "small") {
        return {1, 8, 16, 17, 64, 128, 256, 513, 1024, 2048, 4096};
    }
    if (options.size_profile == "large") {
        return {4097, 8192, 16 * 1024, 64 * 1024, 256 * 1024, 1024 * 1024};
    }
    if (options.size_profile == "mixed") {
        return {1, 16, 17, 64, 256, 513, 1024, 2048, 4096, 4097, 8192, 64 * 1024, 1024 * 1024};
    }
    if (options.size_profile == "custom") {
        return parse_size_list(options.custom_sizes);
    }

    throw std::invalid_argument("unknown --size-profile: " + options.size_profile);
}

std::vector<BackendSpec> make_backend_registry() {
    std::vector<BackendSpec> backends;

    backends.push_back(BackendSpec{
        "numa",
        [] {
            auto resource = std::make_unique<numa_memory_resource>(true);
            auto* raw = resource.get();
            return BackendInstance{std::move(resource), raw};
        }
    });

    backends.push_back(BackendSpec{
        "new_delete",
        [] {
            auto resource = std::make_unique<non_owning_memory_resource>(std::pmr::new_delete_resource());
            auto* raw = resource.get();
            return BackendInstance{std::move(resource), raw};
        }
    });

    backends.push_back(BackendSpec{
        "sync_pool_new_delete",
        [] {
            auto resource = std::make_unique<std::pmr::synchronized_pool_resource>(
                std::pmr::new_delete_resource()
            );
            auto* raw = resource.get();
            return BackendInstance{std::move(resource), raw};
        }
    });

    return backends;
}

void register_custom_backends(std::vector<BackendSpec>& backends) {
    (void)backends;
    // Add project-specific PMR factories here, for example:
    // backends.push_back({"my_allocator", [] { return BackendInstance{...}; }});
}

std::size_t size_for_allocation(
    const std::vector<std::size_t>& sizes,
    std::size_t thread_index,
    std::size_t wave_index,
    std::size_t allocation_index
) {
    const std::size_t mixed =
        allocation_index * 1315423911ULL +
        thread_index * 2654435761ULL +
        wave_index * 97531ULL;
    return sizes[mixed % sizes.size()];
}

WorkloadResult run_workload(
    const Options& options,
    tracking_memory_resource& resource,
    const std::vector<std::size_t>& sizes
) {
    WorkloadResult result;
    std::barrier ready_barrier(static_cast<std::ptrdiff_t>(options.threads + 1));
    std::barrier start_barrier(static_cast<std::ptrdiff_t>(options.threads + 1));
    std::barrier allocated_barrier(static_cast<std::ptrdiff_t>(options.threads));
    std::barrier freed_barrier(static_cast<std::ptrdiff_t>(options.threads));
    std::barrier done_barrier(static_cast<std::ptrdiff_t>(options.threads + 1));
    std::mutex error_mutex;
    std::vector<std::thread> workers;
    workers.reserve(options.threads);

    const int node_count = NumaTopologyManager::instance().node_count();

    for (std::size_t thread_index = 0; thread_index < options.threads; ++thread_index) {
        workers.emplace_back([&, thread_index] {
            std::vector<AllocationRecord> records;
            bool can_allocate = true;

            try {
                records.reserve(options.allocations_per_thread);
            } catch (const std::exception& ex) {
                can_allocate = false;
                std::lock_guard<std::mutex> lock(error_mutex);
                if (result.error.empty()) {
                    result.error = ex.what();
                }
            }

            std::unique_ptr<numa_thread_pin_guard> pin;
            if (options.pin_threads && node_count > 0) {
                pin = std::make_unique<numa_thread_pin_guard>(
                    static_cast<int>(thread_index % static_cast<std::size_t>(node_count))
                );
                if (!pin->pinned()) {
                    can_allocate = false;
                    std::lock_guard<std::mutex> lock(error_mutex);
                    if (result.error.empty()) {
                        result.error = "failed to pin worker thread";
                    }
                }
            }

            ready_barrier.arrive_and_wait();
            start_barrier.arrive_and_wait();

            for (std::size_t wave = 0; wave < options.waves; ++wave) {
                records.clear();

                if (can_allocate) {
                    try {
                        for (std::size_t i = 0; i < options.allocations_per_thread; ++i) {
                            const std::size_t size = size_for_allocation(sizes, thread_index, wave, i);
                            void* ptr = resource.allocate(size, alignof(std::max_align_t));
                            records.push_back(AllocationRecord{ptr, size});
                        }
                    } catch (const std::exception& ex) {
                        can_allocate = false;
                        std::lock_guard<std::mutex> lock(error_mutex);
                        if (result.error.empty()) {
                            result.error = ex.what();
                        }
                    }
                }

                allocated_barrier.arrive_and_wait();

                for (std::size_t i = records.size(); i > 0; --i) {
                    const AllocationRecord& record = records[i - 1];
                    resource.deallocate(record.ptr, record.size, alignof(std::max_align_t));
                }
                records.clear();

                freed_barrier.arrive_and_wait();

                if (thread_index == 0 && options.progress) {
                    std::cerr << "[multialloc] wave " << (wave + 1) << '/' << options.waves
                              << " complete\n";
                }
            }

            done_barrier.arrive_and_wait();
        });
    }

    ready_barrier.arrive_and_wait();
    if (options.vm_trace) {
        raise(SIGUSR1);
    }

    const auto start_time = std::chrono::steady_clock::now();
    start_barrier.arrive_and_wait();

    done_barrier.arrive_and_wait();
    if (options.vm_trace) {
        raise(SIGUSR2);
    }

    for (std::thread& worker : workers) {
        worker.join();
    }
    const auto end_time = std::chrono::steady_clock::now();

    const auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        end_time - start_time
    ).count();

    result.duration_ns = static_cast<std::uint64_t>(duration_ns);
    result.allocation_count = static_cast<std::uint64_t>(
        options.threads * options.allocations_per_thread * options.waves
    );
    result.pmr_operation_count = result.allocation_count * 2;
    result.logical_total_allocated_bytes = resource.total_allocated_bytes();
    return result;
}

bool syscall_succeeded(unsigned long result) {
    const auto signed_result = static_cast<long>(result);
    return !(signed_result < 0 && signed_result >= -4095);
}

void add_traced_mapping(VmTraceStats& stats, std::uint64_t bytes) {
    stats.total_mapped_bytes += bytes;
    stats.current_mapped_bytes += bytes;
}

void remove_traced_mapping(VmTraceStats& stats, std::uint64_t bytes) {
    stats.total_unmapped_bytes += bytes;
    stats.current_mapped_bytes =
        bytes > stats.current_mapped_bytes ? 0 : stats.current_mapped_bytes - bytes;
}

void handle_syscall_exit(SyscallTraceState& state, unsigned long result, VmTraceStats& stats) {
    if (!stats.counting) {
        return;
    }

    if (!syscall_succeeded(result)) {
        return;
    }

    switch (state.number) {
        case SYS_mmap:
            ++stats.mmap_count;
            add_traced_mapping(stats, state.arg1);
            break;
        case SYS_munmap:
            ++stats.munmap_count;
            remove_traced_mapping(stats, state.arg1);
            break;
        case SYS_mremap:
            ++stats.mremap_count;
            if (state.arg2 > state.arg1) {
                add_traced_mapping(stats, state.arg2 - state.arg1);
            } else if (state.arg1 > state.arg2) {
                remove_traced_mapping(stats, state.arg1 - state.arg2);
            }
            break;
        case SYS_brk: {
            ++stats.brk_count;
            const unsigned long current_brk = result;
            if (stats.last_brk == 0) {
                stats.last_brk = current_brk;
                break;
            }
            if (current_brk > stats.last_brk) {
                const std::uint64_t growth = current_brk - stats.last_brk;
                stats.total_brk_growth_bytes += growth;
                stats.current_brk_growth_bytes += growth;
            } else if (stats.last_brk > current_brk) {
                const std::uint64_t shrink = stats.last_brk - current_brk;
                stats.total_brk_shrink_bytes += shrink;
                stats.current_brk_growth_bytes =
                    shrink > stats.current_brk_growth_bytes ? 0 : stats.current_brk_growth_bytes - shrink;
            }
            stats.last_brk = current_brk;
            break;
        }
        default:
            break;
    }
}

double ratio_or_zero(std::size_t numerator, std::size_t denominator) {
    if (denominator == 0) {
        return 0.0;
    }
    return static_cast<double>(numerator) / static_cast<double>(denominator);
}

std::string make_result_json(
    const Options& options,
    const BackendSpec& backend,
    const WorkloadResult& result,
    const tracking_memory_resource& tracker
) {
    const double seconds = static_cast<double>(result.duration_ns) / 1'000'000'000.0;
    const double operations_per_second = seconds == 0.0
        ? 0.0
        : static_cast<double>(result.pmr_operation_count) / seconds;
    const double bytes_per_second = seconds == 0.0
        ? 0.0
        : static_cast<double>(result.logical_total_allocated_bytes) / seconds;
    const bool success = result.error.empty();

    std::ostringstream out;
    out << "{\n"
        << "  \"backend\": \"" << json_escape(backend.name) << "\",\n"
        << "  \"success\": " << (success ? "true" : "false") << ",\n";
    if (!success) {
        out << "  \"error\": \"" << json_escape(result.error) << "\",\n";
    }
    out << "  \"parameters\": {\n"
        << "    \"threads\": " << options.threads << ",\n"
        << "    \"allocations_per_thread\": " << options.allocations_per_thread << ",\n"
        << "    \"waves\": " << options.waves << ",\n"
        << "    \"pin_threads\": " << (options.pin_threads ? "true" : "false") << ",\n"
        << "    \"progress\": " << (options.progress ? "true" : "false") << ",\n"
        << "    \"vm_trace\": " << (options.vm_trace ? "true" : "false") << ",\n"
        << "    \"size_profile\": \"" << json_escape(options.size_profile) << "\"\n"
        << "  },\n"
        << "  \"throughput\": {\n"
        << "    \"duration_ns\": " << result.duration_ns << ",\n"
        << "    \"allocation_count\": " << result.allocation_count << ",\n"
        << "    \"pmr_operation_count\": " << result.pmr_operation_count << ",\n"
        << "    \"operations_per_second\": " << operations_per_second << ",\n"
        << "    \"bytes_per_second\": " << bytes_per_second << "\n"
        << "  },\n"
        << "  \"memory\": {\n"
        << "    \"logical_current_bytes\": " << tracker.current_bytes() << ",\n"
        << "    \"logical_total_allocated_bytes\": " << tracker.total_allocated_bytes() << ",\n"
        << "    \"allocation_count\": " << tracker.allocation_count() << ",\n"
        << "    \"memory_overhead_ratio\": null,\n"
        << "    \"vm_trace_required_for_overhead\": true\n"
        << "  }\n";
    out << "}";
    return out.str();
}

std::string run_backend_json(const Options& options, const BackendSpec& backend) {
    const std::vector<std::size_t> sizes = make_size_profile(options);

    BackendInstance instance = backend.create();
    tracking_memory_resource tracker(instance.resource);
    const WorkloadResult result = run_workload(options, tracker, sizes);

    return make_result_json(
        options,
        backend,
        result,
        tracker
    );
}

bool write_all(int fd, std::string_view data) {
    const char* ptr = data.data();
    std::size_t remaining = data.size();
    while (remaining != 0) {
        const ssize_t written = write(fd, ptr, remaining);
        if (written <= 0) {
            return false;
        }
        ptr += written;
        remaining -= static_cast<std::size_t>(written);
    }
    return true;
}

std::string read_all(int fd) {
    std::string out;
    char buffer[4096];
    while (true) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count == 0) {
            break;
        }
        if (count < 0) {
            throw std::runtime_error("failed to read child output");
        }
        out.append(buffer, static_cast<std::size_t>(count));
    }
    return out;
}

std::string error_result_json(const BackendSpec& backend, std::string_view message) {
    std::ostringstream out;
    out << "{\n"
        << "  \"backend\": \"" << json_escape(backend.name) << "\",\n"
        << "  \"success\": false,\n"
        << "  \"error\": \"" << json_escape(message) << "\"\n"
        << "}";
    return out.str();
}

void set_ptrace_options(pid_t tid) {
    const long options = PTRACE_O_TRACESYSGOOD |
        PTRACE_O_TRACECLONE |
        PTRACE_O_TRACEFORK |
        PTRACE_O_TRACEVFORK;
    (void)ptrace(PTRACE_SETOPTIONS, tid, nullptr, reinterpret_cast<void*>(options));
}

void resume_traced_task(pid_t tid, int signal = 0) {
    (void)ptrace(PTRACE_SYSCALL, tid, nullptr, reinterpret_cast<void*>(static_cast<long>(signal)));
}

void handle_syscall_stop(pid_t tid, std::unordered_map<pid_t, SyscallTraceState>& states, VmTraceStats& stats) {
    user_regs_struct regs{};
    if (ptrace(PTRACE_GETREGS, tid, nullptr, &regs) != 0) {
        stats.error = "PTRACE_GETREGS failed";
        return;
    }

    SyscallTraceState& state = states[tid];
    if (!state.in_syscall) {
        state.in_syscall = true;
        state.number = static_cast<long>(regs.orig_rax);
        state.arg0 = regs.rdi;
        state.arg1 = regs.rsi;
        state.arg2 = regs.rdx;
        return;
    }

    handle_syscall_exit(state, regs.rax, stats);
    state = SyscallTraceState{};
}

VmTraceStats trace_child_vm_syscalls(pid_t pid) {
    VmTraceStats stats;
    stats.enabled = true;
    std::unordered_map<pid_t, SyscallTraceState> states;
    int live_tasks = 1;
    int status = 0;

    if (waitpid(pid, &status, 0) < 0) {
        stats.error = "initial waitpid failed";
        return stats;
    }

    if (!WIFSTOPPED(status)) {
        stats.error = "child did not stop for ptrace setup";
        return stats;
    }

    states.emplace(pid, SyscallTraceState{});
    set_ptrace_options(pid);
    resume_traced_task(pid);

    while (live_tasks > 0) {
        const pid_t tid = waitpid(-1, &status, __WALL);
        if (tid < 0) {
            stats.error = "waitpid failed while tracing";
            return stats;
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            states.erase(tid);
            --live_tasks;
            continue;
        }

        if (!WIFSTOPPED(status)) {
            continue;
        }

        const int stop_signal = WSTOPSIG(status);
        const int ptrace_event = status >> 16;

        if (states.find(tid) == states.end()) {
            states.emplace(tid, SyscallTraceState{});
            ++live_tasks;
            set_ptrace_options(tid);
        }

        if (stop_signal == (SIGTRAP | 0x80)) {
            handle_syscall_stop(tid, states, stats);
            resume_traced_task(tid);
            continue;
        }

        if (tid == pid && stop_signal == SIGUSR1) {
            stats.counting = true;
            ++stats.trace_start_count;
            resume_traced_task(tid);
            continue;
        }

        if (tid == pid && stop_signal == SIGUSR2) {
            stats.counting = false;
            ++stats.trace_stop_count;
            resume_traced_task(tid);
            continue;
        }

        if (stop_signal == SIGTRAP && ptrace_event != 0) {
            if (ptrace_event == PTRACE_EVENT_CLONE ||
                ptrace_event == PTRACE_EVENT_FORK ||
                ptrace_event == PTRACE_EVENT_VFORK) {
                unsigned long new_tid = 0;
                if (ptrace(PTRACE_GETEVENTMSG, tid, nullptr, &new_tid) == 0 && new_tid != 0) {
                    states.emplace(static_cast<pid_t>(new_tid), SyscallTraceState{});
                    ++live_tasks;
                }
            }
            resume_traced_task(tid);
            continue;
        }

        resume_traced_task(tid, stop_signal == SIGSTOP ? 0 : stop_signal);
    }

    stats.complete = stats.error.empty();
    return stats;
}

std::string vm_trace_json(const VmTraceStats& stats) {
    const std::uint64_t total_virtual_allocated =
        stats.total_mapped_bytes + stats.total_brk_growth_bytes;
    const std::uint64_t current_virtual =
        stats.current_mapped_bytes + stats.current_brk_growth_bytes;

    std::ostringstream out;
    out << "{\n"
        << "    \"enabled\": " << (stats.enabled ? "true" : "false") << ",\n"
        << "    \"complete\": " << (stats.complete ? "true" : "false") << ",\n";
    if (!stats.error.empty()) {
        out << "    \"error\": \"" << json_escape(stats.error) << "\",\n";
    }
    out << "    \"trace_start_count\": " << stats.trace_start_count << ",\n"
        << "    \"trace_stop_count\": " << stats.trace_stop_count << ",\n"
        << "    \"mmap_count\": " << stats.mmap_count << ",\n"
        << "    \"munmap_count\": " << stats.munmap_count << ",\n"
        << "    \"mremap_count\": " << stats.mremap_count << ",\n"
        << "    \"brk_count\": " << stats.brk_count << ",\n"
        << "    \"total_mapped_bytes\": " << stats.total_mapped_bytes << ",\n"
        << "    \"total_unmapped_bytes\": " << stats.total_unmapped_bytes << ",\n"
        << "    \"current_mapped_bytes\": " << stats.current_mapped_bytes << ",\n"
        << "    \"total_brk_growth_bytes\": " << stats.total_brk_growth_bytes << ",\n"
        << "    \"total_brk_shrink_bytes\": " << stats.total_brk_shrink_bytes << ",\n"
        << "    \"current_brk_growth_bytes\": " << stats.current_brk_growth_bytes << ",\n"
        << "    \"total_virtual_allocated_bytes\": " << total_virtual_allocated << ",\n"
        << "    \"current_virtual_bytes\": " << current_virtual << "\n"
        << "  }";
    return out.str();
}

std::size_t extract_json_size(std::string_view json, std::string_view key) {
    const std::size_t key_pos = json.find(key);
    if (key_pos == std::string_view::npos) {
        return 0;
    }

    std::size_t pos = key_pos + key.size();
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])) != 0) {
        ++pos;
    }

    std::size_t value = 0;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos])) != 0) {
        value = value * 10 + static_cast<std::size_t>(json[pos] - '0');
        ++pos;
    }
    return value;
}

std::string ratio_json(std::uint64_t numerator, std::size_t denominator) {
    if (denominator == 0) {
        return "null";
    }

    std::ostringstream out;
    out << ratio_or_zero(static_cast<std::size_t>(numerator), denominator);
    return out.str();
}

void replace_first(std::string& text, std::string_view needle, std::string_view replacement) {
    const std::size_t pos = text.find(needle);
    if (pos == std::string::npos) {
        return;
    }

    text.replace(pos, needle.size(), replacement);
}

std::string append_vm_trace_json(std::string record_json, const VmTraceStats& stats) {
    if (stats.enabled && stats.complete) {
        const std::size_t logical_total = extract_json_size(record_json, "\"logical_total_allocated_bytes\": ");

        replace_first(
            record_json,
            "\"memory_overhead_ratio\": null",
            "\"memory_overhead_ratio\": " + ratio_json(stats.total_mapped_bytes, logical_total)
        );
    }

    const auto last_brace = record_json.find_last_of('}');
    if (last_brace == std::string::npos) {
        return record_json;
    }

    std::size_t insert_pos = last_brace;
    while (insert_pos > 0 && std::isspace(static_cast<unsigned char>(record_json[insert_pos - 1])) != 0) {
        --insert_pos;
    }

    std::string out = record_json.substr(0, insert_pos);
    out += ",\n  \"vm_trace\": ";
    out += vm_trace_json(stats);
    out += '\n';
    out += record_json.substr(last_brace);
    return out;
}

std::string run_backend_in_child(const Options& options, const BackendSpec& backend) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        return error_result_json(backend, "pipe failed");
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return error_result_json(backend, "fork failed");
    }

    if (pid == 0) {
        close(pipe_fds[0]);
        if (options.progress) {
            std::cerr << "[multialloc] backend " << backend.name
                      << " started (threads=" << options.threads
                      << ", allocations_per_thread=" << options.allocations_per_thread
                      << ", waves=" << options.waves
                      << ", vm_trace=" << (options.vm_trace ? "true" : "false")
                      << ")\n";
        }
        if (options.vm_trace) {
            if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) != 0) {
                const std::string output = error_result_json(backend, "PTRACE_TRACEME failed");
                (void)write_all(pipe_fds[1], output);
                close(pipe_fds[1]);
                _exit(1);
            }
            raise(SIGSTOP);
        }

        std::string output;
        int exit_code = 0;
        try {
            output = run_backend_json(options, backend);
        } catch (const std::exception& ex) {
            output = error_result_json(backend, ex.what());
            exit_code = 1;
        } catch (...) {
            output = error_result_json(backend, "unknown child exception");
            exit_code = 1;
        }

        (void)write_all(pipe_fds[1], output);
        if (options.progress) {
            std::cerr << "[multialloc] backend " << backend.name << " finished\n";
        }
        close(pipe_fds[1]);
        _exit(exit_code);
    }

    close(pipe_fds[1]);
    VmTraceStats trace_stats;
    if (options.vm_trace) {
        trace_stats = trace_child_vm_syscalls(pid);
    }

    std::string output;
    try {
        output = read_all(pipe_fds[0]);
    } catch (const std::exception& ex) {
        output = error_result_json(backend, ex.what());
    }
    close(pipe_fds[0]);

    int status = 0;
    if (!options.vm_trace && waitpid(pid, &status, 0) < 0) {
        return error_result_json(backend, "waitpid failed");
    }

    if (output.empty()) {
        return error_result_json(backend, "child produced no output");
    }

    if (options.vm_trace) {
        output = append_vm_trace_json(std::move(output), trace_stats);
    }

    return output;
}

std::vector<BackendSpec> select_backends(const Options& options, std::vector<BackendSpec> registry) {
    if (options.backend == "all") {
        return registry;
    }

    for (const BackendSpec& backend : registry) {
        if (backend.name == options.backend) {
            return {backend};
        }
    }

    throw std::invalid_argument("unknown backend: " + options.backend);
}

void write_json_output(const Options& options, const std::vector<std::string>& records) {
    std::ostringstream out;
    out << "{\n  \"results\": [\n";
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (i != 0) {
            out << ",\n";
        }

        std::istringstream record_stream(records[i]);
        std::string line;
        bool first_line = true;
        while (std::getline(record_stream, line)) {
            if (!first_line) {
                out << '\n';
            }
            out << "    " << line;
            first_line = false;
        }
    }
    out << "\n  ]\n}\n";

    if (options.json_path == "-") {
        std::cout << out.str();
        return;
    }

    const std::filesystem::path output_path(options.json_path);
    if (const auto parent = output_path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream file(output_path);
    if (!file) {
        throw std::runtime_error("failed to open JSON output: " + options.json_path);
    }
    file << out.str();
}

} // namespace

int main(int argc, char** argv) {
    try {
        Options options = parse_options(argc, argv);
        std::vector<BackendSpec> registry = make_backend_registry();
        register_custom_backends(registry);
        std::vector<BackendSpec> backends = select_backends(options, std::move(registry));

        std::vector<std::string> records;
        records.reserve(backends.size());
        for (const BackendSpec& backend : backends) {
            records.push_back(run_backend_in_child(options, backend));
        }

        write_json_output(options, records);
    } catch (const std::exception& ex) {
        std::cerr << "multi_node_bench_multialloc: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
