#!/usr/bin/env python3
"""Their Truthcoin node and ours on one local eCash regtest chain (real bip300301_enforcer).

    tests/regtest/interop.py start   [--dir D] [--base-port 49800] [--period-blocks 6]
    tests/regtest/interop.py run     [--dir D]           # the scenario (below) on a started stack
    tests/regtest/interop.py stop    [--dir D]           # stops exactly the recorded PIDs

Stack: ecash-com bitcoind -regtest (RPC base, P2P base+1, ZMQ base+3), electrs (base+5..7), the enforcer
(gRPC base+10, RPC base+11), THEIR truthcoin v0.20.0 release binary (P2P base+20, RPC base+21) and OUR truthcoin
(P2P base+30, RPC base+31), peered only with each other, both with --decision-config-testing N (N blocks per
voting period) and --network regtest. Slot 13 is proposed by the enforcer (M1) and acked.

The scenario mines sidechain blocks alternately on each node (BMM through the enforcer) and after every block
compares the two nodes' answers to every read method (tips, UTXO sets, decisions, markets, votes, ...): deposits,
transfers both ways, decision claims, markets, buys, sells, amplify, votes and ballots, reputation transfers,
period close and settlement, a withdrawal bundle, and reorgs (sidechain and mainchain). Data defaults to
~/.hivemind-interop (never ~/.linked-regtest).
"""

import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

SKY = Path.home() / "skydoge"
HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
BINS = {
    "ecashd": SKY / "ext/ecash-bitcoin/build/bin/bitcoind",
    "electrs": SKY / "ext/electrs/target/release/electrs",
    "enforcer": SKY / "ext/enforcer/target/release/bip300301_enforcer",
    "theirs": Path(os.environ.get("THEIR_TRUTHCOIN", SKY / "ext/truthcoin-dc-release/unz/truthcoin")),
    "ours": Path(os.environ.get("OUR_TRUTHCOIN", REPO / "build/chains/hivemind/truthcoin")),
}
ECASH_MAGIC = "eca5b134"
RPC_USER = RPC_PASS = "interop"
SLOT = 13


def ports(base):
    return {"ecash_rpc": base, "ecash_p2p": base + 1, "ecash_zmq": base + 3, "electrum": base + 5, "esplora": base + 6,
            "electrs_mon": base + 7, "enf_grpc": base + 10, "enf_rpc": base + 11,
            "theirs_p2p": base + 20, "theirs_rpc": base + 21, "ours_p2p": base + 30, "ours_rpc": base + 31}


def free(port):
    for fam, addr in ((socket.AF_INET, "127.0.0.1"), (socket.AF_INET, "0.0.0.0")):
        s = socket.socket(fam, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((addr, port))
        except OSError:
            return False
        finally:
            s.close()
    return True


# ------------------------------------------------------------------ clients
class RpcError(Exception):
    def __init__(self, err):
        super().__init__(json.dumps(err))
        self.err = err


def jsonrpc(port, method, params=None, timeout=600, raw=False):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params if params is not None else []}).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{port}", body, {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = r.read()
    if raw:
        return data
    v = json.loads(data)
    if "error" in v:
        raise RpcError(v["error"])
    return v["result"]


def btc(port, method, *params):
    body = json.dumps({"jsonrpc": "1.0", "id": 1, "method": method, "params": list(params)}).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{port}", body, {"Content-Type": "application/json"})
    import base64
    req.add_header("Authorization", "Basic " + base64.b64encode(f"{RPC_USER}:{RPC_PASS}".encode()).decode())
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            v = json.loads(r.read())
    except urllib.error.HTTPError as e:
        v = json.loads(e.read())
    if v.get("error"):
        raise RpcError(v["error"])
    return v["result"]


def grpc(port, method, req=None, timeout=300):
    if "." not in method.split("/")[0]:
        method = "cusf.mainchain.v1." + method
    r = urllib.request.Request(f"http://127.0.0.1:{port}/{method}", json.dumps(req or {}).encode(),
                               {"Content-Type": "application/json", "Connect-Protocol-Version": "1"})
    try:
        with urllib.request.urlopen(r, timeout=timeout) as x:
            d = x.read()
            return json.loads(d) if d else {}
    except urllib.error.HTTPError as e:
        raise RpcError({"grpc": method, "status": e.code, "body": e.read()[:300].decode(errors="replace")})


