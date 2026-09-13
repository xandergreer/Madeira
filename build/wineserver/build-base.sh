#!/bin/bash
# Build the BASE libwineserver.a from scratch.
#
# build.sh only ever *patches* an existing archive: it compiles ~19 iOS-specific
# or fix-carrying files and swaps them into a base archive via `ar r`. That base
# was gitignored and never published, so a clean clone hits
# "ERROR: No base libwineserver.a found" and stops.
#
# This script reconstructs it by compiling every .c in wine/server/SOURCES with
# the same flags build.sh uses, so the objects are ABI- and macro-compatible
# with the ones build.sh later swaps in. Run this once, then run build.sh as
# normal.
#
# ptrace.c / procfs.c / mach.c are all in SOURCES and all compile: each is
# guarded internally (HAVE_SYS_PTRACE_H, USE_PROCFS, HAVE_SYS_SYSCTL_H), so the
# ones that do not apply to iOS produce near-empty objects rather than errors.
set -e

BUILD_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$BUILD_DIR/../.." && pwd)"
WINE_SRC="$REPO_ROOT/wine"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
APP_LIB="$REPO_ROOT/app/Madeira/libwineserver.a"
SHIMS_DIR="$REPO_ROOT/build/ntdll-unix/shims"
OBJ_DIR="$BUILD_DIR/obj"

if [ ! -f "$WINE_SRC/build-macos/include/config.h" ]; then
    echo "ERROR: wine/build-macos/include/config.h missing."
    echo "Configure Wine for the macOS host first, e.g.:"
    echo "  cd wine && mkdir -p build-macos && cd build-macos && \\"
    echo "    PATH=\"/opt/homebrew/opt/bison/bin:\$PATH\" ../configure \\"
    echo "      --enable-win64 --without-x --disable-tests --enable-archs=aarch64,arm64ec"
    exit 1
fi

mkdir -p "$OBJ_DIR"

# Identical to build.sh's CC_FLAGS — keep the two in sync.
CC_FLAGS=(
    -arch arm64 -isysroot "$SDK" -miphoneos-version-min=17.0 -O2
    -I"$WINE_SRC/include" -I"$WINE_SRC/include/wine"
    -I"$WINE_SRC/build-macos/include"
    -I"$BUILD_DIR" -I"$WINE_SRC/server"
    -I"$SHIMS_DIR"
    -include "$BUILD_DIR/config_ios.h"
    -include stdarg.h
    -include "$BUILD_DIR/unicode_fix.h"
    -include "$BUILD_DIR/wineserver_ios_kill.h"
    -DBINDIR=\"/usr/local/bin\" -DDATADIR=\"/usr/local/share\"
    -D__WINESRC__ -DWINE_IOS=1
    -Dmain=wineserver_main
    -Wno-implicit-function-declaration
)

# Pull the .c list straight from server/Makefile.in so it tracks the submodule
# instead of a hardcoded copy that silently rots.
SOURCES=$(sed -n '/^SOURCES = /,/^$/p' "$WINE_SRC/server/Makefile.in" \
    | tr -d '\\\\' | sed 's/^SOURCES = //' | tr ' \t' '\n' \
    | grep '\.c$' | sort -u)

echo "=== Building base libwineserver.a ($(echo "$SOURCES" | wc -l | tr -d ' ') files) ==="

SUCCEEDED=0
FAILED=0
FAILED_FILES=""

for src in $SOURCES; do
    name="${src%.c}"
    printf "  %-16s " "$name"
    if xcrun -sdk iphoneos clang "${CC_FLAGS[@]}" \
        -c "$WINE_SRC/server/$src" -o "$OBJ_DIR/$name.o" 2>"$OBJ_DIR/base-err-$name.txt"; then
        echo "OK"
        SUCCEEDED=$((SUCCEEDED + 1))
    else
        echo "FAILED"
        FAILED=$((FAILED + 1))
        FAILED_FILES="$FAILED_FILES $name"
    fi
done

echo ""
echo "Results: $SUCCEEDED succeeded, $FAILED failed"
if [ -n "$FAILED_FILES" ]; then
    echo "Failed:$FAILED_FILES"
    echo "(errors in $OBJ_DIR/base-err-<name>.txt)"
    exit 1
fi

echo ""
echo "=== Archiving ==="
rm -f "$OBJ_DIR/libwineserver.a"
ar rcs "$OBJ_DIR/libwineserver.a" $(for src in $SOURCES; do echo "$OBJ_DIR/${src%.c}.o"; done)

cp "$OBJ_DIR/libwineserver.a" "$APP_LIB"
echo "libwineserver.a: $(wc -c < "$APP_LIB" | tr -d ' ') bytes"
echo ""
echo "Base archive written. Now run ./build.sh to apply the iOS patches."
