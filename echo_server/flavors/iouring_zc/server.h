#pragma once

#include "../../common/server_interface.h"
#include <atomic>
#include <cstdint>

// ==============================================================================
// IoUringZcEchoServer — TCP echo server using Linux io_uring + SEND_ZC
// ==============================================================================
//
// Same single-threaded async architecture as the vanilla io_uring flavor,
// but uses IORING_OP_SEND_ZC for the echo send path.
//
// How SEND_ZC differs from normal send:
//
//   Normal send (io_uring_prep_send):
//     req->buf --memcpy--> kernel socket buffer --DMA--> NIC
//     ^ free immediately        ^ kernel owns the copy
//
//   SEND_ZC (io_uring_prep_send_zc):
//     req->buf --page pin--> [NIC DMAs directly from here]
//     ^ LOCKED until CQE       ^ no kernel copy
//
// With normal send, the kernel copies data from req->buf into a kernel
// buffer at submit time. req->buf is free immediately. With SEND_ZC, the
// kernel pins the physical pages backing req->buf and the NIC reads from
// them directly via DMA — zero copy. But req->buf is owned by the kernel
// until the SEND_ZC completes, so we CANNOT submit the next recv until
// handle_send_zc() runs.
//
// Trade-off:
//   - Small payloads (64B): SEND_ZC is SLOWER because page pin/unpin
//     overhead dwarfs the memcpy savings
//   - Large payloads (1MB+): SEND_ZC is FASTER because the memcpy cost
//     (~8us per MB) is eliminated
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
    bool        accept_pending_{false};  // only one ACCEPT in flight at a time
#endif

    uint16_t port_;
    std::atomic<bool> running_{true};
};