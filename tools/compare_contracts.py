#!/usr/bin/env python3
"""
compare_contracts.py — Read-only contract-call differential test between two
CREDITS nodes.

Picks N smart contracts from node A, finds a no-arg getter method on each,
and submits a `forgetNewState=true` (read-only) call to BOTH nodes. Compares
the returned Variant. Reports matches, mismatches, and per-contract errors.

Why forgetNewState=true: the API handler skips signature validation for
getter calls (apihandler.cpp:839 condition is `!forgetNewState`), so this
script does not need a real signing key — dummy zero-bytes for source /
signature suffice. No state is mutated on either node.

Setup (one time):
    pip install thrift
    mkdir -p gen-py
    thrift -r --out gen-py --gen py \\
        ../third-party/thrift-interface-definitions/api.thrift

Then point Python at the generated bindings and run:
    PYTHONPATH=gen-py python3 compare_contracts.py \\
        --node-a 1.2.3.4 --node-b 5.6.7.8 [--count 100] [--port 9090]

Exit code 0 if all contracts match (or were skipped for benign reasons),
non-zero if any mismatch.
"""

import argparse
import sys
import time
from dataclasses import dataclass

try:
    from api import API
    from api.ttypes import Transaction, SmartContractInvocation, TransactionId, TransactionType
    # general.Address / general.AccessID are typedefs of binary/i64 in the
    # thrift IDL, so they don't get wrapper classes — values are plain bytes
    # / int. We still pull Amount and Variant which ARE structs.
    from general.ttypes import Amount, Variant
    from thrift.transport import TSocket, TTransport
    from thrift.protocol import TBinaryProtocol

    # The IDL defines `Variant` as a union and uses `map<Variant, Variant>`
    # in some contract state variables. The python thrift codegen does not
    # produce __hash__ for unions, so deserialising those maps fails with
    # "unhashable type: 'Variant'". Patch it via a tuple of (set-field, value)
    # which is stable for any concrete Variant we see.
    def _variant_hash(self):
        for k, v in vars(self).items():
            if v is not None and k.startswith("v_"):
                try:
                    return hash((k, v))
                except TypeError:
                    return hash((k, repr(v)))
        return 0
    Variant.__hash__ = _variant_hash
except ImportError as e:
    sys.stderr.write(
        "Missing thrift bindings. Generate with:\n"
        "    thrift -r --out gen-py --gen py "
        "../third-party/thrift-interface-definitions/api.thrift\n"
        "and run with PYTHONPATH=gen-py.\n"
        f"Original import error: {e}\n"
    )
    sys.exit(2)


@dataclass
class ContractTarget:
    address: bytes          # 32-byte raw address
    address_hex: str        # for log lines
    method: str             # method name to call
    return_type: str        # for log lines


@dataclass
class CallOutcome:
    ok: bool                # API call returned status code 0 (Success)
    status_code: int
    status_msg: str
    variant: object         # the Variant returned (or None)


def open_client(host: str, port: int) -> API.Client:
    sock = TSocket.TSocket(host, port)
    sock.setTimeout(15000)  # 15s — read-only getters should be quick
    transport = TTransport.TBufferedTransport(sock)
    protocol = TBinaryProtocol.TBinaryProtocol(transport)
    transport.open()
    return API.Client(protocol)


def list_contracts(client: API.Client, limit: int) -> list:
    res = client.SmartContractsAllListGet(0, limit)
    if res.status.code != 0:
        sys.stderr.write(f"SmartContractsAllListGet failed: {res.status.message}\n")
        return []
    return list(res.smartContractsList)


def pick_getter(client: API.Client, addr: bytes) -> tuple:
    """Return (method_name, return_type) for a no-arg, non-void method on the
    contract, or (None, None) if no usable getter exists."""
    res = client.SmartContractDataGet(addr)
    if res.status.code != 0 or not res.methods:
        return (None, None)
    # Prefer methods named like getX / isX / first available no-arg non-void.
    candidates = []
    for m in res.methods:
        if m.arguments:                 # require zero args
            continue
        if not m.returnType:
            continue
        if m.returnType.lower() == "void":
            continue
        candidates.append(m)
    if not candidates:
        return (None, None)
    # Bias toward common idempotent token getters first
    priority = ("getName", "getSymbol", "getDecimals", "totalSupply",
                "name", "symbol", "decimals")
    for p in priority:
        for m in candidates:
            if m.name == p:
                return (m.name, m.returnType)
    return (candidates[0].name, candidates[0].returnType)


def make_getter_tx(target: ContractTarget) -> Transaction:
    """Construct a getter-mode transaction. forgetNewState=true bypasses
    signature validation in the API handler, so all dummy fields are fine."""
    inv = SmartContractInvocation()
    inv.method = target.method
    inv.params = []
    inv.usedContracts = []
    inv.forgetNewState = True
    inv.version = 1

    tx = Transaction()
    tx.id = int(time.time() * 1000) & 0x7fffffffffffffff   # any unique value
    tx.source = b"\x00" * 32
    tx.target = target.address
    tx.amount = Amount(0, 0)
    tx.balance = Amount(0, 0)
    tx.currency = 1
    tx.signature = b"\x00" * 64
    tx.smartContract = inv
    tx.fee = None
    tx.timeCreation = int(time.time() * 1000)
    tx.userFields = b""
    tx.type = TransactionType.TT_ContractCall
    tx.smartInfo = None
    tx.extraFee = None
    tx.poolNumber = 0
    tx.usedContracts = []
    return tx


