#include <csnode/forktracker.hpp>

#include <algorithm>
#include <cstddef>

namespace cs {

void ForkTracker::onBlockAccepted(cs::Sequence seq, NodeVersion writerVersion) {
    window_.push_back({seq, writerVersion});

    // evict entries that have scrolled out of the sliding window
    while (window_.size() > kSignalWindow) {
        window_.pop_front();
    }

    for (const KnownFork& fork : KNOWN_FORKS) {
        if (lockedIn_.count(&fork)) {
            continue;
        }
        if (seq < fork.signalStartHeight || seq >= fork.deadlineHeight) {
            continue;
        }

        std::size_t supporters = 0;
        for (const Entry& e : window_) {
            if (e.writerVersion >= fork.minFullVersion) {
                ++supporters;
            }
        }

        if (supporters * 100 / window_.size() >= kSignalThresholdPercent) {
            lockedIn_.insert(&fork);
            pending_.push_back({&fork, seq + kActivationLag});
        }
    }
}

std::optional<ForkActivation> ForkTracker::pollNewActivation() {
    if (pending_.empty()) {
        return std::nullopt;
    }
    ForkActivation result = pending_.front();
    pending_.erase(pending_.begin());
    return result;
}

uint8_t ForkTracker::currentSupportPercent(const KnownFork& fork) const {
    if (window_.empty()) {
        return 0;
    }
    std::size_t supporters = 0;
    for (const Entry& e : window_) {
        if (e.writerVersion >= fork.minFullVersion) {
            ++supporters;
        }
    }
    // saturate at 100 for safety (should never exceed it)
    std::size_t pct = supporters * 100 / window_.size();
    return static_cast<uint8_t>(pct > 100 ? 100 : pct);
}

}  // namespace cs
