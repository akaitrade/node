#!/usr/bin/env python3
"""
tps_gen.py — Credits node TPS generator.

Derives N child keys from a master seed (SHA256(master_seed || u32_be(i))[:32]),
funds them via TransactionFlow, then pre-signs native-coin transfers and pushes
them through the node's Thrift API at a configurable rate.

Quick start:
  pip install -r requirements.txt
  python tps_gen.py --master <base58_seed_32B> --children 64 --tps 8000

Reasonable production setup for 10K TPS:
  --children 128 --workers 16 --batch 200 --presign 200000

Modes:
  --send-mode transactionslistsend  (default; batched, highest TPS)
  --send-mode transactionsend       (single tx, fire-and-forget)
  --send-mode transactionflow       (blocking until consensus; will not reach 10K)
"""

import argparse
import hashlib
import os
import queue
import random
import struct
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

import base58
import nacl.signing
import thriftpy2
from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_client
from thriftpy2.transport import TBufferedTransportFactory

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
THRIFT_ROOT = os.path.abspath(os.path.join(THIS_DIR, "..", "..",
                                            "third-party", "thrift-interface-definitions"))

api = thriftpy2.load(os.path.join(THRIFT_ROOT, "api.thrift"),
                     module_name="api_thrift",
                     include_dirs=[THRIFT_ROOT])
general = thriftpy2.load(os.path.join(THRIFT_ROOT, "general.thrift"),
                         module_name="general_thrift",
                         include_dirs=[THRIFT_ROOT])

CURRENCY_CS = 1
TX_TYPE_TRANSFER = 0
AMOUNT_MAX_FRACTION = 10 ** 18


def encode_max_fee(value: float) -> int:
    """Mirror csdb::AmountCommission(double) → uint16 bit pattern."""
    import math
    if value <= 0:
        return 0
    v = abs(value)
    expf = math.log10(v) if v > 0 else 0.0
    expi = int(expf + 0.5) if expf >= 0 else int(expf - 0.5)
    v /= 10 ** expi
    if v >= 1.0:
        v *= 0.1
        expi += 1
    sign = 0
    exp = (expi + 18) & 0x1F
    frac = int(round(v * 1024)) & 0x3FF
    return ((sign & 1) << 15) | ((exp & 0x1F) << 10) | (frac & 0x3FF)


def derive_child_seed(master_seed: bytes, index: int) -> bytes:
    return hashlib.sha256(master_seed + struct.pack(">I", index)).digest()


class Account:
    __slots__ = ("sk", "pk", "next_inner_id", "lock")

    def __init__(self, seed: bytes):
        self.sk = nacl.signing.SigningKey(seed)
        self.pk = bytes(self.sk.verify_key)
        self.next_inner_id = 1
        self.lock = threading.Lock()


def pack_inner_id(inner_id: int, src_is_wid: bool, tgt_is_wid: bool) -> bytes:
    if inner_id >= (1 << 46):
        raise ValueError("innerID overflow (>= 2^46)")
    raw = bytearray(inner_id.to_bytes(6, "little"))
    raw[5] |= ((1 if src_is_wid else 0) << 7) | ((1 if tgt_is_wid else 0) << 6)
    return bytes(raw)


def build_sig_payload(src_pk: bytes, tgt_pk: bytes, inner_id: int,
                      amount_int: int, amount_frac: int,
                      max_fee_bits: int) -> bytes:
    return b"".join([
        pack_inner_id(inner_id, False, False),
        src_pk,
        tgt_pk,
        struct.pack("<iQ", amount_int, amount_frac),
        struct.pack("<H", max_fee_bits),
        struct.pack("<B", CURRENCY_CS),
        struct.pack("<B", 0),
    ])


