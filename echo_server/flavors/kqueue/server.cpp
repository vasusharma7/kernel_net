#include "server.h"

#include <iostream>
#include <stdexcept>
#include <unistd.h>

#ifndef __APPLE__
// ==============================================================================
// Stub for non-macOS platforms
// ==============================================================================
EchoServer::EchoServer(uint16_t, size_t) : port_(0), pool_(1) {
    std::cerr << "[kqueue] Fatal: kqueue is macOS/BSD only\n";
    throw std::runtime_error("kqueue unsupported on this platform");
}
EchoServer::~EchoServer() = default;
void EchoServer::run() {}
void EchoServer::shutdown() {}

#else
// ==============================================================================
// macOS implementation using kqueue
// ==============================================================================

#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/types.h>

// ==============================================================================
// Constructor
// ==============================================================================
// 1. Creates a TCP socket (SOCK_STREAM). This is the "front door" — it only
//    accepts connections, never carries data itself.
// 2. Sets it to non-blocking so accept() never blocks.
// 3. Binds to 0.0.0.0:port so anyone can connect.
// 4. listen() tells the kernel to start accepting connections.
// 5. Creates a kqueue instance — the "notifier" that will wake us up when
//    interesting things happen on our file descriptors.
// 6. Registers the listen fd with kqueue so we're notified when new clients
//    are waiting to be accept()ed.
// ==============================================================================
EchoServer::EchoServer(uint16_t port, size_t num_workers)
    : port_(port), pool_(num_workers) {

    // -- Create the listen socket (front door) --
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        throw std::runtime_error("socket: " + std::string(strerror(errno)));
    }

    // -- Make the listen socket non-blocking --
    // Without this, accept() would block if no clients are waiting.
    int flags = fcntl(listen_fd_, F_GETFL, 0);
    fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);

    // -- Allow immediate restart on the same port --
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // -- Bind to 0.0.0.0:port --
    sockaddr_in addr{};
    addr.sin_family         = AF_INET;
    addr.sin_addr.s_addr    = INADDR_ANY;
    addr.sin_port           = htons(port_);

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd_);
        throw std::runtime_error("bind: " + std::string(strerror(errno)));
    }

    // -- Start listening --
    if (listen(listen_fd_, SOMAXCONN) < 0) {
        close(listen_fd_);
        throw std::runtime_error("listen: " + std::string(strerror(errno)));
    }

    // -- Create kqueue instance --
    // kqueue is macOS/BSD's I/O event notification system. It works like a
    // personal assistant: you tell it which file descriptors to watch, and
    // it wakes you up when something happens on them.
    kq_ = kqueue();
    if (kq_ < 0) {
        close(listen_fd_);
        throw std::runtime_error("kqueue: " + std::string(strerror(errno)));
    }

    // -- Register the listen fd with kqueue --
    // We want to be notified when the listen fd is "readable" — which for a
    // listen socket means "there are new connections waiting to be accepted."
    // EVFILT_READ:  watch for "can read" events
    // EV_ADD:       add this fd to the watch list
    // (no EV_CLEAR here = level-triggered for the listen fd, which is simpler)
    struct kevent ev;
    EV_SET(&ev, listen_fd_, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0) {
        close(listen_fd_);
        close(kq_);
        throw std::runtime_error("kevent add listen: " +
                                 std::string(strerror(errno)));
    }
}

EchoServer::~EchoServer() {
    close(kq_);
    close(listen_fd_);
}

void EchoServer::shutdown() {
    running_ = false;
    pool_.stop();
}

void EchoServer::run() {
    event_loop();
}

// ==============================================================================
// kqueue event loop  (runs on the single network thread)
// ==============================================================================
// This is the heart of the server. We sit in a loop calling kevent(), which
// blocks until something happens. When it returns, we check which file
// descriptor has activity:
//
//   listen_fd_  → new client wants to connect  → accept_connection()
//   client_fd   → client sent data             → handle_read()
//
// We use a 1-second timeout so we can also check the running_ flag regularly.
// ==============================================================================
void EchoServer::event_loop() {
    std::vector<struct kevent> events(MAX_EVENTS);
    struct timespec timeout = {1, 0};  // 1 second — lets us check running_

    while (running_) {
        // kevent() sleeps until either:
        //   a) events happen on watched fds, or
        //   b) the 1-second timeout expires
        int nfds = kevent(kq_, nullptr, 0, events.data(), MAX_EVENTS, &timeout);
        if (nfds < 0) {
            if (errno == EINTR) break;
            std::cerr << "kevent: " << strerror(errno) << "\n";
            break;
        }

        // Process each event that kevent() returned
        for (int i = 0; i < nfds; ++i) {
            int fd = static_cast<int>(events[i].ident);

            if (events[i].flags & EV_ERROR) {
                std::cerr << "kevent error on fd " << fd << "\n";
                close(fd);
                continue;
            }

            if (fd == listen_fd_) {
                // The listen socket is readable = new connections ready
                accept_connection(listen_fd_);
            } else if (events[i].filter == EVFILT_READ) {
                // A client sent data — read it and hand off to a worker
                handle_read(fd);
            }
        }
    }
}

