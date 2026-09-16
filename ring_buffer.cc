#include <atomic>
#include <vector>
#include <cstdint>
#include <iostream>

template <typename T, size_t Size>
class SPSCRingBuffer {
    static_assert((Size & (Size - 1)) == 0, "Size must be a power of 2");

private:
    // alignas(64) prevents "False Sharing" in CPU caches
    alignas(64) std::atomic<size_t> write_idx{0};
    alignas(64) std::atomic<size_t> read_idx{0};
    
    // Contiguous memory layout
    std::vector<T> buffer;

public:
    SPSCRingBuffer() : buffer(Size) {}

    bool push(const T& item) {
        // memory_order_relaxed is fast because we just want the current value
        const size_t current_write = write_idx.load(std::memory_order_relaxed);
        const size_t next_write = current_write + 1;

        // Check if full (using acquire to synchronize with consumer)
        if (next_write - read_idx.load(std::memory_order_acquire) > Size) {
            return false; // Buffer is full
        }

        // Bitwise AND for fast wrap-around (requires Size to be power of 2)
        buffer[current_write & (Size - 1)] = item;

        // Release semantics ensure the data write is visible before the index update
        write_idx.store(next_write, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const size_t current_read = read_idx.load(std::memory_order_relaxed);

        // Check if empty
        if (current_read == write_idx.load(std::memory_order_acquire)) {
            return false; // Buffer is empty
        }

        item = buffer[current_read & (Size - 1)];
        read_idx.store(current_read + 1, std::memory_order_release);
        return true;
    }
};

int main() {
    // 1024 capacity queue for LLM token IDs
    SPSCRingBuffer<int, 1024> token_queue; 
    
    token_queue.push(42); // Simulate LLM outputting a token
    
    int received_token;
    if (token_queue.pop(received_token)) {
        std::cout << "PDA received token: " << received_token << std::endl;
    }
    return 0;
}