// ==============================================================================
// io_uring echo server — full implementation (Linux) + stub (other platforms)
// ==============================================================================
//
// This file implements a single-threaded, fully async TCP echo server using
// Linux io_uring. The architecture:
//
//   1. Submit an accept request to the kernel
//   2. When a client connects, submit a recv request
//   3. When data arrives, submit a send request to echo it back
//   4. When send completes, submit another recv for the next message
//   5. Repeat 3-4 until the client disconnects
//
// All steps run on ONE thread. No mutex, no condition variable, no thread
// pool. The kernel does all the I/O work asynchronously.
//
// Why this is faster than kqueue+threads:
//   - Zero syscalls on the data path (submissions are shared-memory writes)
//   - Zero mutex contention (single thread, no shared state)
//   - Zero context switches between workers (no thread pool)
//   - Batching: one io_uring_wait_cqe() can return 64 completions
//
// The data still gets copied (kernel -> req->buf on recv, req->buf -> kernel
// on send). The savings are in control-path overhead, not data copies.
// ==============================================================================

#include "server.h"

#include <iostream>
#include <stdexcept>
#include <unistd.h>

#ifndef __linux__
// ==============================================================================
// Stub for non-Linux platforms
// ==============================================================================
IoUringEchoServer::IoUringEchoServer(uint16_t, size_t) : port_(0) {
    std::cerr << "[io_uring] Fatal: io_uring is Linux-only\n";
    throw std::runtime_error("io_uring unsupported on this platform");
}
IoUringEchoServer::~IoUringEchoServer() = default;
void IoUringEchoServer::run() {}
void IoUringEchoServer::shutdown() { running_ = false; }

#else
// ==============================================================================
// Linux implementation using liburing
// ==============================================================================

#include <cstring>
#include <fcntl.h>

#include <arpa/inet.h>
#include <liburing.h>
#include <sys/socket.h>

// ==============================================================================
// Constructor
// ==============================================================================
// 1. Create a non-blocking TCP listen socket
// 2. Bind to 0.0.0.0:port
// 3. Initialise the io_uring ring with QUEUE_DEPTH entries
//
// io_uring_queue_init() allocates the Submission Queue and Completion Queue
// in shared memory between userspace and the kernel. The SQ and CQ are
// lock-free ring buffers using atomic head/tail indices with memory_order
// acquire/release semantics — no mutexes involved.
// ==============================================================================
IoUringEchoServer::IoUringEchoServer(uint16_t port, size_t /*unused*/)
    : port_(port) {

    // -- Listen socket --
    listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd_ < 0) {
        throw std::runtime_error("socket: " + std::string(strerror(errno)));
    }

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    addr_.sin_family      = AF_INET;
    addr_.sin_addr.s_addr = INADDR_ANY;
    addr_.sin_port        = htons(port_);

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr_), sizeof(addr_)) < 0) {
        close(listen_fd_);
        throw std::runtime_error("bind: " + std::string(strerror(errno)));
    }
    if (listen(listen_fd_, SOMAXCONN) < 0) {
        close(listen_fd_);
        throw std::runtime_error("listen: " + std::string(strerror(errno)));
    }

    // -- Initialise io_uring --
    // QUEUE_DEPTH = 256 means up to 256 I/O operations can be in flight
    // simultaneously. If we exceed this, io_uring_get_sqe() returns NULL
    // and we must wait for completions before submitting more.
    int ret = io_uring_queue_init(256, &ring_, 0);
    if (ret < 0) {
        close(listen_fd_);
        throw std::runtime_error("io_uring_queue_init: " + std::to_string(-ret));
    }

    std::cout << "[io_uring] Echo server on port " << port_ << "\n";
}

IoUringEchoServer::~IoUringEchoServer() {
    io_uring_queue_exit(&ring_);
    if (listen_fd_ >= 0) close(listen_fd_);
}

void IoUringEchoServer::shutdown() {
    running_ = false;
}

void IoUringEchoServer::run() {
    event_loop();
}

// ==============================================================================
// Event loop — pure async, no mutex, no thread pool
// ==============================================================================
// IMPORTANT SUBMISSION RULE:
// We NEVER call io_uring_submit() from inside the io_uring_for_each_cqe()
// loop. Submitting during CQ iteration can cause a newly-completed CQE to
// be written while we're iterating, corrupting the count used by
// io_uring_cq_advance(). This leads to misaligned CQ processing and errors
// like EBADF (bad file descriptor).
//
// Correct pattern (two-phase):
//   1. Iterate CQEs, handle each completion, PREPARE new SQEs (but don't
//      submit — io_uring_get_sqe() just writes to shared memory).
//   2. After the iteration, call io_uring_cq_advance() to consume the
//      completions, THEN io_uring_submit() once for all new work.
//
// Flow per connection:
//   1. submit_accept()          — ask kernel to accept the next client
//   2. handle_accept(new_fd)    — client arrived, prepare recv for it
//   3. handle_recv(data)        — got data, prepare send to echo it back
//   4. handle_send()            — send done, prepare recv again
//   ... repeat 3-4 until client disconnects
//   5. handle_recv(0)           — client closed, done
//
// All steps run on the same thread. No locks needed.
//
// The 1-second timeout on io_uring_wait_cqe_timeout() lets us check the
// running_ flag regularly for clean shutdown.
// ==============================================================================
void IoUringEchoServer::event_loop() {
    constexpr int QUEUE_DEPTH = 256;

    // Prime the first accept — without this, no clients can connect
    submit_accept();

    while (running_) {
        // Wait for completion (1s timeout for shutdown check)
        __kernel_timespec ts = {1, 0};
        io_uring_cqe* cqe    = nullptr;

        // io_uring_wait_cqe_timeout() sleeps until either:
        //   a) a completion arrives in the CQ, or
        //   b) the 1-second timeout expires
        // This is the ONLY blocking syscall in the entire event loop.
        int ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
        if (ret == -ETIME || ret == -EINTR) continue;
        if (ret < 0) {
            std::cerr << "io_uring_wait_cqe: " << -ret << "\n";
            break;
        }

        // -- PHASE 1: process completions, prepare new SQEs (no submit!) --
        unsigned head;
        unsigned count = 0;
        io_uring_for_each_cqe(&ring_, head, cqe) {
            // io_uring_cqe_get_data() returns the Request* we attached
            // via io_uring_sqe_set_data() when we submitted the SQE.
            auto* req = reinterpret_cast<Request*>(io_uring_cqe_get_data(cqe));
            int   res = cqe->res;  // result: new fd, bytes read, bytes sent
            ++count;

            switch (req->op) {
            case Request::ACCEPT:
                handle_accept(res);  // prepares recv SQE (no submit)
                delete req;          // accept is one-shot — clean up
                break;
            case Request::RECV:
                handle_recv(req, res);  // prepares send SQE (no submit)
                // req is NOT deleted here — it's reused for the send
                break;
            case Request::SEND:
                handle_send(req);  // prepares recv SQE (no submit)
                // req is NOT deleted here — it's reused for the next recv
                break;
            }
        }

        // -- PHASE 2: consume completions, THEN submit all new work at once --
        io_uring_cq_advance(&ring_, count);  // free the consumed CQEs
        submit_accept();                     // keep accept primed
        io_uring_submit(&ring_);             // ONE syscall for all new SQEs
    }
}

