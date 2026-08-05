#include "server.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>

// ==============================================================================
// Constructor
// ==============================================================================
// 1. Creates a TCP socket (SOCK_STREAM). This is the "front door" — it only
//    accepts connections, never carries data itself.
// 2. Sets it to non-blocking so accept() never blocks.
// 3. Binds to 0.0.0.0:port so anyone can connect.
// 4. listen() tells the kernel to start accepting connections.
// 5. Creates an epoll instance — the Linux equivalent of kqueue.
// 6. Registers the listen fd with epoll so we're notified when new clients
//    are waiting to be accept()ed.
// ==============================================================================
EchoServer::EchoServer(uint16_t port, size_t num_workers)
    : port_(port), pool_(num_workers) {

    // -- Create the listen socket --
    listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd_ < 0) {
        throw std::runtime_error("socket: " + std::string(strerror(errno)));
    }

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family         = AF_INET;
    addr.sin_addr.s_addr    = INADDR_ANY;
    addr.sin_port           = htons(port_);

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd_);
        throw std::runtime_error("bind: " + std::string(strerror(errno)));
    }

    if (listen(listen_fd_, SOMAXCONN) < 0) {
        close(listen_fd_);
        throw std::runtime_error("listen: " + std::string(strerror(errno)));
    }

    // -- Create epoll instance --
    // epoll is Linux's I/O event notification system. Like kqueue, you tell
    // it which file descriptors to watch, and it wakes you up when something
    // happens on them.
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        close(listen_fd_);
        throw std::runtime_error("epoll_create1: " + std::string(strerror(errno)));
    }

    // -- Register the listen fd with epoll --
    // We want to be notified when the listen fd is "readable" — which for a
    // listen socket means "there are new connections waiting to be accepted."
    epoll_event ev{};
    ev.events   = EPOLLIN;
    ev.data.fd  = listen_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) {
        close(listen_fd_);
        close(epoll_fd_);
        throw std::runtime_error("epoll_ctl add listen: " +
                                 std::string(strerror(errno)));
    }
}

EchoServer::~EchoServer() {
    close(epoll_fd_);
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
// epoll event loop  (runs on the single network thread)
// ==============================================================================
// This is the heart of the server. We sit in a loop calling epoll_wait(),
// which blocks until something happens. When it returns, we check which file
// descriptor has activity:
//
//   listen_fd_  → new client wants to connect  → accept_connection()
//   client_fd   → client sent data             → handle_read()
//
// We use a 1-second timeout so we can also check the running_ flag regularly.
// ==============================================================================
void EchoServer::event_loop() {
    std::vector<epoll_event> events(MAX_EVENTS);

    while (running_) {
        // epoll_wait() sleeps until either:
        //   a) events happen on watched fds, or
        //   b) the 1-second timeout expires
        int nfds = epoll_wait(epoll_fd_, events.data(), MAX_EVENTS, 1000);
        if (nfds < 0) {
            if (errno == EINTR) break;
            std::cerr << "epoll_wait: " << strerror(errno) << "\n";
            break;
        }

        // Process each event that epoll_wait() returned
        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;

            if (events[i].events & EPOLLERR) {
                std::cerr << "epoll error on fd " << fd << "\n";
                close(fd);
                continue;
            }

            if (fd == listen_fd_) {
                // The listen socket is readable = new connections ready
                accept_connection(listen_fd_);
            } else if (events[i].events & EPOLLIN) {
                // A client sent data — read it and hand off to a worker
                handle_read(fd);
            }
        }
    }
}

// ==============================================================================
// Accept all new connections
// ==============================================================================
// Called when epoll tells us the listen fd has incoming connections.
//
// accept() creates a NEW file descriptor for each client. The listen fd
// never changes — it stays open forever to accept() more clients.
//
// We use a while(true) loop to drain ALL pending connections at once.
// This is needed because EPOLLET (edge-triggered) means epoll won't tell
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
        int client_fd = accept4(listen_fd,
                                reinterpret_cast<sockaddr*>(&client_addr),
                                &addrlen, SOCK_NONBLOCK);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            std::cerr << "accept: " << strerror(errno) << "\n";
            break;
        }

        // Register with epoll — edge-triggered
        // EPOLLET = edge-triggered: we MUST read all available data in a
        // loop until EAGAIN, because we won't get another notification
        // until more data arrives.
        epoll_event ev{};
        ev.events   = EPOLLIN | EPOLLET;
        ev.data.fd  = client_fd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            std::cerr << "epoll_ctl add client: " << strerror(errno) << "\n";
            close(client_fd);
        }
    }
}

// ==============================================================================
// Read data from a client fd, then hand off to a worker thread
// ==============================================================================
// Called when epoll tells us a client fd has data to read.
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
            // remove it from epoll and enqueue a close task — the worker
            // owns the fd lifecycle.
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr);
            pool_.enqueue([client_fd]() { close(client_fd); });
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            // A real error — not just "no data right now"
            std::cerr << "read: " << strerror(errno) << "\n";
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr);
            pool_.enqueue([client_fd]() { close(client_fd); });
        }
        // Note: if errno == EAGAIN, it means we read all available data.
        // This can happen with edge-triggered mode. We just return; epoll
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