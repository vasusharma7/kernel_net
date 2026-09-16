// ==============================================================================
// ring_buffer_benchmark.cc — SPSCRingBuffer vs std::mutex
// ==============================================================================
//
// A clean, standalone benchmark comparing two synchronization primitives for
// a single-producer, single-consumer queue:
//
//   1. SPSCRingBuffer — lock-free, atomic loads/stores only
//   2. MutexQueue     — std::queue + std::mutex, busy-spin
//
// Both use the same busy-spin pattern when full/empty. The ONLY difference
// is the synchronization mechanism: atomics vs mutex. This isolates the
// pure cost of pthread_mutex_lock / pthread_mutex_unlock.
//
// Build:
//   g++ -std=c++17 -O3 -march=native -lpthread ring_buffer_benchmark.cc -o ring_buffer_benchmark
//
// Run:
//   ./ring_buffer_benchmark [--messages N]
// ==============================================================================

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// ==============================================================================
// CPU_PAUSE() — What does it do?
// ==============================================================================
//
// Both benchmarks (ring buffer AND mutex queue) busy-spin when the queue is
// full or empty. Without a pause hint, a spinloop compiles to:
//
//     spinloop:
//       check condition
//       if not ready → jump back to spinloop
//
// This is terrible for the CPU:
//
//   1. Memory ordering violation (x86): The CPU's speculative execution
//      assumes the loop will eventually exit and starts loading memory
//      ahead of time. When the condition finally changes, the CPU has
//      to flush its entire speculative pipeline — wasting ~10-20 cycles.
//      PAUSE tells the CPU "don't speculate, this is a spinloop."
//
//   2. Hyperthread starvation (x86): On a hyperthreaded core, two threads
//      share execution units. A tight spinloop hogs the pipeline, starving
//      the other logical core. PAUSE yields the pipeline to the other
//      thread, so the producer/consumer on the sibling core runs faster.
//
//   3. Memory bus contention: Spinning = hammering the same atomic variable
//      with loads. On ARM, the YIELD instruction hints the CPU to back off.
//
//   4. Power waste: Spinning at 100% CPU for no progress = heat, not work.
//
// The three implementations:
//
//   x86/x86_64:  __builtin_ia32_pause()
//     → emits the PAUSE instruction (formerly REP NOP)
//     → ~10 cycles, saves ~10-20 cycles on pipeline flush
//     → frees the pipeline for the sibling hyperthread
//
//   ARM/aarch64: asm volatile("yield" ::: "memory")
//     → emits the YIELD hint instruction
//     → similar effect: hints CPU this is a spinloop
//     → on big.LITTLE, can hint the scheduler to prefer a little core
//
//   Fallback:    std::this_thread::yield()
//     → THIS IS DIFFERENT — it's a kernel syscall, not a CPU hint
//     → tells the OS scheduler: "I'm done, give my timeslice to another thread"
//     → costs ~1µs (vs ~10ns for the CPU hints)
//     → only used on obscure platforms that lack both PAUSE and YIELD
//
// Why both benchmarks use it:
//   If SPSCRingBuffer used CPU_PAUSE() but MutexQueue used nothing,
//   the comparison would be unfair — the mutex version would burn more
//   CPU and get worse results. By using the SAME spin behavior, the
//   ONLY difference is atomics vs mutex.
// ==============================================================================
#if defined(__x86_64__) || defined(__i386__)
  #define CPU_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
  #define CPU_PAUSE() asm volatile("yield" ::: "memory")
#else
  #define CPU_PAUSE() std::this_thread::yield()
#endif

// ----------------------------------------------------------------------------
// Config
// ----------------------------------------------------------------------------
static constexpr size_t RING_SIZE = 4096;  // Must be power of 2

// ----------------------------------------------------------------------------
// 1. Lock-free SPSCRingBuffer
// ----------------------------------------------------------------------------
// Two atomic indices, each on its own cache line (alignas(64)).
// Producer writes to write_idx, consumer reads from read_idx.
// No mutex, no syscall, no kernel — pure userspace atomics.
// ----------------------------------------------------------------------------
template <typename T, size_t Size>
class SPSCRingBuffer {
    static_assert((Size & (Size - 1)) == 0, "Size must be a power of 2");
    alignas(64) std::atomic<size_t> write_idx_{0};
    alignas(64) std::atomic<size_t> read_idx_{0};
    std::vector<T> buffer_;
public:
    SPSCRingBuffer() : buffer_(Size) {}

    bool push(const T& item) {
        const size_t w = write_idx_.load(std::memory_order_relaxed);
        const size_t next = w + 1;
        if (next - read_idx_.load(std::memory_order_acquire) > Size) return false;
        buffer_[w & (Size - 1)] = item;
        write_idx_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const size_t r = read_idx_.load(std::memory_order_relaxed);
        if (r == write_idx_.load(std::memory_order_acquire)) return false;
        item = std::move(buffer_[r & (Size - 1)]);
        read_idx_.store(r + 1, std::memory_order_release);
        return true;
    }
};

