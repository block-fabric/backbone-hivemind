# backbone-hivemind

Drop-in replacement client for **Truthcoin / Hivemind on eCash (slot 13)**: consensus-, P2P-,
mainchain- and RPC-compatible with LayerTwo Labs' truthcoin-dc v0.20.0 (`truthcoin`,
`truthcoin-cli`), which is what betanet runs. C++20 on the shared engine `backbone-core`;
the consensus floating point (LMSR, SVD consensus, rounding) is THEIR `lib/math`, compiled
unmodified behind a small C ABI.

* `doc/hivemind.md`: status, evidence, drop-in surface, reproduced quirks (incl. their bugs),
  kept fixes, consensus-change proposals, RPC mapping for the CHN Hivemind pack.
* `tests/oracle/`: their crates as an oracle (codec vectors, betanet replay, random chains).
* `tests/regtest/interop.py`: their node and ours on one local eCash regtest chain.

Inspired by Hivemind by Paul Sztorc and Truthcoin by LayerTwo Labs (MIT; see
`third_party/truthcoin-dc/LICENSE.txt`).

## Layout

```
core/                     git submodule ../backbone-core (branch dropin-hivemind)
rust/hivemind_math_ffi/   staticlib: their lib/math + tx_pow.rs + decision.rs (third_party/,
                          SHA256SUMS), dependency versions = their Cargo.lock, toolchain 1.97
chains/hivemind/          libhivemind_core (types, codecs, state machine, settlement, 2WPD,
                          mempool, JSON); libhivemind_node (node, side tips, wallet, app, RPC);
                          truthcoin, truthcoin-cli, truthcoin-fetch-blocks, truthcoin-replay
tests/                    unit tests, tests/data/betanet (public data + their digests),
                          oracle/ (Rust harness on their crates), regtest/ (interop)
```

## Build (Linux)

```
git submodule update --init
core/depends/fetch.sh && cmake -S core/depends -B core/depends/work/linux && cmake --build core/depends/work/linux -j4
rust/fetch-vendor.sh      # the pinned crates into rust/vendor, checked against Cargo.lock and VENDOR.sha256
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo     # needs cargo/rustup 1.97 for lib/math
cmake --build build -j4   # cargo build --release --locked --offline runs as part of it
build/tests/bt_tests
```

`rust/hivemind_math_ffi/Cargo.lock` pins every crate to truthcoin-dc v0.20.0's versions, with
crates.io sha256 checksums; `rust/fetch-vendor.sh` vendors them and checks the whole tree
against `rust/VENDOR.sha256`. The vendored sources (62 MB) are not committed.

## The app (hivemind-app)

Our sidechain app on this node: the sidechain template's window, trusted dialog and interface kit, with pages for
Hivemind's data model (markets and trading, creating decisions and markets, the voting center and Votecoin, the
decisions and periods, payouts, paid-out markets with their result at a glance); the node (`truthcoin`) runs as the
app's child, unchanged (consensus and the LayerTwo RPC byte for byte), the app talking to its separate, authenticated
app RPC. The same pages are a pack for the unified wallet. See `doc/app.md` and `core/doc/app.md`.

```
cmake -S . -B build-app -DBT_BUILD_APP=ON -DBT_KIT_DIR=<sidechain-template repo>   # GTK 3, WebKitGTK 4.1, npm, cargo
~/skydoge/tools/build.sh ~/skydoge/backbone-hivemind-app build-app
build-app/chains/hivemind/hivemind-app
```

## Run on betanet

```
truthcoin --headless                                                      # local enforcer :50051
truthcoin --headless --mainchain-grpc-url https://enforcer.beta.ecash.ninja  # public enforcer (TLS)
truthcoin-cli getblockcount                                               # default --rpc-url http://localhost:6013
```

Datadir `~/.local/share/truthcoin_dc` (theirs), our files `backbone-truthcoin.sqlite` and
`backbone-truthcoin-wallet.sqlite`; import their wallet with `--mnemonic-seed-phrase-path` or
`set_seed_from_mnemonic`.
