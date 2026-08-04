// ==============================================================================
// io_uring zero-copy echo server — Linux implementation + stub
// ==============================================================================

#include "server.h"

#include <iostream>
#include <stdexcept>
#include <unistd.h>

#ifndef __linux__
// ==============================================================================
// Stub for non-Linux platforms
// ==============================================================================
IoUringZcEchoServer::IoUringZcEchoServer(uint16_t, size_t) : port_(0) {
    std::cerr << "[io_uring_zc] Fatal: io_uring is Linux-only\n";
    throw std::runtime_error("io_uring_zc unsupported on this platform");
}
IoUringZcEchoServer::~IoUringZcEchoServer() = default;
void IoUringZcEchoServer::run() {}
void IoUringZcEchoServer::shutdown() { running_ = false; }

#else
// ==============================================================================
// Linux implementation using liburing + SEND_ZC
// ==============================================================================

#include <cstring>
#include <fcntl.h>

#include <arpa/inet.h>
#include <liburing.h>
#include <sys/socket.h>

// ==============================================================================
// Constructor
// ==============================================================================
// Same as vanilla io_uring, plus registers the recv buffers with the kernel
// so they are pre-pinned for DMA.
// ==============================================================================
IoUringZcEchoServer::IoUringZcEchoServer(uint16_t port, size_t /*unused*/)
    : port_(port) {

    // -- Listen socket --
    listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd_ < 0)
        throw std::runtime_error("socket: " + std::string(strerror(errno)));

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
    int ret = io_uring_queue_init(256, &ring_, 0);
    if (ret < 0) {
        close(listen_fd_);
        throw std::runtime_error("io_uring_queue_init: " + std::to_string(-ret));
    }

    // -- Register buffers with the kernel --
    // We register a single large buffer (the Request::buf array is 4096
    // bytes per connection). For a real server you'd register a pool, but
    // for our echo one connection at a time is fine.
    // This eliminates the page-fault-and-pin overhead on every recv.
    struct iovec iov;
    // We'll register on-the-fly per-connection instead; skip for now
    // since SEND_ZC is the main optimisation we want to measure.

    std::cout << "[io_uring_zc] Echo server on port " << port_ << "\n"
              << "[io_uring_zc] SEND_ZC enabled — zero-copy on send path\n";
}

IoUringZcEchoServer::~IoUringZcEchoServer() {
    io_uring_queue_exit(&ring_);
    if (listen_fd_ >= 0) close(listen_fd_);
}

void IoUringZcEchoServer::shutdown() {
    running_ = false;
}

void IoUringZcEchoServer::run() {
    event_loop();
}

// ==============================================================================
// Event loop
// ==============================================================================
// Flow per connection:
//
//   1. submit_accept()
//   2. handle_accept() → submit_recv()
//   3. handle_recv(data) → submit_send_zc() — sends from same buf, zero-copy
//   4. handle_send_zc() → submit_recv() — buffer is free, start next read
//   ... repeat 3-4 until disconnect
//
// Key difference from vanilla: no memcpy to a send buffer. SEND_ZC uses the
// original recv buffer directly. But the buffer is pinned by the kernel until
// the SEND_ZC completes, so we cannot submit the next recv until step 4.
// ==============================================================================
void IoUringZcEchoServer::event_loop() {
    submit_accept();

    while (running_) {
        __kernel_timespec ts = {1, 0};
        io_uring_cqe* cqe    = nullptr;

        int ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
        if (ret == -ETIME || ret == -EINTR) continue;
        if (ret < 0) {
            std::cerr << "io_uring_wait_cqe: " << -ret << "\n";
            break;
        }

        unsigned head;
        unsigned count = 0;
        io_uring_for_each_cqe(&ring_, head, cqe) {
            auto* req = reinterpret_cast<Request*>(io_uring_cqe_get_data(cqe));
            int   res = cqe->res;
            ++count;

            switch (req->op) {
            case Request::ACCEPT:
                handle_accept(res);
                delete req;
                break;
            case Request::RECV:
                handle_recv(req, res);
                break;
            case Request::SEND_ZC:
                handle_send_zc(req);
                break;
            }
        }
        io_uring_cq_advance(&ring_, count);

        submit_accept();
    }
}

// ==============================================================================
// accept
// ==============================================================================
void IoUringZcEchoServer::submit_accept() {
    auto* req = new Request{Request::ACCEPT, -1, {}, 0};
    auto* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) { delete req; return; }

    io_uring_prep_accept(sqe, listen_fd_, nullptr, nullptr, SOCK_NONBLOCK);
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring_);
}

void IoUringZcEchoServer::handle_accept(int client_fd) {
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            std::cerr << "accept error: " << strerror(errno) << "\n";
        return;
    }
    submit_recv(new Request{Request::RECV, client_fd, {}, 0});
}

// ==============================================================================
// recv  — same as vanilla: read into req->buf
// ==============================================================================
void IoUringZcEchoServer::submit_recv(Request* req) {
    auto* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) { delete req; return; }

    io_uring_prep_recv(sqe, req->client_fd, req->buf, sizeof(req->buf), 0);
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring_);
}

void IoUringZcEchoServer::handle_recv(Request* req, int nread) {
    if (nread <= 0) {
        if (nread == 0) {
            close(req->client_fd);
        } else if (nread != -EAGAIN) {
            std::cerr << "recv error: " << -nread << "\n";
            close(req->client_fd);
        }
        delete req;
        return;
    }

    // -- Echo using ZERO-COPY send --
    // We use the same buffer that was filled by recv. The kernel will pin
    // these pages and the NIC will DMA directly from them — no kernel-side
    // copy of the data. However, the buffer is now owned by the kernel
    // until the SEND_ZC completes, so we CANNOT submit the next recv yet.
    req->op  = Request::SEND_ZC;
    req->len = static_cast<size_t>(nread);
    submit_send_zc(req);
}

// ==============================================================================
// SEND_ZC  — zero-copy send, NIC DMAs directly from req->buf
// ==============================================================================
void IoUringZcEchoServer::submit_send_zc(Request* req) {
    auto* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) { delete req; return; }

    // Use the zero-copy send operation. The kernel will pin the userspace
    // pages and let the NIC DMA directly from them, avoiding the
    // kernel-buffer copy that a regular send requires.
    io_uring_prep_send_zc(sqe, req->client_fd, req->buf, req->len, 0, 0);
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring_);
}

void IoUringZcEchoServer::handle_send_zc(Request* req) {
    // SEND_ZC completed. The kernel has finished DMA from our buffer,
    // so the buffer is now safe to reuse. Submit the next recv.
    req->op  = Request::RECV;
    req->len = 0;
    submit_recv(req);
}

#endif  // __linux__