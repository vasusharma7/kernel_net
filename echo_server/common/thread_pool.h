#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// ==============================================================================
// ThreadPool — a workqueue for the producer-consumer pattern
// ==============================================================================
// The network thread (producer) pushes tasks into the queue. Worker threads
// (consumers) pull tasks out and execute them. The queue is protected by a
// mutex, and workers sleep on a condition variable when the queue is empty.
//
// Data structures:
//   tasks_   | std::queue<Task>   | The shared FIFO of pending work
//   mtx_     | std::mutex         | Only one thread touches the queue at a time
//   cv_      | std::condition_variable | Workers sleep here when queue is empty
//   workers_ | std::vector<thread>| One OS thread per worker, all in worker_loop()
//   running_ | std::atomic<bool>  | Flag: false → workers exit their loop
// ==============================================================================

class ThreadPool {
public:
    // A Task is anything callable: function, lambda, functor. void() = takes
    // no arguments and returns nothing.
    using Task = std::function<void()>;

    explicit ThreadPool(size_t num_workers);
    ~ThreadPool();

    // -- producer interface --
    // Called by the network thread to hand work to the pool.
    // Thread-safe: acquires the mutex, pushes the task, notifies one worker.
    void enqueue(Task task);

    // -- shutdown --
    // Sets running_=false, wakes all workers, joins their threads.
    void stop();

private:
    // The function every worker thread runs. Pulls tasks from the queue
    // and executes them in a loop. Sleeps when queue is empty.
    void worker_loop();

    std::queue<Task> tasks_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{true};
};