# Truthcoin / Hivemind (eCash slot 13): drop-in client

`truthcoin` replaces LayerTwo Labs' truthcoin-dc v0.20.0 (`aa8b3ea`, tag v0.20.0, the latest
release on 2026-10-22; the betanet chain restarted 2026-10-10, block 0 BMM'd at mainchain
971,693). Consensus, P2P, mainchain link, CLI flags, `truthcoin-cli` and the JSON-RPC surface
follow that tag. Policy: their consensus is reproduced exactly, bugs and float rounding included;
only consensus-neutral fixes are kept; anything that would change consensus is a proposal (below).

## How consensus is kept identical

* **Codecs**: borsh (hashing, txid = blake3(borsh(tx)), signature message = `0x00 || borsh(tx)`),
  bincode (P2P, archive), serde_json (RPC). A NaN f64 cannot be borsh-serialized: their txid
  panics; we refuse the transaction/block.
* **Floats**: the LMSR (prices, costs, treasury, beta), the SVD consensus, the reputation update,
  `safe_math` rounding and the TX-PoW hashes run in THEIR `lib/math` (+ `types/tx_pow.rs`,
  `decision.rs`), vendored unmodified (`third_party/truthcoin-dc`, SHA256SUMS) and built as a
  Rust staticlib with exactly the dependency versions and features of their Cargo.lock
  (nalgebra 0.33.3, matrixmultiply 0.3.10 with its runtime-dispatched kernels, ndarray 0.15.6,
  libm 0.2.16; toolchain 1.97). Their own 141 lib/math tests pass in our build. The remaining
  plain IEEE arithmetic (fees, pricing splits) is C++ in their operation order with
  `-ffp-contract=off`; Rust's `Sum` starting at -0.0, round-half-away and saturating `as` casts
  are reproduced.
* **Iteration orders**: every LMDB table order their consensus depends on (bincode keys:
  u32 LE periods, raw addresses/ids) is a memcmp-ordered table here; LinkedHashMap accumulator
  diffs keep insertion order.

## Evidence

| What | Result |
|---|---|
| Their encoding tests (`encoding_tests.rs`) | all vectors pass (`tests/test_hivemind_types.cpp`) |
| Codec oracle (`hm_oracle vectors`, their crates) | 4,000 random transactions of every TransactionData/outpoint/content kind + 1,000 bodies: borsh, bincode, txid, serde_json, FROST signatures + actor proofs, merkle roots (incl. their error cases) identical; 200 kept as a unit test |
| Betanet blocks | all 651 public blocks (0..650, `seed.beta.ecash.eu.com:4013`) re-encode byte-identical; every BMM commitment checked against the public enforcer's `GetTwoWayPegData` |
| Betanet replay vs THEIR State | 651/651 heights: identical digest of tip, utreexo roots, UTXO set, reputation, decisions, histories, period pricing, markets, market funds, share accounts, votes, outcomes, period stats, timestamps (`hm_oracle replay`; unit test `hm_betanet_replay_matches_their_state`). Betanet holds 15 txs (8 CreateMarket, 7 Trade), 13 deposits, two resolved voting periods |
| Random chains built by THEIR wallet/state (`hm_oracle simulate`) | 448 seeds × 200–300 blocks (3–6 blocks per period): every block, disconnect (reorgs of 1–3 blocks), 2WPD window (deposits, bundle submitted/confirmed/failed), corrupted block and transaction verdict compared; claims of every type, markets (binary/multi/categorical, TX-PoW), buys, sells, amplify, votes, ballots, reputation transfers, withdrawals, consensus, settlement. **0 mismatches**; reject texts identical where compared |
| Live betanet | from an empty datadir via the public enforcer (`https://enforcer.beta.ecash.ninja`) and the public seed: synced to the live tip 651 `7c735e18…` (= the seed's tip, BMM main `…e7589a`) |
| Their node vs ours on regtest (`tests/regtest/interop.py`) | 1,004 RPC checks, 0 differences: blocks mined alternately by each node are accepted by the other; transfers relayed both ways; reputation transfer; withdrawals into a bundle (M3); a mainchain reorg under a BMM block; claims, three markets, buys, ballots, period close, resolution and settlement; every read RPC compared byte for byte after every block |

## Drop-in surface

* **CLI** (app/cli.rs): `--add-peer`, `--datadir` (`<data_dir>/truthcoin_dc`),
  `--decision-config-testing <N>`, `--headless`, `--log-dir` (`<datadir>/logs/v0.20.0`),
  `--log-level`, `--log-level-file`, `--mainchain-grpc-url` (`http://localhost:50051`; https for
  TLS), `--mnemonic-seed-phrase-path`, `-n/--net-addr` (0.0.0.0:4013), `--network`,
  `--network-magic`, `-p/--private-rpc-addr`, `-r/--rpc-addr` (both 127.0.0.1:6013),
  `--server-name`, `--spend-zero-conf-change <bool>`, `--wallet-dir`. Ours: `--bt-*`.
* **JSON-RPC**: all 81 methods of rpc-api/lib.rs (node, private node, wallet, decisions,
  markets, votes), their parameter names, serde field order, ryu floats, `ba-svk1…`/`ba-enc1…`
  keys, `0x` signatures; errors -1 with their texts, -32602 for bad params; `openapi_schema`
  serves their three utoipa documents byte for byte.
* **truthcoin-cli**: all 77 subcommands, flags, help texts and output formats of theirs.
* **P2P**: QUIC, magic `85 18 95 04` (shared with Thunder), port 4013, heartbeat version 0.20.0.
* **Wallet**: their derivation m/43'/1899'/purpose'/13'/0'/i (0 tx keys, 1 encryption,
  2 message signing); address index 0 = voter address; their coin selection and builders.

