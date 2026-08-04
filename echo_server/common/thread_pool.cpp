#include "thread_pool.h"

// ==============================================================================
// Constructor: spawn worker threads
// ==============================================================================
// Creates num_workers real OS threads. Each thread immediately starts running
// worker_loop(). The `this` pointer tells each thread which ThreadPool object
// it belongs to (so it can access the queue, mutex, etc.).
//
// After this, we have (num_workers + 1) threads total:
//   - 1 main thread (runs the kqueue event loop)
//   - num_workers worker threads (sit in worker_loop() waiting for tasks)
// ==============================================================================
ThreadPool::ThreadPool(size_t num_workers) {
    workers_.reserve(num_workers);  // pre-allocate space for efficiency
    for (size_t i = 0; i < num_workers; ++i) {
        workers_.emplace_back(&ThreadPool::worker_loop, this);
    }
}

ThreadPool::~ThreadPool() {
    stop();
}

// ==============================================================================
// stop(): graceful shutdown
// ==============================================================================
// 1. Set running_ = false (atomic — visible to all workers immediately)
// 2. Wake ALL sleeping workers (notify_all, not notify_one)
// 3. Wait for each worker thread to finish (join)
//
// Workers check !running_ in their predicate, wake up, see the queue is
// empty and running_ is false, and exit.
// ==============================================================================
void ThreadPool::stop() {
    running_ = false;
    cv_.notify_all();  // wake every worker so they can exit
    for (auto& t : workers_) {
        if (t.joinable()) t.join();  // wait for thread to finish
    }
}

// ==============================================================================
// enqueue(): producer side (called by network thread)
// ==============================================================================
// 1. Lock the mutex (prevents workers from popping while we push)
// 2. Push the task onto the shared queue
// 3. Mutex unlocks automatically (lock_guard goes out of scope)
// 4. Wake up ONE sleeping worker (cv_.notify_one)
//
// The task is now safely in the queue. If all workers are busy, the
// notification is lost — that's fine, the task will be picked up when a
// worker finishes its current work and loops back to check the queue.
// ==============================================================================
void ThreadPool::enqueue(Task task) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        tasks_.push(std::move(task));
    }  // mutex released here
    cv_.notify_one();
}

// ==============================================================================
// worker_loop(): consumer side (runs on each worker thread)
// ==============================================================================
// This is the "waiter in a restaurant" loop. Each worker:
//
//   while (restaurant is open) {
//     grab the order-spindle key (mutex)
//     if no orders and restaurant is still open → nap (cv_.wait)
//     if closing up and no more orders → clock out (return)
//     grab the first order off the queue
//     release the key (mutex)
//     cook the order (execute task)
//     loop back
//   }
//
// Critical detail: task() runs OUTSIDE the lock. This means multiple workers
// can execute tasks in parallel without blocking each other. The mutex is
// only held during the brief push/pop operations.
// ==============================================================================
void ThreadPool::worker_loop() {
    while (running_) {
        Task task;
        {
            // Acquire the mutex (only one worker at a time past here)
            std::unique_lock<std::mutex> lock(mtx_);

            // cv_.wait(lock, predicate):
            //   If the predicate is false (queue empty AND still running),
            //   release the mutex and put this thread to sleep.
            //   When woken (by notify_one/notify_all), re-acquire the mutex
            //   and re-check the predicate. If true, return.
            cv_.wait(lock, [this]() {
                return !tasks_.empty() || !running_;
                // wake up if: there's a task to do, OR the pool is shutting down
            });

            // If we're shutting down and the queue is empty, exit the thread.
            if (!running_ && tasks_.empty()) return;

            // Dequeue the task (steal it without copying)
            task = std::move(tasks_.front());
            tasks_.pop();
        }  // mutex released here — IMPORTANT!

        // Execute the task OUTSIDE the lock. The mutex is free for other
        // threads to push/pop while this worker is busy writing the echo.
        task();
    }
}