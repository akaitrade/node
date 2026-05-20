#ifndef FORKTRACKER_HPP
#define FORKTRACKER_HPP

#include <cstdint>
#include <deque>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <lib/system/common.hpp>

using NodeVersion = cs::Version;

namespace cs {

struct KnownFork {
    cs::Sequence signalStartHeight;
    cs::Sequence deadlineHeight;
    NodeVersion minFullVersion;
    NodeVersion minCompatibleVersion;
    std::string_view name;
};

// Empty for now. Future forks appended here per release.
inline constexpr KnownFork KNOWN_FORKS[] = {};

struct ForkActivation {
    const KnownFork* fork;
    cs::Sequence activationHeight;
};

class ForkTracker {
public:
    static constexpr cs::Sequence kSignalWindow = 3600;
    static constexpr uint8_t kSignalThresholdPercent = 67;
    static constexpr cs::Sequence kActivationLag = 3600;

    void onBlockAccepted(cs::Sequence seq, NodeVersion writerVersion);
    std::optional<ForkActivation> pollNewActivation();
    uint8_t currentSupportPercent(const KnownFork& fork) const;

private:
    struct Entry {
        cs::Sequence seq;
        NodeVersion writerVersion;
    };

    std::deque<Entry> window_;
    std::unordered_set<const KnownFork*> lockedIn_;
    std::vector<ForkActivation> pending_;
};

}  // namespace cs

#endif  // FORKTRACKER_HPP