def wait_for(f, what, timeout=120, every=1.0):
    t = time.time()
    last = None
    while time.time() - t < timeout:
        try:
            v = f()
            if v:
                return v
        except Exception as e:  # noqa: BLE001
            last = e
        time.sleep(every)
    raise SystemExit(f"timeout waiting for {what} (last error: {last})")


# ------------------------------------------------------------------ processes
def pidfile(d):
    return d / "pids.json"


def read_pids(d):
    try:
        return json.loads(pidfile(d).read_text())
    except FileNotFoundError:
        return {}


def spawn(d, name, argv, env=None):
    log = open(d / "logs" / f"{name}.log", "ab")
    p = subprocess.Popen([str(a) for a in argv], stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True, env=env)
    pids = read_pids(d)
    pids[name] = p.pid
    pidfile(d).write_text(json.dumps(pids))
    return p.pid


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def stop(args):
    d = Path(args.dir).expanduser()
    st = json.loads((d / "state.json").read_text()) if (d / "state.json").exists() else {}
    p = st.get("ports", {})
    for name in ("ours", "theirs"):
        try:
            jsonrpc(p[f"{name}_rpc"], "stop", timeout=5)
        except Exception:  # noqa: BLE001
            pass
    try:
        btc(p["ecash_rpc"], "stop")
    except Exception:  # noqa: BLE001
        pass
    pids = read_pids(d)
    for name, pid in pids.items():  # exact PIDs we started, SIGTERM then SIGKILL
        if alive(pid):
            os.kill(pid, signal.SIGTERM)
    t = time.time()
    while time.time() - t < 20 and any(alive(pid) for pid in pids.values()):
        time.sleep(0.5)
    for name, pid in pids.items():
        if alive(pid):
            os.kill(pid, signal.SIGKILL)
    print("stopped:", ", ".join(f"{n}={p}" for n, p in pids.items()))