// ----------------------------------------------------------------------------
// 2. MutexQueue — std::queue + std::mutex, busy-spin
// ----------------------------------------------------------------------------
// Same bounded semantics as SPSCRingBuffer: producer spins if full,
// consumer spins if empty. The ONLY difference is mutex vs atomics.
// ----------------------------------------------------------------------------
template <typename T>
class MutexQueue {
    std::queue<T> q_;
    std::mutex mtx_;
    size_t capacity_;
public:
    explicit MutexQueue(size_t cap) : capacity_(cap) {}

    void push(T item) {
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mtx_);
                if (q_.size() < capacity_) {
                    q_.push(std::move(item));
                    return;
                }
            }
            CPU_PAUSE();  // same spin behavior as ring buffer
        }
    }

    void pop(T& item) {
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mtx_);
                if (!q_.empty()) {
                    item = std::move(q_.front());
                    q_.pop();
                    return;
                }
            }
            CPU_PAUSE();  // same spin behavior as ring buffer
        }
    }
};

// ----------------------------------------------------------------------------
// Benchmark: SPSCRingBuffer
// ----------------------------------------------------------------------------
uint64_t bench_ring(size_t num_messages) {
    SPSCRingBuffer<uint64_t, RING_SIZE> ring;
    std::atomic<bool> done{false};

    auto producer = [&]() {
        for (size_t i = 0; i < num_messages; ) {
            if (ring.push(i)) {
                ++i;
            } else {
                CPU_PAUSE();  // ring full, spin
            }
        }
        done.store(true, std::memory_order_release);
    };

    auto consumer = [&]() {
        uint64_t val;
        while (true) {
            if (ring.pop(val)) {
                // got a value
            } else if (done.load(std::memory_order_acquire)) {
                while (ring.pop(val));  // drain remaining
                break;
            } else {
                CPU_PAUSE();  // ring empty, spin
            }
        }
    };

    auto start = std::chrono::steady_clock::now();
    std::thread t1(producer);
    std::thread t2(consumer);
    t1.join();
    t2.join();
    auto end = std::chrono::steady_clock::now();

    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
}

// ----------------------------------------------------------------------------
// Benchmark: MutexQueue
// ----------------------------------------------------------------------------
uint64_t bench_mutex(size_t num_messages) {
    MutexQueue<uint64_t> q(RING_SIZE);
    std::atomic<size_t> consumed{0};

    auto producer = [&]() {
        for (size_t i = 0; i < num_messages; ) {
            q.push(i);
            ++i;
        }
    };

    auto consumer = [&]() {
        uint64_t val;
        while (consumed.load(std::memory_order_relaxed) < num_messages) {
            q.pop(val);
            consumed.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto start = std::chrono::steady_clock::now();
    std::thread t1(producer);
    std::thread t2(consumer);
    t1.join();
    t2.join();
    auto end = std::chrono::steady_clock::now();

    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
}

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    size_t num_messages = 10'000'000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--messages") == 0 && i + 1 < argc)
            num_messages = std::stoul(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: ring_buffer_benchmark [--messages N]\n";
            return 0;
        }
    }

    std::cout << "\n";
    std::cout << "============================================================\n";
    std::cout << "  SPSCRingBuffer vs std::mutex\n";
    std::cout << "  Messages: " << num_messages << "  Ring capacity: " << RING_SIZE << "\n";
    std::cout << "============================================================\n\n";

    // Warmup (discard results — caches and branch predictors need to settle)
    bench_ring(100'000);
    bench_mutex(100'000);

    // Run each 3 times, take the best result
    auto run_bench = [&](auto fn, const char* name) {
        uint64_t best = UINT64_MAX;
        for (int iter = 0; iter < 3; ++iter) {
            auto t = fn();
            if (t < best) best = t;
        }
        double ms       = best / 1'000'000.0;
        double ops      = num_messages / (best / 1'000'000'000.0);
        double ns_per_op = static_cast<double>(best) / (num_messages * 2);  // push + pop
        std::cout << "  " << std::left << std::setw(28) << name
                  << std::right << std::setw(10) << std::fixed << std::setprecision(1) << ms
                  << " ms  " << std::setw(12) << static_cast<uint64_t>(ops)
                  << " ops/s  " << std::setw(6) << std::setprecision(1) << ns_per_op
                  << " ns/op\n";
        return ops;
    };

    double ring_ops  = run_bench([&]() { return bench_ring(num_messages); },  "SPSCRingBuffer");
    double mutex_ops = run_bench([&]() { return bench_mutex(num_messages); }, "MutexQueue");

    std::cout << "\n  ───────────────────────────────────────────────────────────\n\n";

    double ratio = ring_ops / mutex_ops;
    std::cout << "  >> SPSCRingBuffer is " << std::setprecision(1) << ratio
              << "x faster than MutexQueue\n";
    std::cout << "     (same busy-spin behavior — pure atomics vs mutex cost)\n\n";

    return 0;
}