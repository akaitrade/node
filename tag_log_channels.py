"""Tag each target .cpp with `#define CS_LOG_CHANNEL "<channel>"` as the first line.

Idempotent — skips files that already declare CS_LOG_CHANNEL.
"""
from pathlib import Path

ROOT = Path(r"C:\Users\basju\Documents\Projects\node")

CHANNELS: dict[str, list[str]] = {
    "consensus": [
        "solver/src/states/defaultstatebehavior.cpp",
        "solver/src/states/handlebbstate.cpp",
        "solver/src/states/handlertstate.cpp",
        "solver/src/states/normalstate.cpp",
        "solver/src/states/primitivewritestate.cpp",
        "solver/src/states/syncstate.cpp",
        "solver/src/states/trustedpoststagestate.cpp",
        "solver/src/states/trustedstage1state.cpp",
        "solver/src/states/trustedstage2state.cpp",
        "solver/src/states/trustedstage3state.cpp",
        "solver/src/states/waitingstate.cpp",
        "solver/src/states/writingstate.cpp",
        "solver/src/smartconsensus.cpp",
        "solver/src/solverinterface.cpp",
    ],
    "sync": [
        "csnode/src/poolsynchronizer.cpp",
    ],
    "smartcontracts": [
        "csnode/src/smartcontracts_serializer.cpp",
        "solver/src/smartcontracts.cpp",
    ],
    "blockchain": [
        "csnode/src/blockchain.cpp",
        "csnode/src/blockhashes.cpp",
        "csnode/src/transactionsindex.cpp",
        "csdb/src/address.cpp",
        "csdb/src/amount.cpp",
        "csdb/src/amount_commission.cpp",
        "csdb/src/binary_streams.cpp",
        "csdb/src/csdb.cpp",
        "csdb/src/currency.cpp",
        "csdb/src/database.cpp",
        "csdb/src/database_berkeleydb.cpp",
        "csdb/src/database_rocksdb.cpp",
        "csdb/src/integral_encdec.cpp",
        "csdb/src/pool.cpp",
        "csdb/src/priv_crypto.cpp",
        "csdb/src/storage.cpp",
        "csdb/src/transaction.cpp",
        "csdb/src/user_field.cpp",
        "csdb/src/utils.cpp",
        "csdb/src/wallet.cpp",
    ],
    "net": [
        "net/src/neighbourhood.cpp",
        "net/src/networkcommands.cpp",
        "net/src/packet.cpp",
        "net/src/packetsqueue.cpp",
        "net/src/packetvalidator.cpp",
        "net/src/transport.cpp",
    ],
}


def tag(path: Path, channel: str) -> str:
    text = path.read_text(encoding="utf-8", errors="surrogateescape")
    head = "\n".join(text.splitlines()[:5])
    if "CS_LOG_CHANNEL" in head:
        return "skip (already tagged)"
    new_text = f'#define CS_LOG_CHANNEL "{channel}"\n' + text
    path.write_text(new_text, encoding="utf-8", errors="surrogateescape", newline="")
    return "tagged"


def main() -> None:
    for channel, files in CHANNELS.items():
        for rel in files:
            p = ROOT / rel
            if not p.is_file():
                print(f"  MISSING: {rel}")
                continue
            status = tag(p, channel)
            print(f"  [{channel:>15}] {status}: {rel}")


if __name__ == "__main__":
    main()
