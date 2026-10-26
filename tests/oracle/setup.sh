#!/bin/sh
# Build hm_oracle: THEIR crates (truthcoin-dc v0.20.0 = aa8b3ea) with this harness added as
# a workspace member, so every dependency resolves from their Cargo.lock.
#   tests/oracle/setup.sh <truthcoin-dc checkout at v0.20.0> <work dir>
# Then: <work dir>/target/release/hm_oracle vectors <seed> <n> | replay <fixture.jsonl>
set -e
src=$(cd "$1" && pwd); work=$2; here=$(cd "$(dirname "$0")" && pwd)
[ "$(git -C "$src" rev-parse HEAD)" = aa8b3ea9cc397883ed05ec07d9ce2182335bb0f8 ] || { echo "not v0.20.0"; exit 1; }
rm -rf "$work" && mkdir -p "$work"
git -C "$src" archive --format=tar HEAD | tar -x -C "$work"
mkdir "$work/bip300301_enforcer" && git -C "$src/bip300301_enforcer" archive --format=tar HEAD | tar -x -C "$work/bip300301_enforcer"
sed -i 's/members = \["app", "cli", "integration_tests", "lib", "rpc-api", "types"\]/members = ["app", "cli", "integration_tests", "lib", "rpc-api", "types", "oracle", "openapi_dump"]/' "$work/Cargo.toml"
mkdir -p "$work/oracle" && cp -r "$here/Cargo.toml" "$here/src" "$work/oracle/"
cp -r "$here/openapi_dump" "$work/"
cd "$work" && cargo +1.97 build --release -j4 -p hm_oracle
