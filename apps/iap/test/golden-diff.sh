#!/bin/sh
# Compare the legacy (non-IDPS) accessory wire format between a git
# revision and the working tree.
#
#     ./golden-diff.sh v5.5
#     ./golden-diff.sh v5.5 ipodvideo
#
# Prints a unified diff of every byte a legacy accessory would receive.
# An empty diff means nothing changed for accessories that never enable
# transaction IDs, which is the bulk of what exists and all of what
# cannot be tested here.
#
# Doing this by hand is error prone in two specific ways, both of which
# have already happened: a stray `cd` pointing git at the wrong place,
# and stale objects surviving a source swap because make only looks at
# timestamps. This script keeps the swap inside one directory and cleans
# between builds.

set -e

REV="${1:-v5.5}"
TGT="${2:-ipod6g}"
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)

# Sources the fixes touch. Add to this list when a fix touches another.
FILES="apps/iap/iap-core.c
apps/iap/iap-core.h
apps/iap/iap-lingo0.c
apps/iap/iap-lingo1.c
apps/iap/iap-lingo2.c
apps/iap/iap-lingo3.c
apps/iap/iap-lingo4.c"

# The RF Tuner lingo only exists on ipodvideo, and so does the tuner
# driver that shares the volume scale with it.
if [ "$TGT" = ipodvideo ]; then
    FILES="$FILES
apps/iap/iap-lingo7.c
firmware/drivers/tuner/ipod_remote_tuner.c"
fi

cd "$ROOT"

if ! git rev-parse --verify --quiet "$REV" >/dev/null; then
    echo "golden-diff: no such revision: $REV" >&2
    exit 2
fi

SAVE=$(mktemp -d)
OUT=$(mktemp -d)
trap 'for f in $FILES; do [ -f "$SAVE/$(basename "$f")" ] && cp "$SAVE/$(basename "$f")" "$ROOT/$f"; done; rm -rf "$SAVE" "$OUT"' EXIT INT TERM

for f in $FILES; do
    cp "$ROOT/$f" "$SAVE/$(basename "$f")"
done

# A build failure here used to leave an empty file, and two empty files
# diff clean -- so a broken build reported "identical", which is the
# worst possible way for this tool to fail. Check the output is real.
build_and_run() {
    label="$1"
    out="$2"

    if ! ( cd "$HERE" && make clean >/dev/null 2>&1 && make TARGET="$TGT" dump >/dev/null 2>&1 ); then
        echo "golden-diff: the dump tool failed to build for $label" >&2
        ( cd "$HERE" && make TARGET="$TGT" dump 2>&1 | tail -20 ) >&2
        exit 3
    fi

    "$HERE/dump_legacy-$TGT" > "$out"

    if ! grep -q '^# end' "$out"; then
        echo "golden-diff: the dump for $label did not run to completion" >&2
        tail -5 "$out" >&2
        exit 3
    fi
}

# Working tree first, while the sources are still untouched.
build_and_run "the working tree" "$OUT/after.txt"

missing=
for f in $FILES; do
    if git cat-file -e "$REV:$f" 2>/dev/null; then
        git show "$REV:$f" > "$ROOT/$f"
    else
        # The file did not exist at $REV. Silently keeping the current
        # one made the tool diff the tree against itself and report
        # "identical" -- the worst way for a no-regression check to fail.
        missing="$missing $f"
    fi
done

if [ -n "$missing" ]; then
    echo "golden-diff: these sources do not exist at $REV, so the" >&2
    echo "comparison would be against the current tree and meaningless:" >&2
    for f in $missing; do echo "    $f" >&2; done
    echo "Add them to the exclusion list deliberately, or pick a revision" >&2
    echo "that has them." >&2
    exit 4
fi

build_and_run "$REV" "$OUT/before.txt"

# Restore before diffing so a failure here still leaves a clean tree.
for f in $FILES; do
    cp "$SAVE/$(basename "$f")" "$ROOT/$f"
done

echo "# legacy accessory wire format ($TGT): $REV -> working tree"
if diff -u "$OUT/before.txt" "$OUT/after.txt"; then
    echo "# identical"
fi