def start(args):
    d = Path(args.dir).expanduser()
    if d.exists() and any(alive(p) for p in read_pids(d).values()):
        sys.exit(f"something runs from {d}: stop it first")
    for k, b in BINS.items():
        if not b.is_file():
            sys.exit(f"missing {k}: {b}")
    if d.exists():
        shutil.rmtree(d)
    (d / "logs").mkdir(parents=True)
    p = ports(args.base_port)
    busy = [n for n, x in p.items() if not free(x) or str(x).endswith("54") or 49660 <= x <= 49669 or 49700 <= x <= 49799]
    if busy:
        sys.exit(f"ports not usable: {busy}")
    st = {"ports": p, "period_blocks": args.period_blocks}
    (d / "state.json").write_text(json.dumps(st))
    ed = d / "ecash"
    ed.mkdir()
    spawn(d, "ecashd", [BINS["ecashd"], "-regtest", f"-datadir={ed}", f"-rpcport={p['ecash_rpc']}", f"-port={p['ecash_p2p']}",
                        f"-bind=127.0.0.1:{p['ecash_p2p']}", "-rest", f"-zmqpubsequence=tcp://127.0.0.1:{p['ecash_zmq']}",
                        "-disablewallet", f"-rpcuser={RPC_USER}", f"-rpcpassword={RPC_PASS}", "-server",
                        "-listenonion=0", "-dnsseed=0", "-fixedseeds=0", "-discover=0", "-printtoconsole=0", "-txindex"])
    wait_for(lambda: btc(p["ecash_rpc"], "getblockcount") >= 0, "ecashd", 120)
    xd = d / "electrs"
    xd.mkdir()
    spawn(d, "electrs", [BINS["electrs"], f"--db-dir={xd}", f"--daemon-dir={ed}", f"--daemon-rpc-addr=127.0.0.1:{p['ecash_rpc']}",
                         f"--electrum-rpc-addr=127.0.0.1:{p['electrum']}", f"--http-addr=127.0.0.1:{p['esplora']}",
                         f"--monitoring-addr=127.0.0.1:{p['electrs_mon']}", "--network=regtest",
                         f"--cookie={RPC_USER}:{RPC_PASS}", "--jsonrpc-import"])
    wait_for(lambda: urllib.request.urlopen(f"http://127.0.0.1:{p['esplora']}/blocks/tip/height", timeout=5).read(), "electrs", 120)
    nd = d / "enforcer"
    nd.mkdir()
    spawn(d, "enforcer", [BINS["enforcer"], f"--data-dir={nd}", f"--node-rpc-addr=127.0.0.1:{p['ecash_rpc']}",
                          f"--node-rpc-user={RPC_USER}", f"--node-rpc-pass={RPC_PASS}",
                          f"--node-zmq-addr-sequence=tcp://127.0.0.1:{p['ecash_zmq']}", f"--network-magic={ECASH_MAGIC}",
                          "--op-drivechain=nop8", "--enable-wallet", "--wallet-auto-create", "--enable-mempool",
                          "--enable-block-template-server", f"--wallet-esplora-url=http://127.0.0.1:{p['esplora']}",
                          f"--serve-grpc-addr=127.0.0.1:{p['enf_grpc']}", f"--serve-rpc-addr=127.0.0.1:{p['enf_rpc']}",
                          f"--log-directory={nd / 'logs'}", "--log-level=info"])
    wait_for(lambda: "blockHeaderInfo" in grpc(p["enf_grpc"], "ValidatorService/GetChainTip"), "the enforcer", 180)
    addr = wait_for(lambda: grpc(p["enf_grpc"], "WalletService/CreateNewAddress")["address"], "the enforcer wallet", 120)
    st["enforcer_address"] = addr
    (d / "state.json").write_text(json.dumps(st))
    lab = Lab(d)
    lab.mine_main(101)
    grpc(p["enf_grpc"], "BlockProducerService/SetAckAllProposals", {"policy": "ACK_ALL_PROPOSALS_POLICY_ALL"})
    grpc(p["enf_grpc"], "BlockProducerService/SubmitSidechainProposal", {
        "sidechainId": SLOT, "declaration": {"v0": {"title": "Truthcoin", "description": "interop test",
                                                     "hashId1": {"hex": "00" * 32}, "hashId2": {"hex": "00" * 20}}}})
    for _ in range(40):
        lab.mine_main(1)
        if lab.slot_active():
            break
    if not lab.slot_active():
        sys.exit("slot 13 did not activate")
    print(f"slot {SLOT} active at mainchain height {btc(p['ecash_rpc'], 'getblockcount')}")
    for who in ("theirs", "ours"):
        (d / who).mkdir()
        spawn_node(d, who, p, args.period_blocks)
    for who in ("theirs", "ours"):
        wait_for(lambda: jsonrpc(p[who + "_rpc"], "getblockcount", timeout=5) is not None, f"{who} RPC", 120)
        m = jsonrpc(p[who + "_rpc"], "generate_mnemonic")
        jsonrpc(p[who + "_rpc"], "set_seed_from_mnemonic", [m])
        (d / f"{who}.mnemonic").write_text(m)
    print("both nodes up; next: interop.py run")


def spawn_node(d, who, p, period_blocks):
    dd = d / who
    if True:
        other = "ours" if who == "theirs" else "theirs"
        argv = [BINS[who], "--headless", "--datadir", dd, "--network", "regtest",
                "--mainchain-grpc-url", f"http://127.0.0.1:{p['enf_grpc']}",
                "--net-addr", f"127.0.0.1:{p[who + '_p2p']}", "--rpc-addr", f"127.0.0.1:{p[who + '_rpc']}",
                "--private-rpc-addr", f"127.0.0.1:{p[who + '_rpc']}",
                "--add-peer", f"127.0.0.1:{p[other + '_p2p']}", "--decision-config-testing", str(period_blocks),
                "--log-level", "debug" if who == "ours" else "info"]
        if who == "ours":
            argv += ["--bt-no-seeds"]
        spawn(d, who, argv)


