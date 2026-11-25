#!/bin/sh
# Build the macOS release: one universal (arm64 + x86_64) binary with SDL2
# linked in statically. Work files go to build/.
#
#   tools/release-macos.sh            build build/dist/dmd5620-VERSION-macos.zip
#
# With DEV_ID (a "Developer ID Application: ..." identity in the keychain)
# and NOTARY_PROFILE (an `xcrun notarytool store-credentials` profile) set,
# it also signs the binary and has Apple notarize it.
set -e
cd "$(dirname "$0")/.."
SDL_VER=2.32.10
SDL_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
export MACOSX_DEPLOYMENT_TARGET=10.13
ARCHS="-arch arm64 -arch x86_64"
VERSION=$(awk '/VERSION_(MAJOR|MINOR|BUILD)/ {v = v (v ? "." : "") $3} END {print v}' src/version.h)
B=$PWD/build

# SDL2, static and universal
if [ ! -f "$B/sdl2-macos/lib/libSDL2.a" ]; then
    mkdir -p "$B/dl"
    tgz=$B/dl/SDL2-$SDL_VER.tar.gz
    [ -f "$tgz" ] || curl -fsSL -o "$tgz" \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz"
    echo "$SDL_SHA256  $tgz" | shasum -a 256 -c -
    rm -rf "$B/sdl2-src" && mkdir -p "$B/sdl2-src"
    tar xzf "$tgz" -C "$B/sdl2-src" --strip-components 1
    cmake -S "$B/sdl2-src" -B "$B/sdl2-build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
        -DCMAKE_INSTALL_PREFIX="$B/sdl2-macos" >/dev/null
    cmake --build "$B/sdl2-build" -j "$(sysctl -n hw.ncpu)" >/dev/null
    cmake --install "$B/sdl2-build" >/dev/null
fi

make clean >/dev/null
make -j"$(sysctl -n hw.ncpu)" WERROR=1 STATIC=1 OBJDIR="$B/obj-macos-universal" \
    SDL_CONFIG="$B/sdl2-macos/bin/sdl2-config" \
    CFLAGS="-O2 $ARCHS" LDFLAGS="$ARCHS"
strip -x dmd5620
lipo -info dmd5620
if otool -L dmd5620 | grep "^	" | grep -v -E "/System/Library/|/usr/lib/"; then
    echo "not static: the lines above"
    exit 1
fi

if [ -n "$DEV_ID" ]; then
    codesign --force --options runtime --timestamp -s "$DEV_ID" dmd5620
    codesign --verify --strict dmd5620
fi

D=$B/dist/dmd5620-$VERSION-macos
rm -rf "$D" "$D.zip" && mkdir -p "$D"
cp dmd5620 README.md MCP.md LICENSE.md "$D/"
cp dmd5620.man "$D/dmd5620.1"
(cd "$B/dist" && ditto -c -k --keepParent "dmd5620-$VERSION-macos" "dmd5620-$VERSION-macos.zip")

if [ -n "$DEV_ID" ] && [ -n "$NOTARY_PROFILE" ]; then
    xcrun notarytool submit "$D.zip" --keychain-profile "$NOTARY_PROFILE" --wait
fi
echo "$D.zip"
