# hivemind-app: our app on the Hivemind drop-in

`hivemind-app` is Hivemind (Truthcoin) with our sidechain app: the sidechain template's window, trusted dialog and
interface kit, with pages for Hivemind's own data model (decisions in slots of periods, LMSR markets on them, shares
held by address, Votecoin and ballots, the settlement that pays the markets out, withdrawal bundles, eCash as the
mainchain). The node is LayerTwo's `truthcoin` (this repository's drop-in), next to the app, unchanged: the app starts
it as its child and talks to its app RPC. Consensus, P2P and the LayerTwo JSON-RPC (truthcoin-dc v0.20.0, 81 methods)
are the drop-in's own and stay byte for byte what they are; the app layer is off unless the node is started with
`--bt-app-rpc-port`.

The design shared by every drop-in app (why a thin client, the app RPC, the privileges, one backup phrase by BIP-85,
the pack, security, tests) is [core/doc/app.md](../core/doc/app.md). This page is Hivemind's part.

## Build and run

```
cmake -S . -B build-app -DBT_BUILD_APP=ON -DBT_KIT_DIR=<sidechain-template repo>   # GTK 3 + WebKitGTK 4.1, npm, cargo 1.97
~/skydoge/tools/build.sh ~/skydoge/backbone-hivemind-app build-app                  # or cmake --build build-app -j4
build-app/chains/hivemind/hivemind-app                  # betanet, the enforcer at http://localhost:50051
build-app/chains/hivemind/hivemind-app --network regtest --mainchain-grpc-url http://127.0.0.1:50051 \
    --node-arg=--decision-config-testing --node-arg=8     # periods of 8 blocks (a test network)
cmake --build build-app --target hivemind-app-pack      # build-app/pack/hivemind: the pack for the unified wallet
```

The app is `hivemind-app` (the stem `hivemind`, the unified wallet's catalog id); the node it runs is `truthcoin`, next
to it (`app/manifest.json`: `"program": "truthcoin"`), with LayerTwo's data folder (`<data_dir>/truthcoin_dc`:
`"datadirName"`), so an existing truthcoin wallet is the app's. `app/kit.commit` pins the sidechain template (branch
`dropin-kit`). The app's options are Thunder's (`--network`, `--datadir`, `--mainchain-grpc-url`, `--rpc-port`,
`--p2p-port`, `--app-rpc-port`, `--node-arg=<arg>`), also in Settings → Node.

## One backup phrase

The wallet is truthcoin's (BIP-39, keys at m/43'/1899'/purpose'/13'/0'/i; address index 0 is the voter address that
holds Votecoin and casts ballots), all of it from the seed: the unified wallet's BIP-85 phrase (core/doc/app.md §3,
stem `hivemind`) covers it, `getwalletphraseinfo`/`setwalletphrase` and `dropin.seed.*` work as for Thunder. Vectors
(master "abandon ×11 about"), checked against an independent implementation:

| stem, network | index | phrase | fingerprint |
|---|---|---|---|
| hivemind, betanet | 1777411230 | essence lesson earth fever power inner edge only transfer almost hint upon | 02749253da7ef11f |
| hivemind, regtest | 91414691 | fragile evidence confirm dune dizzy end blame infant pencil enrich evil airport | dd6edf265dd6471d |

## The pages

The drop-in pages (`core/app/ui`) in the kit's places: Home (with Hivemind's cards: the wallet's bets, the most traded
live markets, the vote of this period), Overview, Send, Receive, Activity (trades, claims, markets, ballots, Votecoin moves and payouts as
their own kinds), Drivechains, the explorer, the node, Settings → Node and → Wallet. Hivemind's own (`app/chain`):

- **Markets**: every market, by stage (trading, being voted on, partly decided, paid out; filters by kind, a search),
  each as a card with its outcomes as bars (their chance now). A market that **paid out** keeps its bars: the outcome
  that won checked and marked *Won*, the others muted at what they paid, a faint mark at its last price before trading
  closed ("the market called it"), and what it paid this wallet. A market opens in its own view: its outcomes and
  prices, the chart of its prices block by block (`market_price_history`), its decisions, who holds its shares, its
  result once paid out, and the **trade panel**: buy (in ECX to spend or in shares) or sell (from an address that
  holds them), the node's quote (`hivemind.quote`: the same LMSR arithmetic as `market_buy`/`market_sell` dry runs),
  a slippage bound, then `market_buy [{market_id, outcome_index, shares_amount, max_cost}]` or `market_sell
  [{..., seller_address, min_proceeds}]` in the trusted dialog.
- **Create** (a tab of Markets): a market on a new decision listed with it (yes or no, a number on a scale, one of
  several options; its period and the listing fee of the cheapest open slot), or on decisions already listed; its
  title, a category, its liquidity and trading fee (`market_create`); or a decision alone (`decision_claim`). The
  listing fee is capped (`max_listing_fee_sats`) at twice the cheapest slot's: the node refuses a claim that would cost
  more. The question, labels and title are what core's `display_text_problem` takes (the page checks the same).