def build_transaction(src: Account, dst_pk: bytes, inner_id: int,
                      amount_int: int, amount_frac: int,
                      max_fee_bits: int):
    payload = build_sig_payload(src.pk, dst_pk, inner_id,
                                amount_int, amount_frac, max_fee_bits)
    sig = src.sk.sign(payload).signature

    t = api.Transaction()
    t.id = inner_id
    t.source = src.pk
    t.target = dst_pk
    t.amount = general.Amount(integral=amount_int, fraction=amount_frac)
    t.balance = general.Amount(integral=0, fraction=0)
    t.currency = CURRENCY_CS
    t.signature = sig
    t.fee = api.AmountCommission(commission=max_fee_bits)
    t.timeCreation = int(time.time() * 1000)
    t.userFields = b""
    t.type = TX_TYPE_TRANSFER
    t.poolNumber = 0
    return t


def make_api_client(host: str, port: int, timeout_ms: int = 60000):
    return make_client(api.API, host=host, port=port,
                       proto_factory=TBinaryProtocolFactory(),
                       trans_factory=TBufferedTransportFactory(),
                       timeout=timeout_ms)


def fetch_next_inner_id(client, pk: bytes) -> int:
    res = client.WalletTransactionsCountGet(pk)
    last = res.lastTransactionInnerId or 0
    return int(last) + 1


def fund_children(host: str, port: int, master: Account,
                  children, amount_int: int, max_fee_bits: int,
                  inflight_cap: int = 4):
    """Send funding transfers from master to every child via TransactionFlow.
    TransactionFlow blocks until commit, so we cap concurrent in-flight calls."""
    print(f"[fund] funding {len(children)} children with {amount_int} CS each ...")
    client0 = make_api_client(host, port)
    master.next_inner_id = fetch_next_inner_id(client0, master.pk)
    client0.close()
    print(f"[fund] master next inner id = {master.next_inner_id}")

    txs = []
    for c in children:
        iid = master.next_inner_id
        master.next_inner_id += 1
        txs.append(build_transaction(master, c.pk, iid,
                                     amount_int, 0, max_fee_bits))

    sem = threading.Semaphore(inflight_cap)
    errors = []
    err_lock = threading.Lock()
    done = [0]

    def send_one(tx):
        cli = make_api_client(host, port)
        try:
            r = cli.TransactionFlow(tx)
            if r.status.code != 0:
                with err_lock:
                    errors.append(r.status.message)
        except Exception as e:
            with err_lock:
                errors.append(str(e))
        finally:
            try:
                cli.close()
            except Exception:
                pass
            done[0] += 1
            sem.release()

    threads = []
    for tx in txs:
        sem.acquire()
        th = threading.Thread(target=send_one, args=(tx,), daemon=True)
        th.start()
        threads.append(th)

    for th in threads:
        th.join()

    print(f"[fund] done={done[0]} errors={len(errors)}")
    if errors:
        for msg in errors[:5]:
            print(f"  ! {msg}")
        if len(errors) > 5:
            print(f"  ... ({len(errors) - 5} more)")


class PresignWorker(threading.Thread):
    """Continuously refill a bounded queue with pre-signed transactions."""

    def __init__(self, accounts, q, max_fee_bits, stop_evt,
                 amount_int: int = 0, amount_frac: int = 1):
        super().__init__(daemon=True)
        self.accounts = accounts
        self.q = q
        self.max_fee_bits = max_fee_bits
        self.stop_evt = stop_evt
        self.amount_int = amount_int
        self.amount_frac = amount_frac

    def run(self):
        n = len(self.accounts)
        while not self.stop_evt.is_set():
            src = random.choice(self.accounts)
            dst = self.accounts[random.randrange(n)]
            if dst is src:
                dst = self.accounts[(self.accounts.index(src) + 1) % n]
            with src.lock:
                iid = src.next_inner_id
                src.next_inner_id += 1
            tx = build_transaction(src, dst.pk, iid,
                                   self.amount_int, self.amount_frac,
                                   self.max_fee_bits)
            try:
                self.q.put(tx, timeout=0.5)
            except queue.Full:
                continue