## Reproduced quirks and their bugs (consensus: kept on purpose)

1. A known upstream consensus behaviour at period close is reproduced exactly (a chain-liveness
   issue); details are held for coordinated disclosure.
2. CreateMarket new claims validated through the decisions DB interface without timestamp,
   height or genesis (PR#47): the claim's period is checked against period 0.
3. Their block templates can carry transactions that conflict only at connect (details held
   for coordinated disclosure). Ours dry-runs templates: kept fix.
4. A known upstream block-template behaviour is reproduced where it is consensus; details held for
   coordinated disclosure.
5. A known upstream limit in decision ids is reproduced exactly (consensus); details held for
   coordinated disclosure.
6. Some invalid blocks make their node stop where ours rejects them (same verdict: the block
   never connects); details held for coordinated disclosure.
7. Mainchain reorgs: their side-tips rule keeps sidechain blocks whose BMM block was reorged
   away (ported from the BitNames drop-in; `REPRODUCE_SIDE_TIPS_HALT` switch, default off).
8. A known upstream mempool behaviour; ours applies a stricter, consensus-neutral mempool policy.
   Details held for coordinated disclosure.

## Kept fixes (consensus-neutral)

* Template dry-run (prevalidate + connect) with fallback.
* Mempool: every check before any write (their LMDB transaction is dropped on error); not
  persisted across restarts (theirs is); a reorg that gives up restores it.
* P2P hardening of the core template: bans enforced at connect and on every request, unknown
  connections fail closed, per-IP GetHeaders budget, bounded header walk, one body chain per
  peer that ends without progress, stricter mempool checks.
* RPC: lists theirs return in HashMap order come back sorted; `market_buy`/`market_sell` with an
  out-of-range outcome answer an error (theirs panics).

## Consensus-change proposals (NOT applied)

* **P1**: held for coordinated disclosure (see item 1 above).
* **P2**: validate CreateMarket new claims with the real period context (fixes PR#47 bug).

## CHN Hivemind pack mapping (for the unified wallet; not built)

The CHN pack (`~/skydoge/hivemind`, Bitcoin-Core RPC) would drive this node as:

| CHN pack call | truthcoin JSON-RPC |
|---|---|
| gethivemindinfo | decision_status + list_open_periods_with_pricing + getblockcount |
| listdecisions / getdecision | decision_list (filter) / decision_get |
| listmarkets / getmarket | market_list / market_get (+ market_price_history) |
| quotetrade | market_buy / market_sell with `dry_run: true` |
| listreputation / getwalletreputation | vote_voters, votecoin_balance(address) |
| listpositions / getwalletpositions | market_positions(address, market_id?) |
| getballot | vote_list(filter) / vote_voter(address) |
| createdecision | decision_claim |
| createmarket | market_create (dimensions: existing/new) |
| buyshares / sellshares | market_buy / market_sell (or create_trade + sign_transaction) |
| transfervotecoin | transfer_votecoin |
| sealvote/revealvote, sealballot/revealballot | no commit-reveal in truthcoin-dc: vote_submit (one ballot, revealed at once); the pack must hide the seal step |
| gettransaction (wallet) | get_transaction_info |
| getaddressinfo ismine | get_wallet_addresses |

Units: truthcoin uses sats and f64 reputation ("votecoin") where CHN uses its own amounts;
scaled votes are submitted in decision units and normalized by `vote_submit`.
