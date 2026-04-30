#ifndef CSNODE_VERIFIER_POOL_HPP
#define CSNODE_VERIFIER_POOL_HPP

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace cs {

// Small fixed-size thread pool. Submit returns std::future.
class VerifierPool {
public:
    explicit VerifierPool(size_t n_workers) {
        if (n_workers == 0) n_workers = 1;
        workers_.reserve(n_workers);
        for (size_t i = 0; i < n_workers; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    }

    ~VerifierPool() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    VerifierPool(const VerifierPool&) = delete;
    VerifierPool& operator=(const VerifierPool&) = delete;

    template <typename F>
    auto submit(F&& f) -> std::future<decltype(f())> {
        using R = decltype(f());
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        std::future<R> fut = task->get_future();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    size_t size() const { return workers_.size(); }

private:
    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

}  // namespace cs

#endif
