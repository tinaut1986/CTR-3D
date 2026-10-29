#!/usr/bin/env bash
# Host parity tests for the Old-3DS CPU work (software GTE, RenderBucket model
# walk): a base git revision vs the working tree, built into one binary each.
#
#   platform/n3ds/tests/perf-parity/run.sh [BASE_REV] [GTE_TRIALS] [RB_SCENES]
#
# BASE_REV defaults to HEAD (i.e. "does my uncommitted change keep the output
# identical"); a0ece35 is the tree before the Old-3DS instance-path work. Needs
# a host gcc with -m32 (the RenderBucket test keeps 32-bit pointers in ints,
# as the game does). Prints the trial counts and exits non-zero on a mismatch.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
base="${1:-HEAD}"
gte_trials="${2:-2000000}"
rb_scenes="${3:-1000}"
work="$(mktemp -d /tmp/ctr3d-perf-parity.XXXXXX)"
trap 'rm -rf "$work"' EXIT

git -C "$root" archive "$base" include game platform | tar -x -C "$work" --one-top-level=base

defs="-DCTR_NATIVE -DBUILD=926 -DCTR_NATIVE_GTE_CAPTURE=1 -fno-short-enums -fno-pie -fno-stack-protector -U_FORTIFY_SOURCE -w"
build_pair() { # $1 = tu source, $2 = out name, $3 = extra flags
	gcc -O2 $3 $defs -I"$work/base/include" -I"$work/base" -I"$here" -c "$here/$1" -o "$work/b.o"
	gcc -O2 $3 $defs -I"$root/include" -I"$root" -I"$here" -c "$here/$1" -o "$work/t.o"
	objcopy --prefix-symbols=base_ "$work/b.o" "$work/$2-base.o"
	objcopy --prefix-symbols=test_ "$work/t.o" "$work/$2-test.o"
}

echo "== GTE: $base vs working tree"
build_pair gte_tu.c gte ""
gcc -O2 -no-pie -w -I"$root/include" -I"$root" "$here/gte_main.c" "$work/gte-base.o" "$work/gte-test.o" -o "$work/gte"
"$work/gte" "$gte_trials" x

echo "== RenderBucket: $base vs working tree"
build_pair rb_tu.c rb "-m32 -DCTR_NATIVE_MODEL_RENDERER=1 -D__3DS__"
gcc -m32 -O2 -fno-pie -no-pie -w -DCTR_NATIVE -DBUILD=926 -fno-short-enums -I"$root/include" -I"$root" "$here/rb_main.c" \
	"$work/rb-base.o" "$work/rb-test.o" -o "$work/rb"
"$work/rb" "$rb_scenes" t