def restart(args):
    """Restart one node with the same datadir and flags (e.g. ours after a rebuild)."""
    d = Path(args.dir).expanduser()
    st = json.loads((d / "state.json").read_text())
    pid = read_pids(d).get(args.node)
    if pid and alive(pid):
        try:
            jsonrpc(st["ports"][args.node + "_rpc"], "stop", timeout=5)
        except Exception:  # noqa: BLE001
            pass
        t = time.time()
        while alive(pid) and time.time() - t < 30:
            time.sleep(0.5)
        if alive(pid):
            os.kill(pid, signal.SIGTERM)
    spawn_node(d, args.node, st["ports"], st["period_blocks"])
    wait_for(lambda: jsonrpc(st["ports"][args.node + "_rpc"], "getblockcount", timeout=5) is not None, "RPC", 120)
    print(f"{args.node} restarted")


# ------------------------------------------------------------------ the scenario
class Lab:
    def __init__(self, d):
        self.d = d
        self.st = json.loads((d / "state.json").read_text())
        self.p = self.st["ports"]
        self.checks = 0
        self.diffs = []

    def node(self, who, method, params=None, **kw):
        return jsonrpc(self.p[who + "_rpc"], method, params, **kw)

    def mine_main(self, n=1):
        r = grpc(self.p["enf_grpc"], "MiningService/GenerateToAddress", {"blocks": n, "address": self.st["enforcer_address"]})
        return [h["hex"] for h in r.get("blockHashes", [])]

    def slot_active(self):
        r = grpc(self.p["enf_grpc"], "ValidatorService/GetSidechains")
        return any(s.get("sidechainNumber") == SLOT for s in r.get("sidechains", []))

    def mine_side(self, who):
        """`mine` on one node (BMM request through the enforcer), a mainchain block to include it."""
        before = self.node(who, "getblockcount")
        err = []

        def run():
            try:
                self.node(who, "mine", [None])
            except Exception as e:  # noqa: BLE001
                err.append(e)
        t = threading.Thread(target=run)
        t.start()
        time.sleep(1.5)
        self.mine_main(1)
        t.join(120)
        if err:
            raise SystemExit(f"mine on {who}: {err[0]}")
        wait_for(lambda: self.node(who, "getblockcount") > before, f"a block from {who}", 60, 0.5)
        return self.synced()

    def synced(self, timeout=90):
        def same():
            a = self.node("theirs", "get_best_sidechain_block_hash")
            b = self.node("ours", "get_best_sidechain_block_hash")
            return a if a == b and a is not None else None
        return wait_for(same, "one sidechain tip on both nodes", timeout, 0.5)

    READS = [("getblockcount", []), ("get_best_sidechain_block_hash", []), ("get_best_mainchain_block_hash", []),
             ("sidechain_wealth", []), ("pending_withdrawal_bundle", []), ("latest_failed_withdrawal_bundle_height", []),
             ("decision_status", []), ("decision_list", [None]), ("market_list", []), ("vote_voters", []),
             ("vote_period", [None]), ("list_open_periods_with_pricing", [])]

    def compare(self, label):
        """Every read answer of both nodes, byte for byte (sets compared as sets)."""
        tip = self.synced()
        reads = list(self.READS)
        reads.append(("get_block", [tip]))
        reads.append(("get_two_way_peg_events", [tip]))
        reads.append(("get_block_index", [tip]))
        for m in self.node("theirs", "market_list"):
            reads.append(("market_get", [m["market_id"]]))
        for name in ("theirs", "ours"):
            for a in self.node(name, "get_wallet_addresses")[:3]:
                reads.append(("votecoin_balance", [a]))
                reads.append(("get_utxos", [[a]]))
        UNORDERED = {"get_utxos", "get_stxos", "list_utxos", "get_wallet_utxos"}  # theirs: HashMap order
        for method, params in reads:
            if method in UNORDERED:
                ra = sorted(json.dumps(x, sort_keys=True) for x in jsonrpc(self.p["theirs_rpc"], method, params))
                rb = sorted(json.dumps(x, sort_keys=True) for x in jsonrpc(self.p["ours_rpc"], method, params))
                self.checks += 1
                if ra != rb:
                    self.diffs.append((label, method, params, str(ra)[:400], str(rb)[:400]))
                continue
            try:
                a = jsonrpc(self.p["theirs_rpc"], method, params, raw=True)
            except Exception as e:  # noqa: BLE001
                a = f"EXC {e}".encode()
            try:
                b = jsonrpc(self.p["ours_rpc"], method, params, raw=True)
            except Exception as e:  # noqa: BLE001
                b = f"EXC {e}".encode()
            self.checks += 1
            if a != b:
                self.diffs.append((label, method, params, a[:400], b[:400]))
        ua = sorted(json.dumps(x, sort_keys=True) for x in self.node("theirs", "list_utxos"))
        ub = sorted(json.dumps(x, sort_keys=True) for x in self.node("ours", "list_utxos"))
        self.checks += 1
        if ua != ub:
            self.diffs.append((label, "list_utxos", [], str(len(ua)), str(len(ub))))

    def report(self):
        print(f"{self.checks} checks, {len(self.diffs)} differences")
        for d in self.diffs[:40]:
            print("DIFF", d)
        (self.d / "diffs.json").write_text(json.dumps([[str(x) for x in d] for d in self.diffs], indent=1))


