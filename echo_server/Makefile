CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O3 -march=native -DNDEBUG

all: echo_server bench_client

echo_server: common/thread_pool.cpp flavors/kqueue/main.cpp flavors/kqueue/server.cpp
	$(CXX) $(CXXFLAGS) -Icommon -Iflavors/kqueue $^ -lpthread -o $@

bench_client: bench/bench_client.cpp
	$(CXX) $(CXXFLAGS) bench/bench_client.cpp -lpthread -o $@

clean:
	rm -f echo_server bench_client