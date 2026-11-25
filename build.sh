#!/bin/sh
# Builds dmd5620 (SDL front end + Rust dmd_core) on macOS or Linux.
# The Rust toolchain and cargo cache live under ./build -- nothing is
# installed outside this folder. Wipe it all with: rm -rf build
#
#   ./build.sh          build ./dmd5620
#   ./build.sh clean    remove objects and the core library, then build
set -e
cd "$(dirname "$0")"

B=$PWD/build
export RUSTUP_HOME="$B/rustup" CARGO_HOME="$B/cargo"
export PATH="$CARGO_HOME/bin:$PATH"

# --- contained rust toolchain -------------------------------------------------
if [ ! -x "$CARGO_HOME/bin/cargo" ]; then
  echo "==> installing rustup into $B (nothing outside this dir is touched)"
  mkdir -p "$B"
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
    | sh -s -- -y --no-modify-path --profile minimal --default-toolchain stable
fi

[ -f dmd_core/Cargo.toml ] || git submodule update --init

if [ "$1" = clean ]; then
  make clean
  rm -rf dmd_core/target
fi

make CARGO="$CARGO_HOME/bin/cargo"
xattr -d com.apple.quarantine dmd5620 2>/dev/null || true
./dmd5620 -v
