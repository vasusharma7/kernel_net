#pragma once

#include "../../common/server_interface.h"
#include <atomic>
#include <cstdint>

// ==============================================================================
// IoUringEchoServer — TCP echo server using Linux io_uring
// ==============================================================================
//
// io_uring is a Linux kernel interface for asynchronous I/O. It uses two
// shared-memory ring buffers between userspace and the kernel:
//
//   Submission Queue (SQ):  we write I/O requests here ("read this fd",
//                           "write to this fd")
//   Completion Queue (CQ):  the kernel writes results here ("read done",
//                           "write done")
//
// The key insight: submitting an SQE is a memory write (no syscall). Only
// io_uring_submit() and io_uring_wait_cqe() are actual syscalls. For an echo
// server, this means:
//
//   kqueue:    kevent() + read() + write() = 3 syscalls per echo
//   io_uring:  io_uring_wait_cqe()         = 1 syscall per echo
//              (submissions are batched in shared memory)
//
// Architecturally different from the kqueue flavor:
//
//   kqueue:    net thread -> read -> [mutex] -> worker -> write
//   io_uring:  single thread -> submit recv -> COMPLETE -> submit send -> COMPLETE
//
// No thread pool. No mutex. The kernel handles all I/O asynchronously.
// The single thread never blocks — it submits work, sleeps on completions,
// handles results, and submits more work.
//
// Data still crosses the kernel/userspace boundary (the kernel copies data
// into req->buf on recv, and copies from req->buf on send). The savings
// come from eliminating syscalls on the data path, not from zero-copy.
// See the iouring_zc flavor for zero-copy send.
//
// On non-Linux platforms this compiles to a stub that prints an error.
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
    // Each in-flight I/O operation gets one of these. We attach it to the
    // io_uring SQE via io_uring_sqe_set_data(), and the kernel gives it
    // back in the CQE via io_uring_cqe_get_data(). This is how we know
    // which client and what operation a completion belongs to.
    //
    // Flow per connection:
    //   ACCEPT:  waiting for a new client to connect
    //   RECV:    waiting for data from the client (into req->buf)
    //   SEND:    waiting for the echo write to complete (from req->buf)
    //
    // Vanilla io_uring uses io_uring_prep_send(), which copies data from
    // req->buf into a kernel buffer immediately at submit time. This means
    // the userspace buffer is free instantly and we can submit the next
    // recv right after submitting the send — the kernel has its own copy.
    struct Request {
        enum Op { ACCEPT, RECV, SEND };

        Op      op;          // what kind of operation is this?
        int     client_fd;   // which socket does this belong to?
        char    buf[4096];   // buffer for incoming/outgoing data
        size_t  len;         // how many bytes to send
    };

    void event_loop();
    void submit_accept();
    void handle_accept(int client_fd);
    void submit_recv(Request* req);
    void handle_recv(Request* req, int nread);
    void submit_send(Request* req);
    void handle_send(Request* req);

    int       listen_fd_{-1};
    io_uring  ring_{};       // the io_uring instance (SQ + CQ)
    sockaddr_in addr_{};
    bool      accept_pending_{false};  // only one ACCEPT in flight at a time
#endif

    uint16_t port_;
    std::atomic<bool> running_{true};
};