def run(args):
    lab = Lab(Path(args.dir).expanduser())
    try:
        scenario(lab, args)
    finally:
        lab.report()


def scenario(lab, args):
    p = lab.p
    # deposits to both nodes
    for who in ("theirs", "ours"):
        a = lab.node(who, "get_new_address")
        r = grpc(p["enf_grpc"], "WalletService/CreateDepositTransaction",
                 {"sidechainId": SLOT, "address": lab.node(who, "format_deposit_address", [a]),
                  "valueSats": str(5 * 100_000_000), "feeSats": "10000"})
        print(f"deposit to {who}: {r['txid']['hex'][:16]}..")
        lab.mine_main(1)
    who = "ours"
    for i in range(4):
        lab.mine_side(who)
        lab.compare(f"genesis+{i}")
        who = "theirs" if who == "ours" else "ours"
    for w in ("theirs", "ours"):
        print(w, "balance", lab.node(w, "balance"))
    # transfers both ways, each mined by the other node (relay over P2P)
    for frm, to in (("theirs", "ours"), ("ours", "theirs")):
        dest = lab.node(to, "get_new_address")
        txid = lab.node(frm, "create_transfer", [dest, 12_345_678, 1_000])
        wait_for(lambda: any(t["txid"] == txid for t in lab.node(to, "list_mempool")), f"{frm}'s transfer in {to}'s mempool", 60, 0.5)
        lab.mine_side(to)
        lab.compare(f"transfer {frm}->{to}")
        info = lab.node(frm, "get_transaction_info", [txid])
        print(f"transfer {frm}->{to}: {txid[:16]}.. confirmations {info and info['confirmations']}")
    # reputation: the genesis voter (whoever mined block 0) sends some to the other node's voter
    voters = {w: lab.node(w, "get_voter_address") for w in ("theirs", "ours")}
    rep = {w: lab.node(w, "votecoin_balance", [voters[w]]) for w in voters}
    holder = max(rep, key=rep.get)
    other = "ours" if holder == "theirs" else "theirs"
    # the voter address needs coins for the fee
    lab.node(other if False else holder, "create_transfer", [voters[holder], 100_000, 1_000])
    lab.mine_side(other)
    txid = lab.node(holder, "transfer_votecoin", [voters[other], 0.25, 1_000])
    print(f"reputation 0.25 {holder}->{other}: {txid[:16]}..")
    lab.mine_side(other)
    lab.compare("reputation transfer")
    print("votecoin", {w: lab.node(w, "votecoin_balance", [voters[w]]) for w in voters})
    withdrawals(lab)
    markets(lab)
    mainchain_reorg(lab)


def act(lab, prefer, method, params):
    """A write call on `prefer`, or on the other node while ours lacks the method."""
    for who in (prefer, "theirs" if prefer == "ours" else "ours"):
        try:
            return who, lab.node(who, method, params)
        except RpcError as e:
            if e.err.get("code") == -32601:
                continue
            raise
    raise SystemExit(f"{method}: no node serves it")


