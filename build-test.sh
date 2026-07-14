#!/bin/bash
# Build vpu_vpp_test plugin for iPod 6G and deploy to ipod-re/test
# Usage: ./build-test.sh [commit_msg]
set -e
cd "$(dirname "$0")"

# Toolchain: use local copy under Source (bypasses macOS TCC on ~/rockbox-toolchain)
TC="${HOME}/Source/rbtc"
if [ ! -d "$TC/bin" ]; then
    echo "ERROR: toolchain not found at $TC"
    echo "Run: cp -a ~/rockbox-toolchain ~/Source/rbtc"
    exit 1
fi
export PATH="$TC/bin:$PATH"

BUILDDIR="build-hw-ipod6g"
ROCK="$BUILDDIR/apps/plugins/vpu_vpp_test.rock"
DEST="../ipod-re/test/vpu_vpp_test.rock"

# Configure if needed
if [ ! -f "$BUILDDIR/Makefile" ]; then
    echo "Configuring..."
    mkdir -p "$BUILDDIR"
    cd "$BUILDDIR"
    ../tools/configure --target=ipod6g --type=n
    cd ..
fi

# Build (override CC to use accessible toolchain)
echo "Building..."
cd "$BUILDDIR"
rm -f apps/plugins/vpu_vpp_test.o apps/plugins/vpu_vpp_test.elf apps/plugins/vpu_vpp_test.rock
make CC="$TC/bin/arm-elf-eabi-gcc" \
     LD="$TC/bin/arm-elf-eabi-ld" \
     AR="$TC/bin/arm-elf-eabi-ar" \
     AS="$TC/bin/arm-elf-eabi-as" \
     OC="$TC/bin/arm-elf-eabi-objcopy" \
     -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd ..

# Copy rock to ipod-re
cp "$ROCK" "$DEST"
echo "Deployed: $(ls -la "$DEST")"

# Commit + push if message provided
GIT_AUTHOR="Nux Li <44252050+nuxcodes@users.noreply.github.com>"
export GIT_COMMITTER_NAME="Nux Li"
export GIT_COMMITTER_EMAIL="44252050+nuxcodes@users.noreply.github.com"

MSG="${1:-$(grep -m1 '^\s\*\sv[0-9]' apps/plugins/vpu_vpp_test.c | sed 's/.*\(v[0-9]*\)/\1/' | head -1)}"

if [ -n "$MSG" ]; then
    echo "Committing: $MSG"
    git add apps/plugins/vpu_vpp_test.c
    git commit --author="$GIT_AUTHOR" -m "$MSG"
    git push origin h264

    cd ../ipod-re
    git add test/vpu_vpp_test.rock
    git commit --author="$GIT_AUTHOR" -m "$MSG"
    git push origin master
    cd ../rockbox
    echo "Pushed both repos."
else
    echo "No commit message — skipping git. Run: ./build-test.sh 'v32: description'"
fi

echo "Done. Sync ipod-re to iPod and run the plugin."