class Sender(threading.Thread):
    def __init__(self, host, port, q, batch, mode, sent, fails,
                 rate_gate, stop_evt):
        super().__init__(daemon=True)
        self.host = host
        self.port = port
        self.q = q
        self.batch = batch
        self.mode = mode
        self.sent = sent
        self.fails = fails
        self.rate_gate = rate_gate
        self.stop_evt = stop_evt

    def run(self):
        cli = None
        while not self.stop_evt.is_set():
            try:
                if cli is None:
                    cli = make_api_client(self.host, self.port, timeout_ms=15000)
                if self.mode == "transactionslistsend":
                    batch = []
                    for _ in range(self.batch):
                        try:
                            batch.append(self.q.get(timeout=0.2))
                        except queue.Empty:
                            break
                    if not batch:
                        continue
                    self.rate_gate(len(batch))
                    payload = api.TransactionsList(transactions=batch)
                    r = cli.TransactionsListSend(payload)
                    if r.status.code == 0:
                        self.sent[0] += len(batch)
                    else:
                        self.fails[0] += len(batch)
                elif self.mode == "transactionsend":
                    try:
                        tx = self.q.get(timeout=0.2)
                    except queue.Empty:
                        continue
                    self.rate_gate(1)
                    r = cli.TransactionSend(tx)
                    if r.status.code == 0:
                        self.sent[0] += 1
                    else:
                        self.fails[0] += 1
                else:
                    try:
                        tx = self.q.get(timeout=0.2)
                    except queue.Empty:
                        continue
                    self.rate_gate(1)
                    r = cli.TransactionFlow(tx)
                    if r.status.code == 0:
                        self.sent[0] += 1
                    else:
                        self.fails[0] += 1
            except Exception:
                self.fails[0] += 1
                try:
                    cli.close()
                except Exception:
                    pass
                cli = None
                time.sleep(0.05)
        if cli is not None:
            try:
                cli.close()
            except Exception:
                pass


class RateLimiter:
    """Token-bucket rate limiter shared across sender threads."""

    def __init__(self, rate: float, burst: float = None):
        self.rate = float(rate)
        self.tokens = float(burst if burst is not None else rate)
        self.cap = float(burst if burst is not None else rate)
        self.last = time.monotonic()
        self.lock = threading.Lock()

    def acquire(self, n: int):
        while True:
            with self.lock:
                now = time.monotonic()
                dt = now - self.last
                self.last = now
                self.tokens = min(self.cap, self.tokens + dt * self.rate)
                if self.tokens >= n:
                    self.tokens -= n
                    return
                deficit = n - self.tokens
            time.sleep(min(0.05, deficit / max(self.rate, 1.0)))


def parse_args():
    p = argparse.ArgumentParser(description="Credits TPS generator")
    p.add_argument("--master", required=True,
                   help="base58-encoded 32-byte master seed (funding source)")
    p.add_argument("--node", default="127.0.0.1:9090",
                   help="node API host:port (default 127.0.0.1:9090)")
    p.add_argument("--children", type=int, default=64,
                   help="number of derived source accounts (default 64)")
    p.add_argument("--tps", type=int, default=5000,
                   help="target transactions per second (default 5000)")
    p.add_argument("--duration", type=int, default=30,
                   help="run seconds (0 = forever; default 30)")
    p.add_argument("--batch", type=int, default=100,
                   help="txns per TransactionsListSend call (default 100)")
    p.add_argument("--workers", type=int, default=8,
                   help="concurrent sender threads/connections (default 8)")
    p.add_argument("--presign", type=int, default=50000,
                   help="presigned-queue capacity (default 50k)")
    p.add_argument("--presign-workers", type=int, default=4,
                   help="threads pre-signing transactions (default 4)")
    p.add_argument("--fund-amount", type=int, default=100,
                   help="CS to send to each child during funding (default 100)")
    p.add_argument("--max-fee", type=float, default=0.1,
                   help="max fee per tx as float (default 0.1)")
    p.add_argument("--skip-fund", action="store_true",
                   help="assume children are already funded")
    p.add_argument("--send-mode",
                   choices=["transactionslistsend", "transactionsend", "transactionflow"],
                   default="transactionslistsend")
    return p.parse_args()


