#pragma once

#include "../../common/server_interface.h"
#include <atomic>
#include <cstdint>

// ==============================================================================
// IoUringEchoServer — TCP echo server using Linux io_uring
// ==============================================================================
// Architecturally different from the kqueue flavor:
//
//   kqueue:    net thread → read → [mutex] → worker → write
//   io_uring:  single thread → submit recv → COMPLETE → submit send → COMPLETE
//
// No thread pool. No mutex. The kernel handles all I/O asynchronously.
// ==============================================================================
//
// On non-Linux platforms this compiles to a stub that prints an error.
//
// ==============================================================================

#ifdef __linux__
#include <liburing.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

class IoUringEchoServer : public ServerInterface {
public:
    explicit IoUringEchoServer(uint16_t port, size_t /*unused*/);
    ~IoUringEchoServer() override;

    void run() override;
    void shutdown() override;

private:
#ifdef __linux__
    // -- Request tracking --
    // Each in-flight I/O operation gets one of these. Stored in the
    // io_uring SQE's user_data so we know what to do on completion.
    //
    // RECV requests own their buf and reuse it across recv cycles.
    // SEND requests receive a COPY of the data so the original RECV
    // request can immediately start reading the next message.
    struct Request {
        enum Op { ACCEPT, RECV, SEND };

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
    void handle_send(Request* req);

    int       listen_fd_{-1};
    io_uring  ring_{};
    sockaddr_in addr_{};
#endif

    uint16_t port_;
    std::atomic<bool> running_{true};
};