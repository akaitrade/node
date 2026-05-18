#include <csnode/syncwatchdog.hpp>

#include <csnode/blockchain.hpp>
#include <csnode/conveyer.hpp>
#include <csnode/eventreport.hpp>
#include <csnode/node.hpp>
#include <csnode/poolsynchronizer.hpp>

#include <lib/system/logger.hpp>

namespace cs {

SyncWatchdog::SyncWatchdog(BlockChain& blockchain, PoolSynchronizer& sync, Node& node)
: blockchain_(blockchain), sync_(sync), node_(node) {}

SyncWatchdog::~SyncWatchdog() {
    stop();
}

void SyncWatchdog::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    thread_ = std::thread(&SyncWatchdog::run, this);
}

void SyncWatchdog::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    {
        std::lock_guard<std::mutex> lock(wakeMux_);
    }
    wakeCv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool SyncWatchdog::isExpectingProgress(uint64_t roundsAdvanced) const {
    if (sync_.isSyncroStarted()) return true;
    if (blockchain_.getCachedBlocksSize() > 0) return true;
    if (roundsAdvanced > 0) return true;
    if (node_.getNodeLevel() == Node::Level::Confidant) return true;
    return false;
}

void SyncWatchdog::run() {
    auto lastSeenSeq    = blockchain_.getLastSeq();
    auto lastSeenRound  = cs::Conveyer::instance().currentRoundNumber();
    auto lastChangeTime = std::chrono::steady_clock::now();

    cslog() << "SyncWatchdog: started (interval " << checkInterval_.count()
            << "s, threshold " << stuckThreshold_.count() << "m)";

    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock(wakeMux_);
            wakeCv_.wait_for(lock, checkInterval_, [this] {
                return !running_.load(std::memory_order_acquire);
            });
        }
        if (!running_.load(std::memory_order_acquire)) break;

        const auto currentSeq   = blockchain_.getLastSeq();
        const auto currentRound = cs::Conveyer::instance().currentRoundNumber();
        const uint64_t roundsAdvanced = (currentRound > lastSeenRound)
                                      ? (currentRound - lastSeenRound) : 0;
        const auto now = std::chrono::steady_clock::now();

        if (currentSeq != lastSeenSeq) {
            lastSeenSeq    = currentSeq;
            lastSeenRound  = currentRound;
            lastChangeTime = now;
            continue;
        }

        const auto stuckFor = std::chrono::duration_cast<std::chrono::seconds>(now - lastChangeTime);
        if (stuckFor < stuckThreshold_) continue;

        if (!isExpectingProgress(roundsAdvanced)) {
            // chain genuinely idle — silent, but reset the round baseline so
            // the next fire reflects a fresh observation window
            lastSeenRound = currentRound;
            continue;
        }

        onStuckDetected(currentSeq, stuckFor, sync_.isSyncroStarted(),
                        blockchain_.getCachedBlocksSize(), roundsAdvanced);

        // throttle: require another stuckThreshold window before re-firing
        lastChangeTime = now;
        lastSeenRound  = currentRound;
    }

    cslog() << "SyncWatchdog: stopped";
}

void SyncWatchdog::onStuckDetected(cs::Sequence seq, std::chrono::seconds stuckFor,
                                   bool syncActive, size_t cached, uint64_t roundsAdvanced) {
    cswarning() << "SYNC_STUCK: lastSeq=" << seq
                << " stuck_for=" << stuckFor.count() << "s"
                << " sync_active=" << (syncActive ? 1 : 0)
                << " cached=" << cached
                << " rounds_advanced=" << roundsAdvanced
                << " level=" << static_cast<int>(node_.getNodeLevel());

    if (telemetryEnabled_.load(std::memory_order_acquire)) {
        EventReport::sendStuckDetected(node_, seq, static_cast<uint32_t>(stuckFor.count()),
                                       syncActive, static_cast<uint32_t>(cached));
    }

    if (kickEnabled_.load(std::memory_order_acquire)) {
        cslog() << "SyncWatchdog: kicking sync engine";
        node_.kickSync();
    }
}

} // namespace cs
