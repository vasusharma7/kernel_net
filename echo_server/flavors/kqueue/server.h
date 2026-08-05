#pragma once

#include "../../common/thread_pool.h"
#include <atomic>
#include <cstdint>
#include <vector>

// ==============================================================================
// EchoServer — TCP echo server using macOS kqueue
// ==============================================================================
// kqueue is macOS/BSD's I/O event notification system. This flavor uses a
// single network thread (kqueue event loop) + a ThreadPool for echo writes.
//
// On non-macOS platforms this compiles to a stub that prints an error.
// ==============================================================================

#ifdef __APPLE__

#include <sys/event.h>

class EchoServer {
public:
    EchoServer(uint16_t port, size_t num_workers);
    ~EchoServer();

    void run();
    void shutdown();

private:
    void event_loop();
    void accept_connection(int listen_fd);
    void handle_read(int client_fd);

    int listen_fd_;
    int kq_;  // kqueue fd
    uint16_t port_;
    ThreadPool pool_;
    std::atomic<bool> running_{true};

    static constexpr int MAX_EVENTS = 1024;
    static constexpr size_t BUF_SIZE = 4096;
};

#else
// Stub for non-macOS platforms
class EchoServer {
public:
    EchoServer(uint16_t port, size_t num_workers);
    ~EchoServer();
    void run();
    void shutdown();
private:
    uint16_t port_;
    ThreadPool pool_;
};
#endif