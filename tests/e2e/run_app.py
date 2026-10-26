#!/usr/bin/env python3
# Copyright (c) 2026 The Backbone developers
# Distributed under the MIT software license.
"""End-to-end run of hivemind-app on a local eCash regtest (doc/app.md "Tests"): slot 13, app.js in its page; the runner
is core/tests/e2e/dropin_app.py. The node is truthcoin (LayerTwo's name), every node with --decision-config-testing
(periods of a few blocks). Besides deposit, payments both ways, receipt and a withdrawal paid on eCash: a market opened
on a new decision, shares bought and some sold, Votecoin moved to node B's voter address, the decision voted on, the
market paid out (the Markets page's result bars, the Payouts page).

    run_app.py --bin build-app/chains/hivemind --backboned <backboned> --tmp <dir> --shots <dir> [--base-port 49100]
"""

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "core" / "tests" / "e2e"))
from dropin_app import Chain, main  # noqa: E402

PERIOD_BLOCKS = 8

READS = [("getblockcount", []), ("get_best_sidechain_block_hash", []), ("get_best_mainchain_block_hash", []), ("list_utxos", []),
         ("pending_withdrawal_bundle", []), ("latest_failed_withdrawal_bundle_height", []), ("sidechain_wealth", []), ("balance", []),
         ("get_wallet_addresses", []), ("get_wallet_utxos", []), ("openapi_schema", []), ("list_peers", []), ("list_mempool", []),
         ("mainchain_sync_progress", []), ("decision_status", []), ("decision_list", [None]), ("list_open_periods_with_pricing", []),
         ("market_list", []), ("vote_voters", []), ("get_voter_address", [])]


def more_reads(a, addrs):
    reads = [("votecoin_balance", [x]) for x in addrs[:3]] + [("vote_voter", [x]) for x in addrs[:2]]
    for m in a.market_list()[:3]:
        reads += [("market_get", [m["market_id"]]), ("market_price_history", [m["market_id"]])]
        reads += [("market_positions", [x, m["market_id"]]) for x in addrs[:2]]
    for d in a.decision_list(None)[:4]:
        reads += [("decision_get", [d["decision_id_hex"]])]
    current = a.decision_status()["current_period"]
    reads += [("vote_period", [p]) for p in range(max(1, current - 3), current + 1)]
    reads += [("vote_list", [{"period_id": p}]) for p in range(max(1, current - 3), current + 1)]
    return reads


if __name__ == "__main__":
    sys.exit(main(Chain(stem="hivemind", title="Hivemind", slot=13, program="truthcoin", datadir_flag="--datadir", script=HERE / "app.js",
                        reads=READS, node_args=["--decision-config-testing", str(PERIOD_BLOCKS)], more_reads=more_reads,
                        e2e_extra=lambda b: {"peerVoter": b.get_voter_address()})))
