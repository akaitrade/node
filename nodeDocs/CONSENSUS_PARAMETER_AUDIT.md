# Consensus Parameter Audit — Decentralization Hard-Fork Safety Check

## ⚠️ Current build setting — TESTNET ONLY

`Consensus::H_activate_decentralization` is currently compiled in as **`1`** in
`solver/include/solver/consensus.hpp`. This enables decentralized 2/3-quorum recovery
from block 1 onward so the feature can be exercised on local testnets.

**Before mainnet release this constant MUST be changed back to either `UINT64_MAX`
(dormant) or a real future block height agreed by validators.** Shipping `=1` to mainnet
would invalidate every historical pre-activation block's admin-signed BootstrapTable
expectations and is not safe.

## Purpose

This document is the operator checklist for verifying live mainnet consensus parameter
state before committing to a real value for `Consensus::H_activate_decentralization`.

**Why it matters.** When `H_activate_decentralization` is set to a real block height,
`processSpecialInfo` stops mutating `Consensus::*` fields for any block at or after
that height. From that point onward every node derives its parameters from two sources:
the static defaults compiled into `solver/src/consensus.cpp`, plus whatever historical
admin transactions were recorded before the cut-off height and are replayed during chain
sync. A node syncing from genesis re-applies every pre-activation admin transaction in
order, so it converges to the same live state as an existing node — provided its binary
has the same static defaults. If a parameter was silently mutated by admin in the past
and the compiled-in static default differs from the last admin-set value, a fresh node
bootstrapping from genesis with a new binary will diverge from existing nodes at the
first admin transaction that changed that parameter, producing a hard fork.

**How to verify.** Run the `dump_consensus_state` tool (see `tools/dump_consensus_state/`)
against the live database before flipping the activation height:

```
dump_consensus_state /path/to/bdb_data
```

Compare each printed line against the matching default in `solver/src/consensus.cpp`.
Every line must match. Any deviation is a parameter that was admin-set and whose static
default needs updating in the binary before `H_activate_decentralization` is activated.

---

## Mutable Parameters — Full Audit Table

