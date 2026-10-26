#!/bin/sh
# Fill rust/vendor with the exact crates of hivemind_math_ffi/Cargo.lock and check them.
#  * `cargo vendor --locked` downloads each crate from crates.io and verifies it against the
#    sha256 recorded in Cargo.lock (nalgebra 0.33.3, matrixmultiply 0.3.10, ndarray 0.15.6,
#    libm 0.2.16, ... = truthcoin-dc v0.20.0's versions);
#  * the whole tree is then hashed (sorted list of per-file sha256) and compared with
#    VENDOR.sha256, so the build input is pinned bit for bit.
# The build itself is `cargo build --release --locked --offline` (CMake does it). The vendored
# sources are not committed.
set -e
cd "$(dirname "$0")"
CARGO=${CARGO:-$(command -v cargo || echo "$HOME/.cargo/bin/cargo")}
if [ ! -d vendor ] || [ "$1" = "--refresh" ]; then
  rm -rf vendor
  (cd hivemind_math_ffi && "$CARGO" vendor --locked --versioned-dirs -q ../vendor >/dev/null)
fi
got=$(cd vendor && find . -type f | LC_ALL=C sort | xargs sha256sum | sha256sum | cut -d' ' -f1)
want=$(cut -d' ' -f1 VENDOR.sha256)
if [ "$got" != "$want" ]; then
  echo "vendor tree hash $got != VENDOR.sha256 $want" >&2
  exit 1
fi
echo "vendor OK ($got)"
