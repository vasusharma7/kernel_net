#include "server.h"

#include <csignal>
#include <iostream>
#include <memory>
#include <cstring>

static std::unique_ptr<EchoServer> g_server;

// -----------------------------------------------------------------------
// Handle Ctrl-C / SIGTERM gracefully
// -----------------------------------------------------------------------
static void signal_handler(int /*sig*/) {
    if (g_server) {
        std::cout << "\nShutting down..." << std::endl;
        g_server->shutdown();
    }
}

// -----------------------------------------------------------------------
// Usage: echo_server [--port PORT] [--workers N]
// -----------------------------------------------------------------------
int main(int argc, char* argv[]) {
    uint16_t port    = 8080;
    size_t   workers = 1;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            workers = std::stoul(argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: echo_server [--port PORT] [--workers N]\n";
            return 0;
        }
    }

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        g_server = std::make_unique<EchoServer>(port, workers);
        std::cout << "[kqueue] Echo server on port " << port
                  << " with " << workers << " worker(s)" << std::endl;
        g_server->run();
    } catch (const std::exception& ex) {
        std::cerr << "Fatal: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}