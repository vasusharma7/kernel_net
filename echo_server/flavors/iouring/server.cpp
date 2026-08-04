// ==============================================================================
// io_uring echo server — full implementation (Linux) + stub (other platforms)
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
// Flow per connection:
//
//   1. submit_accept()          — ask kernel to accept the next client
//   2. handle_accept(new_fd)    — client arrived, submit recv for it
//   3. handle_recv(data)        — got data, submit send to echo it back
//   4. handle_send()            — send done, submit recv again for more data
//   ... repeat 3-4 until client disconnects
//   5. handle_recv(0)           — client closed, done
//
// All steps run on the same thread. No locks needed.
// ==============================================================================
void IoUringEchoServer::event_loop() {
    constexpr int QUEUE_DEPTH = 256;

    // Prime the first accept
    submit_accept();

    while (running_) {
        // Wait for completion (1s timeout for shutdown check)
        __kernel_timespec ts = {1, 0};
        io_uring_cqe* cqe    = nullptr;

        int ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
        if (ret == -ETIME || ret == -EINTR) continue;
        if (ret < 0) {
            std::cerr << "io_uring_wait_cqe: " << -ret << "\n";
            break;
        }

        // Process all available completions
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
            case Request::SEND:
                handle_send(req);
                break;
            }
        }
        io_uring_cq_advance(&ring_, count);

        // Keep the accept pipeline primed
        submit_accept();
    }
}

// ==============================================================================
// submit_accept  — ask the kernel to accept the next connection
// handle_accept  — got a new client, start reading from it
// ==============================================================================
void IoUringEchoServer::submit_accept() {
    auto* req   = new Request{Request::ACCEPT, -1, {}, 0};
    auto* sqe   = io_uring_get_sqe(&ring_);
    if (!sqe) { delete req; return; }

    io_uring_prep_accept(sqe, listen_fd_, nullptr, nullptr, SOCK_NONBLOCK);
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring_);
}

void IoUringEchoServer::handle_accept(int client_fd) {
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            std::cerr << "accept error: " << strerror(errno) << "\n";
        return;
    }
    // Start reading from this client
    submit_recv(new Request{Request::RECV, client_fd, {}, 0});
}

// ==============================================================================
// submit_recv  — submit an async read from a client fd
// handle_recv  — data arrived, echo it back or close on disconnect
// ==============================================================================
void IoUringEchoServer::submit_recv(Request* req) {
    auto* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) { delete req; return; }

    io_uring_prep_recv(sqe, req->client_fd, req->buf, sizeof(req->buf), 0);
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring_);
}

void IoUringEchoServer::handle_recv(Request* req, int nread) {
    if (nread <= 0) {
        if (nread == 0) {
            close(req->client_fd);   // clean close
        } else if (nread != -EAGAIN) {
            std::cerr << "recv error: " << -nread << "\n";
            close(req->client_fd);
        }
        delete req;
        return;
    }

    // Echo it back
    req->op  = Request::SEND;
    req->len = static_cast<size_t>(nread);
    submit_send(req);
}

// ==============================================================================
// submit_send  — submit an async write to echo data back
// handle_send  — send completed, go back to reading from this client
// ==============================================================================
void IoUringEchoServer::submit_send(Request* req) {
    auto* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) { delete req; return; }

    io_uring_prep_send(sqe, req->client_fd, req->buf, req->len, 0);
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring_);
}

void IoUringEchoServer::handle_send(Request* req) {
    // Send completed — now wait for the next message
    req->op  = Request::RECV;
    req->len = 0;
    submit_recv(req);
}

#endif  // __linux__