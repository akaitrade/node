// dump_consensus_state: read-only BerkeleyDB chain walker that replays every
// admin "special" transaction up to Consensus::H_activate_decentralization and
// prints the final value of every Consensus::* field that processSpecialInfo
// can mutate (orders 9, 11, 21-31, 35, 37).
//
// Output format: one field per line, ready to paste-compare against
// solver/src/consensus.cpp:
//
//   uint64_t StartingDPOS = 10000;
//   csdb::Amount MinStakeValue = csdb::Amount{50000, 0};
//   ...
//
// Usage:
//   dump_consensus_state /path/to/bdb_dir [--bdb-seq-bits N]
//
// The tool opens the database read-only, iterates every block in sequence
// order, and applies exactly the same opcode dispatch as Node::processSpecialInfo.
// It does NOT write anything, does NOT connect to peers, does NOT construct
// BlockChain or Node objects.

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include <csdb/amount.hpp>
#include <csdb/database_berkeleydb.hpp>
#include <csdb/pool.hpp>
// csdb/storage.hpp not needed — we use DatabaseBerkeleyDB directly.

#include <csnode/idatastream.hpp>   // cs::IDataStream
#include <csnode/nodecore.hpp>      // cs::trx_uf::sp::managing

#include <solver/consensus.hpp>     // Consensus::*

namespace fs = boost::filesystem;

// ---------------------------------------------------------------------------
// Minimal inline replica of processSpecialInfo opcode dispatch.
// Keeps this tool self-contained — no production code modified.
// ---------------------------------------------------------------------------

struct PendingMiningSettings {
    bool pending = false;
    uint64_t applyAtRound = ULLONG_MAX;
    bool stakingOn = false;
    bool miningOn = false;
    csdb::Amount blockReward;
    csdb::Amount miningCoefficient;
};

static void apply_special_transaction(const csdb::Transaction& tx,
                                      cs::Sequence blockSeq,
                                      PendingMiningSettings& pending)
{
    if (!tx.user_field(cs::trx_uf::sp::managing).is_valid()) {
        return;
    }

    auto stringBytes = tx.user_field(cs::trx_uf::sp::managing).value<std::string>();
    std::vector<cs::Byte> msg(stringBytes.begin(), stringBytes.end());
    cs::IDataStream stream(msg.data(), msg.size());

    uint16_t order = 0;
    stream >> order;

    if (!stream.isValid()) {
        return;
    }

    switch (order) {
    case 9U: {
        cs::Sequence val = 0;
        stream >> val;
        if (stream.isValid()) {
            Consensus::StartingDPOS = val;
        }
        break;
    }
    case 11U: {
        unsigned int val = 0;
        stream >> val;
        if (stream.isValid()) {
            Consensus::MaxRoundsExecuteContract = val;
        }
        break;
    }
    case 21U: {
        uint64_t val = 0;
        stream >> val;
        if (stream.isValid()) {
            Consensus::StageOneMaximumSize = val;
        }
        break;
    }
    case 22U: {
        int32_t integral = 0;
        uint64_t fraction = 0;
        stream >> integral >> fraction;
        if (stream.isValid()) {
            Consensus::MinStakeValue = csdb::Amount(integral, fraction);
        }
        break;
    }
    case 23U: {
        uint32_t val = 0;
        stream >> val;
        if (stream.isValid()) {
            Consensus::TimeMinStage1 = val;
        }
        break;
    }
    case 24U: {
        uint32_t val = 0;
        stream >> val;
        if (stream.isValid()) {
            Consensus::GrayListPunishment = val;
        }
        break;
    }
    case 25U: {
        uint64_t val = 0;
        stream >> val;
        if (stream.isValid()) {
            Consensus::MaxStageOneHashes = static_cast<size_t>(val);
        }
        break;
    }
    case 26U: {
        uint64_t val = 0;
        stream >> val;
        if (stream.isValid()) {
            Consensus::MaxTransactionSize = static_cast<size_t>(val);
        }
        break;
    }
    case 27U: {
        uint64_t val = 0;
        stream >> val;
        if (stream.isValid()) {
            Consensus::MaxStageOneTransactions = static_cast<size_t>(val);
        }
        break;
    }
    case 28U: {
        uint64_t val = 0;
        stream >> val;
        if (stream.isValid()) {
            Consensus::MaxPreliminaryBlockSize = static_cast<size_t>(val);
        }
        break;
    }
    case 29U: {
        uint64_t val = 0;
        stream >> val;
        if (stream.isValid()) {
            Consensus::MaxPacketsPerRound = static_cast<size_t>(val);
        }
        break;
    }
    case 30U: {
        uint64_t val = 0;
        stream >> val;
        if (stream.isValid()) {
            Consensus::MaxPacketTransactions = static_cast<size_t>(val);
        }
        break;
    }
    case 31U: {
        uint64_t val = 0;
        stream >> val;
        if (stream.isValid()) {
            Consensus::MaxQueueSize = static_cast<size_t>(val);
        }
        break;
    }
    case 35U: {
        uint64_t val = 0;
        stream >> val;
        if (stream.isValid()) {
            Consensus::syncroChangeRound = val;
        }
        break;
    }
    case 37U: {
        uint8_t sign = 0;
        uint64_t round = 0;
        int32_t rewInt = 0;
        int32_t coeffInt = 0;
        uint64_t rewFrac = 0;
        uint64_t coefFrac = 0;
        stream >> sign >> round >> rewInt >> rewFrac >> coeffInt >> coefFrac;
        if (!stream.isValid()) {
            break;
        }
        // Mirror the node's two-phase commit: settings take effect at `round`.
        if (round == 0ULL || round == ULLONG_MAX) {
            pending.applyAtRound = ULLONG_MAX;
        } else {
            pending.applyAtRound = round;
        }
        pending.pending = true;
        pending.stakingOn = (sign == 3 || sign == 2);
        pending.miningOn  = (sign == 3 || sign == 1);
        pending.blockReward = csdb::Amount(rewInt, rewFrac);
        pending.miningCoefficient = csdb::Amount(coeffInt, coefFrac);
        break;
    }
    default:
        break;
    }
    // Note: the deferred apply for order-37 settings (mirrors checkConsensusSettings)
    // is performed once per block in the main loop, not per-transaction, to match
    // the node's sequencing exactly.
}