def call(client: API.Client, target: ContractTarget) -> CallOutcome:
    tx = make_getter_tx(target)
    res = client.TransactionFlow(tx)
    return CallOutcome(
        ok=(res.status.code == 0),
        status_code=res.status.code,
        status_msg=res.status.message or "",
        variant=res.smart_contract_result,
    )


def variants_equal(a, b) -> bool:
    """Compare two thrift Variants structurally. Returns True if logically
    equal. Handles the union by comparing the single set field."""
    if a is None and b is None:
        return True
    if a is None or b is None:
        return False
    # Variant is a union; iterate set attributes.
    a_fields = {k: v for k, v in vars(a).items() if v is not None}
    b_fields = {k: v for k, v in vars(b).items() if v is not None}
    if a_fields.keys() != b_fields.keys():
        return False
    for k in a_fields:
        if a_fields[k] != b_fields[k]:
            return False
    return True


def variant_summary(v) -> str:
    if v is None:
        return "<none>"
    for k, val in vars(v).items():
        if val is not None and k.startswith("v_"):
            text = repr(val)
            return f"{k}={text[:80]}"
    return "<empty>"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--node-a", required=True, help="node A IP/hostname")
    ap.add_argument("--node-b", required=True, help="node B IP/hostname")
    ap.add_argument("--port", type=int, default=9090, help="public API port (default 9090)")
    ap.add_argument("--count", type=int, default=100, help="contracts to compare (default 100)")
    args = ap.parse_args()

    print(f"Connecting: A={args.node_a}:{args.port}, B={args.node_b}:{args.port}")
    try:
        client_a = open_client(args.node_a, args.port)
        client_b = open_client(args.node_b, args.port)
    except Exception as e:
        sys.stderr.write(f"Failed to connect: {e}\n")
        return 2

    print(f"Listing {args.count} contracts from node A ...")
    contracts = list_contracts(client_a, args.count)
    print(f"Got {len(contracts)} contract entries")

    matches = 0
    mismatches = 0
    skipped_no_getter = 0
    errors_a = 0
    errors_b = 0
    mismatch_details = []

    for i, sc in enumerate(contracts):
        addr = sc.address     # Address is a binary typedef → plain bytes
        addr_hex = addr.hex()
        method, return_type = pick_getter(client_a, addr)
        if method is None:
            skipped_no_getter += 1
            continue
        target = ContractTarget(addr, addr_hex, method, return_type)
        try:
            out_a = call(client_a, target)
        except Exception as e:
            errors_a += 1
            print(f"[{i+1:3}/{len(contracts)}] {addr_hex[:16]}.. {method}() : A error {e}")
            continue
        try:
            out_b = call(client_b, target)
        except Exception as e:
            errors_b += 1
            print(f"[{i+1:3}/{len(contracts)}] {addr_hex[:16]}.. {method}() : B error {e}")
            continue
        if out_a.ok != out_b.ok or out_a.status_code != out_b.status_code:
            mismatches += 1
            mismatch_details.append((addr_hex, method,
                f"status A={out_a.status_code}/{out_a.status_msg!r}, "
                f"B={out_b.status_code}/{out_b.status_msg!r}"))
            print(f"[{i+1:3}/{len(contracts)}] {addr_hex[:16]}.. {method}() : STATUS MISMATCH")
            continue
        if not out_a.ok:
            # Both returned the same non-success — count as a match (not useful but consistent)
            matches += 1
            continue
        if variants_equal(out_a.variant, out_b.variant):
            matches += 1
            print(f"[{i+1:3}/{len(contracts)}] {addr_hex[:16]}.. {method}() : OK ({variant_summary(out_a.variant)})")
        else:
            mismatches += 1
            mismatch_details.append((addr_hex, method,
                f"A={variant_summary(out_a.variant)} | B={variant_summary(out_b.variant)}"))
            print(f"[{i+1:3}/{len(contracts)}] {addr_hex[:16]}.. {method}() : VALUE MISMATCH "
                  f"A={variant_summary(out_a.variant)} | B={variant_summary(out_b.variant)}")

    print()
    print("=" * 60)
    print(f"Total contracts considered : {len(contracts)}")
    print(f"  match                    : {matches}")
    print(f"  mismatch                 : {mismatches}")
    print(f"  skipped (no getter)      : {skipped_no_getter}")
    print(f"  errors on A              : {errors_a}")
    print(f"  errors on B              : {errors_b}")

    if mismatch_details:
        print()
        print("Mismatch details:")
        for addr, method, detail in mismatch_details:
            print(f"  {addr}  {method}()")
            print(f"    {detail}")

    return 0 if mismatches == 0 and errors_a == 0 and errors_b == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