- **Portfolio**: the wallet's shares by market and outcome, what they are worth now and pay if right; sell from here.
- **Voting center**: the decisions being voted on in this period (one ballot, `vote_submit`, counted as cast: the
  chain has no sealed votes), those coming up, this wallet's votes; **Votecoin** (a tab): what the voter address holds,
  moving it (`transfer_votecoin`, its fee paid from the voter address, the chain's rule), every holder.
- **Decisions**: the questions of every period, their state, and the calendar of the periods with their open slots.
- **Payouts**: every market that paid out, with its result bars and what it paid this wallet (shares, fees as a voter or
  author, liquidity back), as the chain recorded it in the block that settled it.

Text of the chain (titles, questions, labels, answers) is data on the pages (SafeText, never translated); in a trusted
dialog, text someone else chose is shown as `display_text_shown` writes it (a hidden or turning character spelled out).

## What the pages use of the node (app/manifest.json)

- **Reads**: the LayerTwo reads by name (the same handlers and JSON): `balance`, `calculate_initial_liquidity`,
  `decision_get`, `decision_list`, `decision_listing_fee`, `decision_status`, `format_deposit_address`,
  `get_best_mainchain_block_hash`, `get_best_sidechain_block_hash`, `get_block`, `get_block_hash`, `get_bmm_inclusions`,
  `get_stxos`, `get_transaction`, `get_transaction_info`, `get_utxos`, `get_voter_address`, `get_wallet_addresses`,
  `get_wallet_utxos`, `get_withdrawal_bundle`, `getblockcount`, `latest_failed_withdrawal_bundle_height`, `list_mempool`,
  `list_open_periods_with_pricing`, `list_peers`, `list_utxos`, `mainchain_sync_progress`, `market_get`, `market_list`,
  `market_positions`, `market_price_history`, `openapi_schema`, `pending_withdrawal_bundle`, `sidechain_wealth`,
  `vote_list`, `vote_period`, `vote_voter`, `vote_voters`, `votecoin_balance`; and `getmainchaininfo`, `getbalances`,
  `getblockchaininfo`, `getnetworkinfo`, `uptime`.
- **Confirmed** (only through `rpc.confirmed`, asked in the trusted dialog): `create_transfer`, `create_transfer_many`,
  `create_withdrawal`, `create_deposit`, `mine`, `transfer_votecoin [dest, amount, fee_sats]`, `decision_claim
  [request]`, `market_create [request]`, `market_buy [request]`, `market_sell [request]`, `market_amplify_beta
  [request]`, `vote_submit [votes, fee_sats]`. Each describer canonicalises LayerTwo's request (its fields typed: whole
  numbers, finite f64, texts of bounded length that a dialog can show; a dry run is a quote, never a spend), refuses
  before asking what the chain or the wallet would refuse (a slot no longer free, a market that does not trade, an
  outcome it does not have, shares the seller does not hold, a ballot for decisions not being voted on or voted on
  already, Votecoin the voter address does not hold, a price moved past the bound), and sums with `spend_total`.
- **Host**: the shared `dropin.*` and `hivemind.status` (`decision_status`), `hivemind.markets` (`market_list`,
  `market_get`), `hivemind.market` (`market_get`, `market_price_history`), `hivemind.decisions` (`decision_list`,
  `decision_get`), `hivemind.quote` (`market_get`), `hivemind.positions` (`get_wallet_addresses`, `market_positions`),
  `hivemind.votecoin` (`get_wallet_addresses`, `get_voter_address`, `votecoin_balance`), `hivemind.ballots`
  (`get_wallet_addresses`, `vote_list`), `hivemind.payouts` (`get_wallet_addresses`, `market_get`).

Ownership: which positions, votes, Votecoin and payouts are the wallet's is said only to a caller of
`get_wallet_addresses`; the chain's markets and decisions to a reader of the chain, without a `mine`. The tests check
this file equals what the node serves (`AppLayer::manifest`).

## Tests

- `tests/test_hivemind_appui.cpp`: every LayerTwo method's reply byte-identical with and without the app layer (80
  methods on one server, seven kinds of bad parameters each, the reads with good ones), the app RPC's reads equal to
  the LayerTwo results, the phrase made only in a question / refused over another, the unified wallet's phrase (the
  BIP-85 vectors above), wallet data only for its reader, every spend refused before any question (sums that wrap or
  pass 21,000,000 ECX, numbers that are not finite, dry runs, share counts out of range, a hidden or control character
  in a question, title or label), the manifest.
- The interface (`build-app/hivemind-app-ui/src/webui/app`): `npx svelte-check --fail-on-warnings` and `npx vitest
  run src/chain` (`app/chain/hivemind.test.ts`: the spends with LayerTwo's parameters, nothing outside the manifest,
  the stages and results of markets; with the shared `dropin.test.ts`).
- `tests/e2e/run_app.py` (a build with `-DBT_APP_TEST_HOOKS=ON`; xvfb-run; every node with
  `--decision-config-testing 8`): a local eCash regtest, hivemind-app runs node A, a truthcoin node B is the other side:
  the phrase made in the trusted dialog, 10 ECX deposited, 1.5 ECX sent to B, 0.25 ECX received from B, 2 ECX
  withdrawn; the wallet's voter address holds the initial Votecoin (A mined block 0); a market opened on a new yes-or-no
  decision of the next period, 20,000 shares of Yes bought and 5,000 sold back, 0.1 Votecoin moved to B's voter
  address; in the decision's voting period it is voted Yes; the market pays out (Yes won, the wallet paid for its
  15,000 shares, its liquidity back); the withdrawal paid on eCash; the phrase shown and saved (0600); then A's
  LayerTwo replies byte-identical without and with the app layer. Last run: 17 checks passed, 47/47 replies
  byte-identical.

```
python3 tests/e2e/run_app.py --bin build-app/chains/hivemind --backboned <backboned> --tmp <dir> --shots <dir> --base-port 49100
```

## Platforms

Linux (GTK 3, WebKitGTK 4.1): built, unit-tested and run end to end. Windows: the node and the app are meant to
cross-compile as the other drop-in apps do (`-DCMAKE_TOOLCHAIN_FILE=core/cmake/mingw64.cmake -DBT_BUILD_APP=ON
-DWEBVIEW2_INCLUDE_DIR=<the WebView2 SDK headers>`, `HM_RUST_TRIPLE=x86_64-pc-windows-gnu` for lib/math); not yet built
for Windows. macOS: not built.