// ---------------------------------------------------------------------------

static void print_amount(const char* type_name, const char* field_name,
                         const csdb::Amount& v)
{
    // Emit as csdb::Amount{integral, fraction} — unambiguous, pasteable into
    // consensus.cpp as an initialiser.
    std::printf("%s %s = csdb::Amount{%d, %lluU};\n",
                type_name, field_name,
                static_cast<int>(v.integral()),
                static_cast<unsigned long long>(v.fraction()));
}

static void print_results()
{
    std::puts("// --- dump_consensus_state output ---");
    std::printf("uint64_t  StartingDPOS             = %lluULL;\n",
                static_cast<unsigned long long>(Consensus::StartingDPOS));
    std::printf("unsigned int MaxRoundsExecuteContract = %u;\n",
                Consensus::MaxRoundsExecuteContract);
    std::printf("uint64_t  StageOneMaximumSize      = %lluULL;\n",
                static_cast<unsigned long long>(Consensus::StageOneMaximumSize));
    print_amount("csdb::Amount", "MinStakeValue", Consensus::MinStakeValue);
    std::printf("uint32_t  TimeMinStage1            = %u;\n",
                Consensus::TimeMinStage1);
    std::printf("uint32_t  GrayListPunishment       = %u;\n",
                Consensus::GrayListPunishment);
    std::printf("size_t    MaxStageOneHashes        = %zu;\n",
                Consensus::MaxStageOneHashes);
    std::printf("size_t    MaxTransactionSize       = %zu;\n",
                Consensus::MaxTransactionSize);
    std::printf("size_t    MaxStageOneTransactions  = %zu;\n",
                Consensus::MaxStageOneTransactions);
    std::printf("size_t    MaxPreliminaryBlockSize  = %zu;\n",
                Consensus::MaxPreliminaryBlockSize);
    std::printf("size_t    MaxPacketsPerRound       = %zu;\n",
                Consensus::MaxPacketsPerRound);
    std::printf("size_t    MaxPacketTransactions    = %zu;\n",
                Consensus::MaxPacketTransactions);
    std::printf("size_t    MaxQueueSize             = %zu;\n",
                Consensus::MaxQueueSize);

    // syncroChangeRound: ULLONG_MAX is the "not scheduled" sentinel.
    if (Consensus::syncroChangeRound == ULLONG_MAX) {
        std::printf("cs::RoundNumber syncroChangeRound  = ULLONG_MAX; // (not scheduled)\n");
    } else {
        std::printf("cs::RoundNumber syncroChangeRound  = %lluULL;\n",
                    static_cast<unsigned long long>(Consensus::syncroChangeRound));
    }

    // Mining / staking (order 37)
    std::printf("bool      stakingOn                = %s;\n",
                Consensus::stakingOn ? "true" : "false");
    std::printf("bool      miningOn                 = %s;\n",
                Consensus::miningOn ? "true" : "false");
    print_amount("csdb::Amount", "blockReward", Consensus::blockReward);
    print_amount("csdb::Amount", "miningCoefficient", Consensus::miningCoefficient);
}

