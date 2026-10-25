#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace core {

// Thread pool with priorities (lower value runs first). Jobs are mostly I/O bound
// (HTTP range requests), so the pool is deliberately larger than the core count.
class JobSystem {
public:
    explicit JobSystem(unsigned threads) {
        for (unsigned i = 0; i < threads; ++i) workers_.emplace_back([this] { run(); });
    }
    ~JobSystem() {
        {
            std::lock_guard lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }
    void submit(float priority, std::function<void()> fn) {
        {
            std::lock_guard lk(m_);
            q_.push({priority, seq_++, std::move(fn)});
        }
        cv_.notify_one();
    }
    size_t pending() {
        std::lock_guard lk(m_);
        return q_.size();
    }

private:
    struct Job {
        float prio;
        uint64_t seq;
        std::function<void()> fn;
        bool operator<(const Job& o) const { return prio != o.prio ? prio > o.prio : seq > o.seq; }
    };
    void run() {
        for (;;) {
            Job j;
            {
                std::unique_lock lk(m_);
                cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
                if (stop_) return;
                j = std::move(const_cast<Job&>(q_.top()));
                q_.pop();
            }
            j.fn();
        }
    }
    std::vector<std::thread> workers_;
    std::priority_queue<Job> q_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    uint64_t seq_ = 0;
};

}  // namespace core