// ==============================================================================
// Accept all new connections
// ==============================================================================
// Called when kevent tells us the listen fd has incoming connections.
//
// accept() creates a NEW file descriptor for each client. The listen fd
// never changes — it stays open forever to accept() more clients.
//
// We use a while(true) loop to drain ALL pending connections at once.
// This is needed because EV_CLEAR (edge-triggered) means kevent won't tell
// us again until NEW clients arrive — if we only accept() 1 out of 5,
// the remaining 4 would be forgotten.
//
// When no more clients are pending, accept() returns -1 with errno=EAGAIN
// (meaning "try again later — nothing here right now"), and we break out.
// ==============================================================================
void EchoServer::accept_connection(int listen_fd) {
    sockaddr_in client_addr{};
    socklen_t   addrlen = sizeof(client_addr);

    while (true) {
        // accept() takes a waiting client off the kernel's queue and gives
        // us a BRAND NEW file descriptor for talking to this one client.
        // listen_fd (the front door) stays open.
        int client_fd = accept(listen_fd,
                                reinterpret_cast<sockaddr*>(&client_addr),
                                &addrlen);
        if (client_fd < 0) {
            // EAGAIN/EWOULDBLOCK = "no more clients right now"
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            std::cerr << "accept: " << strerror(errno) << "\n";
            break;
        }

        // -- Set the client socket to non-blocking --
        // read() should never block — if there's no data, we want EAGAIN,
        // not a hang.
        int fl = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, fl | O_NONBLOCK);

        // -- Register the client fd with kqueue --
        // We want to be woken up when this client sends data.
        // EV_CLEAR = edge-triggered: we MUST read all available data in a
        // loop until EAGAIN, because we won't get another notification
        // until more data arrives.
        struct kevent ev;
        EV_SET(&ev, client_fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
        if (kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0) {
            std::cerr << "kevent add client: " << strerror(errno) << "\n";
            close(client_fd);
        }
    }
}

// ==============================================================================
// Read data from a client fd, then hand off to a worker thread
// ==============================================================================
// Called when kevent tells us a client fd has data to read.
//
// The network thread:
//   1. Reads the data from the socket into a buffer
//   2. Pushes the buffer as a task onto the ThreadPool's queue
//   3. Goes back to the event loop immediately
//
// A worker thread will later pop the task and write the echo reply.
// This separation means the network thread never blocks on write().
// ==============================================================================
void EchoServer::handle_read(int client_fd) {
    std::vector<char> buf(BUF_SIZE);
    ssize_t n = read(client_fd, buf.data(), buf.size());

    if (n <= 0) {
        if (n == 0) {
            // n == 0 means the client closed the connection cleanly.
            // IMPORTANT: the network thread must NOT close the fd here.
            // A worker may still be writing an echo to this fd. Instead we
            // deregister from kqueue (so kevent won't fire again for this
            // fd) and enqueue a close task — the worker owns the fd lifecycle.
            struct kevent ev;
            EV_SET(&ev, client_fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
            kevent(kq_, &ev, 1, nullptr, 0, nullptr);
            pool_.enqueue([client_fd]() { close(client_fd); });
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            // A real error — not just "no data right now"
            std::cerr << "read: " << strerror(errno) << "\n";
            struct kevent ev;
            EV_SET(&ev, client_fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
            kevent(kq_, &ev, 1, nullptr, 0, nullptr);
            pool_.enqueue([client_fd]() { close(client_fd); });
        }
        // Note: if errno == EAGAIN, it means we read all available data.
        // This can happen with edge-triggered mode. We just return; kqueue
        // will notify us when more data arrives.
        return;
    }

    // Shrink the buffer to the actual number of bytes read
    buf.resize(static_cast<size_t>(n));

    // -- Hand off to a worker thread via the shared queue --
    // The lambda captures:
    //   client_fd — which socket to write back to
    //   buf       — the data (moved in, not copied)
    // The worker thread will run this lambda and echo the data back.
    pool_.enqueue([client_fd, buf = std::move(buf)]() {
        // Echo the data back on the worker thread
        // write() may not send all bytes in one call, so we loop.
        size_t total_written = 0;
        while (total_written < buf.size()) {
            ssize_t nw = write(client_fd,
                               buf.data() + total_written,
                               buf.size() - total_written);
            if (nw < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Socket send buffer full — retry (rare for small payloads)
                    continue;
                }
                // Connection reset or error — give up
                break;
            }
            total_written += static_cast<size_t>(nw);
        }
    });
}

#endif  // __APPLE__