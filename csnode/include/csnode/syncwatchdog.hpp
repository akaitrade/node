#ifndef CSNODE_SYNCWATCHDOG_HPP
#define CSNODE_SYNCWATCHDOG_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <lib/system/common.hpp>

class BlockChain;
class Node;

namespace cs {
class PoolSynchronizer;

// Background watchdog that detects apply-side stalls (executor hang, disk
// freeze, deadlock in BlockChain) via the primary signal: getLastSeq()
// unchanged for stuck_threshold while progress is expected. Logs + emits
// EventReport on detection. Optional kick re-issues sync requests.
class SyncWatchdog {
public:
    SyncWatchdog(BlockChain& blockchain, PoolSynchronizer& sync, Node& node);
    ~SyncWatchdog();

    SyncWatchdog(const SyncWatchdog&)            = delete;
    SyncWatchdog& operator=(const SyncWatchdog&) = delete;

    void start();
    void stop();

    void setCheckInterval(std::chrono::seconds v)   { checkInterval_ = v; }
    void setStuckThreshold(std::chrono::minutes v)  { stuckThreshold_ = v; }
    void setKickEnabled(bool v)                     { kickEnabled_.store(v, std::memory_order_release); }
    void setTelemetryEnabled(bool v)                { telemetryEnabled_.store(v, std::memory_order_release); }

private:
    void run();
    bool isExpectingProgress(uint64_t roundsAdvanced) const;
    void onStuckDetected(cs::Sequence seq, std::chrono::seconds stuckFor,
                         bool syncActive, size_t cached, uint64_t roundsAdvanced);

    BlockChain&       blockchain_;
    PoolSynchronizer& sync_;
    Node&             node_;

    std::thread             thread_;
    std::atomic<bool>       running_{false};
    std::atomic<bool>       kickEnabled_{false};
    std::atomic<bool>       telemetryEnabled_{true};
    std::chrono::seconds    checkInterval_{60};
    std::chrono::minutes    stuckThreshold_{10};

    std::mutex              wakeMux_;
    std::condition_variable wakeCv_;
};

} // namespace cs

#endif // CSNODE_SYNCWATCHDOG_HPP
