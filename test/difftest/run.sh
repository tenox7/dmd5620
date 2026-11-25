#!/bin/sh
# Differential test: the C core against the original Rust dmd_core 0.7.2.
# Needs git, patch, cargo (Rust) and a C compiler. Work files go to
# build/difftest. Set FUZZ (trials) and STEPS (boot steps) for longer runs.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=${WORK:-$root/build/difftest}
rev=ee222b68db8a7adc4d29a9d23c12e2ecb893f88e # dmd_core v0.7.2

mkdir -p "$work"
if [ ! -d "$work/dmd_core" ]; then
    git clone -q https://git.loomcom.com/seth/dmd_core.git "$work/dmd_core.tmp"
    git -C "$work/dmd_core.tmp" checkout -q $rev
    (cd "$work/dmd_core.tmp" && patch -s -p1 < "$here/rust_core.patch")
    mv "$work/dmd_core.tmp" "$work/dmd_core"
fi
(cd "$work/dmd_core" && cargo build --release -q)

case $(uname -s) in
Darwin) libs="-liconv -lm" ;;
*) libs="-lm -lpthread -ldl" ;;
esac
${CC:-cc} -std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -O2 -Wall \
    -o "$work/difftest" "$here/difftest.c" \
    "$root/core/cpu.c" "$root/core/bus.c" "$root/core/duart.c" \
    "$root/core/dmd.c" "$root/core/rom.c" \
    "$work/dmd_core/target/release/libdmd_core.a" $libs

"$work/difftest" fuzz "${FUZZ:-20000}" 1
"$work/difftest" boot 1 "${STEPS:-20000000}" 1 72
"$work/difftest" boot 2 "${STEPS:-20000000}" 2 100
