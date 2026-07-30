#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace meat {

// Small worker pool for pure background work (chunk meshing, generation).
// Threading law (see ARCHITECTURE.md): workers touch only their job's own
// inputs/outputs. Results return to the main thread via post()/drainMainThread().
class JobQueue {
public:
    ~JobQueue() { stop(); }

    void start(unsigned workers);
    void stop();

    void enqueue(std::function<void()> job);     // runs on a worker
    void post(std::function<void()> fn);         // runs on next drainMainThread()
    void drainMainThread();                      // Engine calls once per frame

    bool idle() const;                           // no queued or running jobs

private:
    void workerLoop();

    std::vector<std::thread> m_workers;
    std::deque<std::function<void()>> m_jobs;
    std::deque<std::function<void()>> m_mainThread;
    mutable std::mutex m_jobsMutex;
    mutable std::mutex m_mainMutex;
    std::condition_variable m_wake;
    unsigned m_running = 0;
    bool m_stopping = false;
};

} // namespace meat
