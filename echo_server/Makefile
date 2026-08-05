CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O3 -march=native -DNDEBUG
UNAME_S  := $(shell uname -s)

# ------------------------------------------------------------------
# Default target — auto-selects server based on platform
# ------------------------------------------------------------------
ifeq ($(UNAME_S),Linux)
all: echo_server_epoll echo_server_iouring echo_server_iouring_zc bench_client
else
all: echo_server bench_client
endif

# ------------------------------------------------------------------
# kqueue server — builds on macOS
# ------------------------------------------------------------------
echo_server: common/thread_pool.cpp flavors/kqueue/main.cpp flavors/kqueue/server.cpp
	$(CXX) $(CXXFLAGS) -Icommon -Iflavors/kqueue $^ -lpthread -o $@

# ------------------------------------------------------------------
# epoll server — builds on Linux (identical architecture to kqueue)
# ------------------------------------------------------------------
echo_server_epoll: common/thread_pool.cpp flavors/epoll/main.cpp flavors/epoll/server.cpp
	$(CXX) $(CXXFLAGS) -Icommon -Iflavors/epoll $^ -lpthread -o $@

# ------------------------------------------------------------------
# io_uring server — vanilla (baseline async)
# ------------------------------------------------------------------
echo_server_iouring: flavors/iouring/main.cpp flavors/iouring/server.cpp
	$(CXX) $(CXXFLAGS) -Icommon -Iflavors/iouring $^ -lpthread -luring -o $@

# ------------------------------------------------------------------
# io_uring server — SEND_ZC (zero-copy send)
# ------------------------------------------------------------------
echo_server_iouring_zc: flavors/iouring_zc/main.cpp flavors/iouring_zc/server.cpp
	$(CXX) $(CXXFLAGS) -Icommon -Iflavors/iouring_zc $^ -lpthread -luring -o $@

# ------------------------------------------------------------------
# Benchmark client — cross-platform
# ------------------------------------------------------------------
bench_client: bench/bench_client.cpp
	$(CXX) $(CXXFLAGS) bench/bench_client.cpp -lpthread -o $@

clean:
	rm -f echo_server echo_server_epoll echo_server_iouring echo_server_iouring_zc bench_client