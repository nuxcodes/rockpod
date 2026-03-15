#!/bin/sh
cd "$(dirname "$0")"

TARGET="${1:-ipod6g}"

case "$TARGET" in
    ipod6g|6g)  TARGET=ipod6g ;;
    ipodvideo|5g) TARGET=ipodvideo ;;
    *)
        echo "Usage: $0 [ipod6g|6g|ipodvideo|5g]"
        echo "  ipod6g / 6g      iPod Classic 6G/7G (default)"
        echo "  ipodvideo / 5g   iPod Video 5G/5.5G"
        exit 1
        ;;
esac

BUILDDIR="build-hw-${TARGET}"

rm -rf "$BUILDDIR"
mkdir "$BUILDDIR"
cd "$BUILDDIR"
../tools/configure --target="$TARGET" --type=n
make -j$(sysctl -n hw.ncpu)
make zip