def main():
    args = parse_args()
    host, _, port_s = args.node.partition(":")
    port = int(port_s or "9090")

    master_raw = base58.b58decode(args.master)
    if len(master_raw) not in (32, 64):
        sys.exit(f"--master decoded to {len(master_raw)} bytes; expected 32 (seed) "
                 "or 64 (Credits private key = seed || pubkey)")
    master = Account(master_raw[:32])
    if len(master_raw) == 64 and master_raw[32:] != master.pk:
        sys.exit("--master is 64 bytes but the embedded pubkey does not match the "
                 "pubkey derived from the seed half; key format is not libsodium ed25519")
    children = [Account(derive_child_seed(master_raw[:32], i))
                for i in range(args.children)]

    print(f"[init] node={host}:{port} children={len(children)} "
          f"tps={args.tps} duration={args.duration}s mode={args.send_mode}")
    print(f"[init] master pk b58 = {base58.b58encode(master.pk).decode()}")

    max_fee_bits = encode_max_fee(args.max_fee)

    if not args.skip_fund:
        fund_children(host, port, master, children,
                      amount_int=args.fund_amount,
                      max_fee_bits=max_fee_bits)
        print("[fund] waiting 8s for funding to settle ...")
        time.sleep(8)

    print("[init] fetching child next-inner-ids ...")
    cli = make_api_client(host, port)
    for c in children:
        try:
            c.next_inner_id = fetch_next_inner_id(cli, c.pk)
        except Exception as e:
            print(f"  ! failed for {base58.b58encode(c.pk).decode()[:12]}: {e}")
            c.next_inner_id = 1
    cli.close()

    q = queue.Queue(maxsize=args.presign)
    stop_evt = threading.Event()
    for _ in range(args.presign_workers):
        PresignWorker(children, q, max_fee_bits, stop_evt,
                      amount_int=0, amount_frac=1).start()

    print(f"[init] warming presign queue (target {args.presign}) ...")
    t0 = time.time()
    while q.qsize() < min(args.presign, args.batch * args.workers * 4):
        if time.time() - t0 > 20:
            break
        time.sleep(0.1)
    print(f"[init] presign queue at {q.qsize()}, starting senders")

    rate = RateLimiter(args.tps, burst=max(args.tps, args.batch * args.workers))
    sent = [0]
    fails = [0]
    senders = []
    for _ in range(args.workers):
        s = Sender(host, port, q, args.batch, args.send_mode,
                   sent, fails, rate.acquire, stop_evt)
        s.start()
        senders.append(s)

    start = time.time()
    last_sent = 0
    last_t = start
    try:
        while True:
            time.sleep(1.0)
            now = time.time()
            cur = sent[0]
            inst = (cur - last_sent) / max(now - last_t, 1e-3)
            avg = cur / max(now - start, 1e-3)
            print(f"[stat] t={now-start:5.1f}s sent={cur:>9d} fails={fails[0]:>6d} "
                  f"q={q.qsize():>6d} inst={inst:8.1f} avg={avg:8.1f} TPS")
            last_sent = cur
            last_t = now
            if args.duration and (now - start) >= args.duration:
                break
    except KeyboardInterrupt:
        print("\n[stop] interrupted")

    stop_evt.set()
    print("[stop] draining senders ...")
    for s in senders:
        s.join(timeout=2.0)

    elapsed = time.time() - start
    print(f"[done] total sent={sent[0]} fails={fails[0]} elapsed={elapsed:.1f}s "
          f"avg={sent[0]/max(elapsed,1e-3):.1f} TPS")


if __name__ == "__main__":
    main()
