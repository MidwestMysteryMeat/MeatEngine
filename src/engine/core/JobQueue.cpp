#include "engine/core/JobQueue.h"
#include <algorithm>

namespace meat {

void JobQueue::start(unsigned workers) {
    stop();
    m_stopping = false;
    const unsigned n = std::clamp(workers, 2u, 4u);
    m_workers.reserve(n);
    for (unsigned i = 0; i < n; ++i) m_workers.emplace_back([this] { workerLoop(); });
}

void JobQueue::stop() {
    {
        std::lock_guard lock(m_jobsMutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    for (auto& t : m_workers)
        if (t.joinable()) t.join();
    m_workers.clear();
    m_jobs.clear();
}

void JobQueue::enqueue(std::function<void()> job) {
    {
        std::lock_guard lock(m_jobsMutex);
        m_jobs.push_back(std::move(job));
    }
    m_wake.notify_one();
}

void JobQueue::post(std::function<void()> fn) {
    std::lock_guard lock(m_mainMutex);
    m_mainThread.push_back(std::move(fn));
}

void JobQueue::drainMainThread() {
    // Swap out under the lock so posted callbacks can post follow-ups freely.
    std::deque<std::function<void()>> ready;
    {
        std::lock_guard lock(m_mainMutex);
        ready.swap(m_mainThread);
    }
    for (auto& fn : ready) fn();
}

bool JobQueue::idle() const {
    std::lock_guard lock(m_jobsMutex);
    return m_jobs.empty() && m_running == 0;
}

void JobQueue::workerLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(m_jobsMutex);
            m_wake.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
            if (m_stopping) return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
            ++m_running;
        }
        job();
        {
            std::lock_guard lock(m_jobsMutex);
            --m_running;
        }
    }
}

} // namespace meat