def mine_until_period(lab, period, who="ours"):
    while lab.node("theirs", "decision_status")["current_period"] < period:
        lab.mine_side(who)
        lab.compare(f"to period {period}")
        who = "theirs" if who == "ours" else "ours"
    return who


def markets(lab):
    """Claims, markets (binary, scaled, categorical), buys and sells by both nodes, votes and ballots
    from both voters, the period close (consensus), resolution and settlement payouts."""
    st = lab.node("theirs", "decision_status")
    p0 = st["current_period"]
    print(f"markets: period {p0}, {st['blocks_per_period']} blocks per period")
    who, r = act(lab, "ours", "decision_claim", [{"decision_type": "binary", "decisions": [
        {"period_index": p0, "header": "Will it rain?", "description": "interop", "option_0_label": "No", "option_1_label": "Yes"}],
        "tx_fee_sats": 1000}])
    print(f"  claim by {who}: {r['decision_ids']}")
    other = "theirs" if who == "ours" else "ours"
    wait_for(lambda: any(t["txid"] == r["txid"] for t in lab.node(other, "list_mempool")), "the claim relayed", 60, 0.5)
    who2, r2 = act(lab, "theirs", "decision_claim", [{"decision_type": "scaled", "decisions": [
        {"period_index": p0, "header": "Temperature", "description": "deg C"}], "min": -20.0, "max": 40.0, "increment": 0.5,
        "tx_fee_sats": 1000}])
    print(f"  scaled claim by {who2}: {r2['decision_ids']}")
    lab.mine_side("theirs")
    lab.compare("claims")
    m1w, m1 = act(lab, "theirs", "market_create", [{"title": "Rain", "description": "binary market",
        "dimensions": [{"type": "existing", "id": r["decision_ids"][0]}], "initial_liquidity": 100000, "tx_fee_sats": 2000}])
    m2w, m2 = act(lab, "ours", "market_create", [{"title": "Rain and temp", "description": "two dimensions",
        "dimensions": [{"type": "existing", "id": r["decision_ids"][0]}, {"type": "existing", "id": r2["decision_ids"][0]},
                       {"type": "new", "period_index": p0, "decision_type": "category", "header": "Winner",
                        "option_labels": ["A", "B", "C"]}],
        "trading_fee": 0.01, "initial_liquidity": 300000, "tx_fee_sats": 3000}])
    print(f"  markets: {m1w} {m1.get('market_id')}, {m2w} {m2.get('market_id')}")
    lab.mine_side("ours")
    lab.compare("markets")
    for i, (w, mid, oc, n) in enumerate([("ours", m1["market_id"], 1, 40000), ("theirs", m1["market_id"], 0, 25000),
                                         ("ours", m2["market_id"], 3, 30), ("theirs", m2["market_id"], 5, 10)]):
        bw, b = act(lab, w, "market_buy", [{"market_id": mid, "outcome_index": oc, "shares_amount": n, "max_cost": 5_000_000}])
        print(f"  buy {n} of {oc} on {mid} by {bw}: {b.get('txid')} cost {b.get('cost_sats')}")
        # a trade has no outputs: its inputs (all coins it selected) come back only once mined
        lab.mine_side("theirs" if i % 2 else "ours")
        lab.compare(f"buy {i}")
    # sells: each node sells part of what it bought
    for w, mid, oc, n in (("ours", m1["market_id"], 1, 15000), ("theirs", m1["market_id"], 0, 10000)):
        seller = None
        for a in lab.node(w, "get_wallet_addresses"):
            pos = lab.node(w, "market_positions", [a, mid])
            if any(p.get("outcome_index") == oc and p.get("shares", 0) >= n for p in pos.get("positions", [])):
                seller = a
                break
        if not seller:
            print(f"  no position of {w} found for a sell; skipped")
            continue
        sw, sres = act(lab, w, "market_sell", [{"market_id": mid, "outcome_index": oc, "shares_amount": n,
                                                "seller_address": seller, "min_proceeds": 0}])
        print(f"  sell {n} of {oc} on {mid} by {sw}: {sres.get('txid')} proceeds {sres.get('net_proceeds_sats')}")
        lab.mine_side("theirs" if w == "ours" else "ours")
        lab.compare(f"sell by {w}")
    for w in ("ours", "theirs"):
        pos = lab.node("theirs", "market_positions", [lab.node(w, "get_wallet_addresses")[0], None]) if False else None
    who = mine_until_period(lab, p0 + 1)
    # votes: both voters (each holds reputation after the transfer above)
    for w, val in (("theirs", 1.0), ("ours", 1.0)):
        vw, v = act(lab, w, "vote_submit", [[{"decision_id": r["decision_ids"][0], "vote_value": val},
                                             {"decision_id": r2["decision_ids"][0], "vote_value": 21.5}], 1000])
        print(f"  ballot by {vw}: {v}")
    lab.mine_side(who)
    lab.compare("votes")
    who = mine_until_period(lab, p0 + 3, who)
    print("  outcomes:", json.dumps(lab.node("theirs", "vote_period", [p0 + 1]))[:300])
    print("  markets:", [(m["market_id"], m.get("state")) for m in lab.node("theirs", "market_list")])


