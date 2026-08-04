#include "server.h"

#include <csignal>
#include <iostream>
#include <memory>

static std::unique_ptr<IoUringEchoServer> g_server;

static void signal_handler(int /*sig*/) {
    if (g_server) {
        std::cout << "\nShutting down..." << std::endl;
        g_server->shutdown();
    }
}

int main(int argc, char* argv[]) {
    uint16_t port    = 8082;   // default different from kqueue so both can coexist
    size_t   workers = 0;      // io_uring doesn't use workers, but we accept the flag

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (std::strcmp(argv[i], "--workers") == 0 && i + 1 < argc)
            workers = std::stoul(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: echo_server_iouring [--port PORT] [--workers N]\n"
                      << "  Note: --workers is accepted but unused (io_uring is single-threaded)\n";
            return 0;
        }
    }

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        g_server = std::make_unique<IoUringEchoServer>(port, workers);
        g_server->run();
    } catch (const std::exception& ex) {
        std::cerr << "Fatal: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}