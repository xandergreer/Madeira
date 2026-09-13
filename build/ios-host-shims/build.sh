#!/bin/bash
# Compile the iOS host shims and merge them into libFEXCore.a.
#
# Merging into an archive the Xcode project ALREADY links means no pbxproj
# change is needed -- the same trick build/win32u-unix/build.sh uses to fold
# libfreetype.a into libwin32u_unix.a.
#
# NOTE: rebuilding FEX regenerates libFEXCore.a and drops these objects. Re-run
# this script after any FEX rebuild.
set -e

BUILD_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$BUILD_DIR/../.." && pwd)"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
FEXCORE_LIB="$REPO_ROOT/FEX/build-ios/FEXCore/Source/libFEXCore.a"
OBJ_DIR="$BUILD_DIR/obj"

[ -f "$FEXCORE_LIB" ] || { echo "ERROR: build FEX first ($FEXCORE_LIB missing)"; exit 1; }

mkdir -p "$OBJ_DIR"

echo -n "  ios_host_shims... "
xcrun -sdk iphoneos clang -arch arm64 -isysroot "$SDK" -miphoneos-version-min=17.0 \
    -O2 -fPIC -c "$BUILD_DIR/ios_host_shims.c" -o "$OBJ_DIR/ios_host_shims.o"
echo "OK"

echo -n "  merging into libFEXCore.a... "
ar d "$FEXCORE_LIB" ios_host_shims.o 2>/dev/null || true
ar r "$FEXCORE_LIB" "$OBJ_DIR/ios_host_shims.o"
echo "OK"

echo "libFEXCore.a: $(wc -c < "$FEXCORE_LIB" | tr -d ' ') bytes"