| Order | Parameter name | Declaration (consensus.hpp) | Static default (consensus.cpp) | Type | Effect of wrong default |
|------:|----------------|-----------------------------|---------------------------------|------|-------------------------|
| 9 | `Consensus::StartingDPOS` | line 71 | line 34 — `10'000ULL` | `uint64_t` | Activating DPOS too early lets under-staked nodes become trusted before stake checks are meaningful, undermining validator quality; too late delays decentralized selection and extends admin-dependent bootstrapping. |
| 11 | `Consensus::MaxRoundsExecuteContract` | line 146 | line 109 — `10` | `unsigned int` | Too small a value kills long-running contracts prematurely before executor returns; too large holds consensus threads waiting on hung contracts, stalling the whole round. |
| 21 | `Consensus::StageOneMaximumSize` | line 31 | line 19 — `36000` | `uint64_t` | Too low truncates Stage-1 data causing valid hashes to be dropped and consensus to abort rounds; too high widens the network attack surface for oversized Stage-1 spam. |
| 22 | `Consensus::MinStakeValue` | line 62 | line 29 — `csdb::Amount{50000}` | `csdb::Amount` | Too low makes validator slots trivially cheap, enabling sybil attackers to flood the trusted-node set at minimal cost; too high excludes legitimate validators and reduces set diversity. |
| 23 | `Consensus::TimeMinStage1` | line 80 | line 43 — `500` (ms) | `uint32_t` | Too short causes the writer to close Stage-1 before slow nodes have submitted their hashes, inflating absent-hash penalties and gray-listing healthy nodes; too long adds latency to every round. |
| 24 | `Consensus::GrayListPunishment` | line 83 | line 46 — `1000` (rounds) | `uint32_t` | Too small lets misbehaving nodes cycle back into consensus quickly, weakening the disincentive; too large permanently locks out nodes that suffered transient connectivity issues. |
| 25 | `Consensus::MaxStageOneHashes` | line 113 | line 76 — `100` | `size_t` | Too low caps the number of participating trusted nodes whose hashes count, silently excluding latecomers and degrading BFT guarantees; too high bloats Stage-1 messages and slows serialization. |
| 26 | `Consensus::MaxTransactionSize` | line 125 | line 88 — `102400` (100 KiB) | `size_t` | Too small rejects valid large transactions (e.g., contract deploys) at the conveyer; too large allows individual transactions to monopolize block space and amplify DoS payloads. |
| 27 | `Consensus::MaxStageOneTransactions` | line 128 | line 91 — `1000` | `size_t` | Too low caps transactions captured in Stage-1, causing excess transactions to be silently deferred across rounds and reducing throughput; too high bloats Stage-1 packets. |
| 28 | `Consensus::MaxPreliminaryBlockSize` | line 131 | line 94 — `1048576` (1 MiB) | `size_t` | Too small causes the writer to truncate blocks mid-fill, reducing effective throughput; too large permits runaway block growth that strains network and disk I/O for all nodes. |
| 29 | `Consensus::MaxPacketsPerRound` | line 85 | line 48 — `1000` | `size_t` | Too low rate-limits the conveyer and starves the block writer of transactions; too high allows a single sender to monopolize the conveyer queue and crowd out other participants. |
| 30 | `Consensus::MaxPacketTransactions` | line 86 | line 49 — `1000` | `size_t` | Too small forces many small packets per sender, increasing per-packet overhead and gossip amplification; too large enables a single malicious packet to monopolize block capacity. |
| 31 | `Consensus::MaxQueueSize` | line 89 | line 52 — `1000000` | `size_t` | Too small causes premature queue-full rejections under normal load spikes, dropping valid transactions; too large allows unbounded memory growth on a slow-consuming node under attack. |
| 35 | `Consensus::syncroChangeRound` | line 163 | line 129 — `ULLONG_MAX` | `cs::RoundNumber` (`uint64_t`) | If left as `ULLONG_MAX` no deferred settings change fires, which is safe; if incorrectly reset to a real round number, mining/staking parameters toggle at that round unexpectedly on fresh nodes that replayed history differently. |
| 37 | `Consensus::stakingOn` | line 159 | line 125 — `false` | `bool` | Staking enabled by default on fresh nodes that did not replay the enabling admin transaction will grant staking rewards without the protective pre-activation round table, causing wallet-cache divergence. |
| 37 | `Consensus::miningOn` | line 160 | line 127 — `false` | `bool` | Mining enabled by default distributes block rewards on fresh nodes before the activating admin transaction is replayed, creating balance divergence versus nodes that synced incrementally. |
| 37 | `Consensus::blockReward` | line 155 | line 118 — `csdb::Amount{0}` | `csdb::Amount` | Non-zero default on a fresh node means reward transactions are synthesised before the first admin-set reward transaction is replayed, inflating balances and hard-forking from the live chain. |
| 37 | `Consensus::miningCoefficient` | line 157 | line 120 — `csdb::Amount{0}` | `csdb::Amount` | Non-zero default changes the per-block reward multiplier before replay of the admin transaction that introduced it, producing cumulative balance errors across every rewarded block in history. |

### Note on order 37

Order 37 is decoded inside `processSpecialInfo` and then held in node-private fields
(`stakingOn_`, `miningOn_`, `blockReward_`, `miningCoefficient_`) until round
`consensusSettingsChangingRound_` fires, at which point `checkConsensusSettings` commits
them to the `Consensus::*` statics. The dump tool must simulate this two-phase
commit when replaying history.

### Parameters with no compile-time fallback (flag for review)

None of the order-37 sub-parameters (`stakingOn`, `miningOn`, `blockReward`,
`miningCoefficient`) have a static default that is non-trivial; all four default to
"off / zero" in `consensus.cpp`. If mainnet ever had an admin transaction that turned
these on and the activation height is set before a corresponding off-transaction, the
compiled-in `false`/`0` defaults will mismatch the live state. The dump tool output
must be compared against the *last* effective admin-set value for each, not just the
compile-time default.