// ==============================================================================
// submit_accept  — ask the kernel to accept the next connection
// handle_accept  — got a new client, start reading from it
// ==============================================================================
// accept() creates a NEW file descriptor for each client. The listen fd
// never changes — it stays open forever to accept() more clients.
// ==============================================================================
void IoUringEchoServer::submit_accept() {
    auto* req   = new Request{Request::ACCEPT, -1, {}, 0};
    auto* sqe   = io_uring_get_sqe(&ring_);
    if (!sqe) { delete req; return; }  // ring full — retry next iteration

    // io_uring_prep_accept() fills the SQE with "call accept4() on listen_fd"
    // This is NOT a syscall — it's writing to a memory-mapped buffer.
    io_uring_prep_accept(sqe, listen_fd_, nullptr, nullptr, SOCK_NONBLOCK);
    io_uring_sqe_set_data(sqe, req);  // attach our tracking struct
    // NOTE: no io_uring_submit() here — the event loop submits in phase 2
}

void IoUringEchoServer::handle_accept(int client_fd) {
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            std::cerr << "accept error: " << strerror(errno) << "\n";
        return;
    }
    // Start reading from this client (prepares the recv SQE, no submit)
    submit_recv(new Request{Request::RECV, client_fd, {}, 0});
}

// ==============================================================================
// submit_recv  — submit an async read from a client fd
// handle_recv  — data arrived, echo it back or close on disconnect
// ==============================================================================
// NOTE: despite the name, submit_recv() only PREPARES the SQE. The actual
// io_uring_submit() happens once in the event loop's phase 2. This keeps
// all submissions batched outside the CQ iteration.
// ==============================================================================
void IoUringEchoServer::submit_recv(Request* req) {
    auto* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) { delete req; return; }

    // io_uring_prep_recv() fills the SQE with "read from this fd into this buf"
    // The kernel will write data into req->buf directly (a copy from kernel
    // socket buffer to userspace).
    io_uring_prep_recv(sqe, req->client_fd, req->buf, sizeof(req->buf), 0);
    io_uring_sqe_set_data(sqe, req);
}

void IoUringEchoServer::handle_recv(Request* req, int nread) {
    if (nread <= 0) {
        if (nread == 0) {
            // nread == 0 means the client closed the connection cleanly
            close(req->client_fd);
        } else if (nread != -EAGAIN) {
            std::cerr << "recv error: " << -nread << "\n";
            close(req->client_fd);
        }
        delete req;
        return;
    }

    // Echo it back
    // io_uring_prep_send() copies data from req->buf into a kernel buffer
    // immediately at submit time. This means the userspace buffer is free
    // instantly and we can submit the next recv right after submitting the
    // send — the kernel has its own copy of the data.
    req->op  = Request::SEND;
    req->len = static_cast<size_t>(nread);
    submit_send(req);
}

// ==============================================================================
// submit_send  — submit an async write to echo data back
// handle_send  — send completed, go back to reading from this client
// ==============================================================================
// Vanilla io_uring_prep_send() does an internal memcpy of req->buf into a
// kernel socket buffer at submit time. The kernel then sends from its own
// buffer. This means:
//   - req->buf is free immediately after submit_send() returns
//   - We can submit the next recv right away (which we do in handle_send)
//   - But there IS a copy: user buf -> kernel buf (the "1 copy" on send)
//
// Compare with the iouring_zc flavor which eliminates this copy.
// ==============================================================================
void IoUringEchoServer::submit_send(Request* req) {
    auto* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) { delete req; return; }

    io_uring_prep_send(sqe, req->client_fd, req->buf, req->len, 0);
    io_uring_sqe_set_data(sqe, req);
    // NOTE: no io_uring_submit() here — the event loop submits in phase 2
}

void IoUringEchoServer::handle_send(Request* req) {
    // Send completed — now wait for the next message from this client
    // The kernel has finished sending its copy of the data, and req->buf
    // was never touched by the kernel (it copied at submit time), so we
    // can safely reuse req->buf for the next recv.
    req->op  = Request::RECV;
    req->len = 0;
    submit_recv(req);
}

#endif  // __linux__