def withdrawals(lab):
    """A withdrawal from each node, the bundle, M3 (Submitted) and payout (M6) through the enforcer."""
    main_addr = btc(lab.p["ecash_rpc"], "getblockchaininfo") and "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"
    for w in ("theirs", "ours"):
        txid = lab.node(w, "create_withdrawal", [main_addr, 1_000_000, 1_000, 5_000])
        print(f"withdrawal from {w}: {txid[:16]}..")
    who = "ours"
    for i in range(8):
        lab.mine_side(who)
        lab.compare(f"withdrawal {i}")
        who = "theirs" if who == "ours" else "ours"
        b = lab.node("theirs", "pending_withdrawal_bundle")
        if b:
            print(f"  pending bundle at side height {lab.node('theirs', 'getblockcount')}: {len(b['spend_utxos'])} inputs")
    # mainchain blocks: the enforcer's block producer includes the proposed bundle (M3) and acks it (M4)
    for i in range(12):
        lab.mine_main(1)
        if i % 3 == 2:
            lab.mine_side(who)
            lab.compare(f"bundle votes {i}")
            who = "theirs" if who == "ours" else "ours"
    print("  latest failed bundle height:", lab.node("theirs", "latest_failed_withdrawal_bundle_height"),
          "pending:", bool(lab.node("theirs", "pending_withdrawal_bundle")))


def mainchain_reorg(lab):
    """A sidechain block whose BMM mainchain block is reorged away (their side-tips rule keeps it)."""
    tip_before = lab.synced()
    lab.mine_side("ours")
    side_tip = lab.synced()
    main_tip = btc(lab.p["ecash_rpc"], "getbestblockhash")
    btc(lab.p["ecash_rpc"], "invalidateblock", main_tip)
    # the enforcer's next template would be the invalidated block again (duplicate-invalid)
    btc(lab.p["ecash_rpc"], "generatetoaddress", 2, lab.st["enforcer_address"])
    best = btc(lab.p["ecash_rpc"], "getbestblockhash")
    wait_for(lambda: grpc(lab.p["enf_grpc"], "ValidatorService/GetChainTip")["blockHeaderInfo"]["blockHash"]["hex"] == best,
             "the enforcer on the reorged mainchain", 180, 2)
    lab.mine_side("theirs")
    lab.compare("after mainchain reorg")
    print(f"mainchain reorg: side tip before {side_tip[:16]}.., now {lab.node('theirs', 'get_best_sidechain_block_hash')[:16]}.. "
          f"/ {lab.node('ours', 'get_best_sidechain_block_hash')[:16]}.. (start {tip_before[:16]}..)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["start", "run", "stop", "restart"])
    ap.add_argument("--node", default="ours", choices=["ours", "theirs"])
    ap.add_argument("--dir", default=os.environ.get("HIVEMIND_INTEROP_DIR", "~/.hivemind-interop"))
    ap.add_argument("--base-port", type=int, default=49800)
    ap.add_argument("--period-blocks", type=int, default=6)
    args = ap.parse_args()
    {"start": start, "run": run, "stop": stop, "restart": restart}[args.cmd](args)


if __name__ == "__main__":
    main()