// ---------------------------------------------------------------------------

static void print_usage(const char* argv0)
{
    std::fprintf(stderr,
        "Usage: %s /path/to/bdb_dir [--bdb-seq-bits N]\n"
        "\n"
        "Walks the BerkeleyDB chain read-only, replays every admin\n"
        "special-transaction opcode (orders 9,11,21-31,35,37) up to\n"
        "Consensus::H_activate_decentralization, and prints the resulting\n"
        "Consensus::* field values for paste-comparison against\n"
        "solver/src/consensus.cpp.\n"
        "\n"
        "  --bdb-seq-bits N   BDB sequence-key width in bits (default: 32).\n"
        "                     Pass 64 if the database was created with 64-bit keys.\n",
        argv0);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string db_path;
    // BerkeleyDB open options: the project default key size is 32-bit sequence.
    // Pass --bdb-seq-bits 64 if the db was opened with 64-bit seq keys.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (std::strcmp(argv[i], "--bdb-seq-bits") == 0) {
            ++i; // consume next arg; ignored for now, kept for future use
            continue;
        }
        db_path = argv[i];
    }

    if (db_path.empty()) {
        std::fprintf(stderr, "error: no database path supplied\n");
        print_usage(argv[0]);
        return 1;
    }

    if (!fs::exists(db_path) || !fs::is_directory(db_path)) {
        std::fprintf(stderr, "error: '%s' is not an existing directory\n", db_path.c_str());
        return 1;
    }

    // Open the database via DatabaseBerkeleyDB.
    // The API does not expose a read-only flag; we compensate by never calling
    // put / write_batch / remove — only new_iterator() is used below.
    auto db = std::make_shared<csdb::DatabaseBerkeleyDB>();

    if (!db->open(db_path)) {
        std::fprintf(stderr, "error: failed to open BerkeleyDB at '%s': %s\n",
                     db_path.c_str(), db->last_error_message().c_str());
        return 1;
    }

    // Iterate every block in sequence order.
    auto it = db->new_iterator();
    if (!it) {
        std::fprintf(stderr, "error: could not create DB iterator\n");
        return 1;
    }

    const uint64_t cutoff = Consensus::H_activate_decentralization;

    PendingMiningSettings pending;
    uint64_t blocks_walked = 0;
    uint64_t special_tx_seen = 0;

    it->seek_to_first();
    while (it->is_valid()) {
        const cs::Sequence seq = static_cast<cs::Sequence>(it->key());

        // Stop at the decentralization activation height.
        if (seq >= cutoff) {
            break;
        }

        cs::Bytes raw = it->value();
        csdb::Pool pool = csdb::Pool::from_binary(std::move(raw));

        if (pool.is_valid()) {
            for (const auto& tx : pool.transactions()) {
                if (tx.user_field(cs::trx_uf::sp::managing).is_valid()) {
                    ++special_tx_seen;
                    apply_special_transaction(tx, seq, pending);
                }
            }
            // Check whether a deferred mining-settings change fires on this round.
            if (pending.pending && seq == pending.applyAtRound) {
                Consensus::stakingOn = pending.stakingOn;
                Consensus::miningOn  = pending.miningOn;
                Consensus::blockReward = pending.blockReward;
                Consensus::miningCoefficient = pending.miningCoefficient;
                pending.pending = false;
                pending.applyAtRound = ULLONG_MAX;
            }
        }

        ++blocks_walked;
        it->next();
    }

    std::fprintf(stderr, "walked %llu blocks, found %llu special transactions\n",
                 static_cast<unsigned long long>(blocks_walked),
                 static_cast<unsigned long long>(special_tx_seen));
    if (cutoff != ULLONG_MAX) {
        std::fprintf(stderr, "stopped at H_activate_decentralization = %llu\n",
                     static_cast<unsigned long long>(cutoff));
    } else {
        std::fprintf(stderr, "H_activate_decentralization = UINT64_MAX (walked full chain)\n");
    }

    print_results();
    return 0;
}
