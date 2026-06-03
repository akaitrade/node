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
    auto lastSeenSeq      = blockchain_.getLastSeq();
    auto lastSeenRound    = cs::Conveyer::instance().currentRoundNumber();
    auto lastChangeTime   = std::chrono::steady_clock::now();
    size_t consecutiveKicks = 0;

    cslog() << "SyncWatchdog: started (interval " << checkInterval_.count()
            << "s, threshold " << stuckThreshold_.count() << "m, kick="
            << (kickEnabled_.load() ? "on" : "off")
            << ", hardResetAfter=" << hardResetAfterKicks_
            << ", minBehind=" << minBehindRounds_ << ")";

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
        const uint64_t behind = (currentRound > currentSeq) ? (currentRound - currentSeq) : 0;
        const auto now = std::chrono::steady_clock::now();
        const bool progressed = (currentSeq != lastSeenSeq);

        if (progressed) {
            lastSeenSeq      = currentSeq;
            lastChangeTime   = now;
            consecutiveKicks = 0;
        }
        lastSeenRound = currentRound;

        // Caught up (or close enough): nothing to do this tick.
        if (behind < minBehindRounds_) continue;

        // Don't act on a genuinely idle chain (no rounds advancing means
        // network is silent, not that we're stuck).
        if (!isExpectingProgress(roundsAdvanced) && !sync_.isSyncroStarted()) {
            continue;
        }

        const auto stuckFor = std::chrono::duration_cast<std::chrono::seconds>(now - lastChangeTime);

        // If we're behind but apply has made *some* progress within the last
        // stuckThreshold window, let the normal sync engine keep working.
        if (progressed && stuckFor < stuckThreshold_) continue;

        // Behind + stalled long enough → keep nudging the sync engine every
        // check interval (not only once every stuckThreshold). The kick is
        // cheap; if normal sync is already running it harmlessly re-runs.
        onStuckDetected(currentSeq, stuckFor, sync_.isSyncroStarted(),
                        blockchain_.getCachedBlocksSize(), roundsAdvanced);
        ++consecutiveKicks;

        // After N ineffective kicks, do a full reset (drop cached blocks +
        // sync state) so the next nudge starts fresh.
        if (kickEnabled_.load(std::memory_order_acquire)
            && consecutiveKicks >= hardResetAfterKicks_) {
            cswarning() << "SyncWatchdog: hard reset after " << consecutiveKicks
                        << " ineffective kicks (lastSeq=" << currentSeq
                        << " behind=" << behind << ")";
            sync_.forceResync();
            consecutiveKicks = 0;
            lastChangeTime   = now;   // give the reset a stuckThreshold window to take effect
        }
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
