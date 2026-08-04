#pragma once

#include "../../common/server_interface.h"
#include <atomic>
#include <cstdint>

// ==============================================================================
// IoUringZcEchoServer — TCP echo server using Linux io_uring + SEND_ZC
// ==============================================================================
// Same architecture as the vanilla io_uring flavor, but:
//
//   1. Registers a pool of buffers with the kernel (IORING_REGISTER_BUFFERS)
//      so recv buffers are pre-pinned — no page-fault overhead per recv.
//
//   2. Uses IORING_OP_SEND_ZC for the echo — the NIC DMAs directly from
//      the userspace buffer, eliminating the kernel-side copy on send.
//      This means the buffer is pinned until SEND_ZC completes, so we
//      cannot immediately submit the next recv — we wait for the SEND_ZC
//      completion first.
//
// Flow per connection:
//
//   recv → buf  →  SEND_ZC(buf)  →  SEND_ZC done  →  recv → buf  → ...
//
// No thread pool. No mutex. True zero-copy on the send path.
// ==============================================================================

#ifdef __linux__
#include <liburing.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

class IoUringZcEchoServer : public ServerInterface {
public:
    explicit IoUringZcEchoServer(uint16_t port, size_t /*unused*/);
    ~IoUringZcEchoServer() override;

    void run() override;
    void shutdown() override;

private:
#ifdef __linux__
    struct Request {
        enum Op { ACCEPT, RECV, SEND_ZC };

        Op      op;
        int     client_fd;
        char    buf[4096];
        size_t  len;
    };

    void event_loop();
    void submit_accept();
    void handle_accept(int client_fd);
    void submit_recv(Request* req);
    void handle_recv(Request* req, int nread);
    void submit_send_zc(Request* req);
    void handle_send_zc(Request* req);

    int         listen_fd_{-1};
    io_uring    ring_{};
    sockaddr_in addr_{};
    // Pre-registered buffer index for IORING_REGISTER_BUFFERS
    int         buf_group_{0};
#endif

    uint16_t port_;
    std::atomic<bool> running_{true};
};