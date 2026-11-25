#!/bin/sh
# Build the macOS release: one universal (arm64 + x86_64) binary with SDL2
# linked in statically, as a plain zip (command line) and as DMD5620.app in
# a DMG. Both have layers (git submodule dmd-layers), also universal.
# Work files go to build/, the results to build/dist/.
#
#   tools/release-macos.sh
#
# With DEV_ID (a "Developer ID Application: ..." identity in the keychain)
# and NOTARY_PROFILE (an `xcrun notarytool store-credentials` profile) set,
# it also signs everything and has Apple notarize the zip and the DMG.
# Without them in the environment, it reads them from .env (DEV_ID := ...).
set -e
cd "$(dirname "$0")/.."
SDL_VER=2.32.10
SDL_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
export MACOSX_DEPLOYMENT_TARGET=10.13
ARCHS="-arch arm64 -arch x86_64"
VERSION=$(awk '/VERSION_(MAJOR|MINOR|BUILD)/ {v = v (v ? "." : "") $3} END {print v}' src/version.h)
B=$PWD/build
envval() { sed -n "s/^$1 *:\{0,1\}= *//p" .env | tail -1; }
if [ -z "$DEV_ID" ] && [ -f .env ]; then
    DEV_ID=$(envval DEV_ID)
    NOTARY_PROFILE=${NOTARY_PROFILE:-$(envval NOTARY_PROFILE)}
fi

# notarize FILE: send it to Apple and wait; fail unless Apple accepts it.
notarize() {
    out=$(xcrun notarytool submit "$1" --keychain-profile "$NOTARY_PROFILE" --wait 2>&1) || true
    echo "$out" | grep -E "^ *(id|status):" | awk '!seen[$0]++'
    echo "$out" | grep -q "status: Accepted" || { echo "notarization failed: $1"; exit 1; }
}

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

# layers: one build for each CPU, then lipo puts them together in layers/.
[ -f dmd-layers/Makefile ] || git submodule update --init dmd-layers
LCFLAGS="-O -w -std=gnu89 -D_FORTIFY_SOURCE=0 -Wno-return-mismatch"
rm -rf layers
for a in arm64 x86_64; do
    rm -rf "$B/layers-$a"
    make -j1 -C dmd-layers clean >/dev/null 2>&1
    make -j1 -C dmd-layers DMD="$B/layers-$a" EXTRACFLAGS="$LCFLAGS -arch $a" install \
        >"$B/layers-$a.log" 2>&1 || true
    for f in bin/layers bin/jwin bin/xtagent lib/layersys/movelogin; do
        [ -x "$B/layers-$a/$f" ] || { echo "layers ($a): no $f, see $B/layers-$a.log"; exit 1; }
    done
done
make -j1 -C dmd-layers clean >/dev/null 2>&1
cp -R "$B/layers-arm64" layers
rm -rf layers/xt
for f in $(find layers -type f); do
    lipo -info "$f" >/dev/null 2>&1 || continue
    lipo -create "$B/layers-arm64/${f#layers/}" "$B/layers-x86_64/${f#layers/}" -output "$f"
done
lipo -info layers/bin/layers

SIGN="-" TS=""
[ -n "$DEV_ID" ] && SIGN=$DEV_ID TS="--timestamp"
for f in $(find layers -type f ! -name '*.a'); do
    lipo -info "$f" >/dev/null 2>&1 || continue
    codesign --force --options runtime $TS -s "$SIGN" "$f"
done

# The app. It gets its own copy of the binary: signing the bundle signs it again.
APP=$B/dist/DMD5620.app
rm -rf "$APP" && mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp assets/Info.plist "$APP/Contents/"
plutil -replace CFBundleShortVersionString -string "$VERSION" "$APP/Contents/Info.plist"
plutil -replace CFBundleVersion -string "$VERSION" "$APP/Contents/Info.plist"
cp assets/dmd5620.icns "$APP/Contents/Resources/"
cp dmd5620 "$APP/Contents/MacOS/"
cp -R layers "$APP/Contents/Resources/"
codesign --force --options runtime $TS -s "$SIGN" "$APP"
codesign --verify --strict --deep "$APP"

# The command line binary
codesign --force --options runtime $TS -i com.github.tenox7.dmd5620 -s "$SIGN" dmd5620
codesign --verify --strict dmd5620

D=$B/dist/dmd5620-$VERSION-macos
rm -rf "$D" "$D.zip" && mkdir -p "$D"
cp dmd5620 README.md MCP.md LICENSE.md "$D/"
cp -R layers "$D/"
cp dmd5620.man "$D/dmd5620.1"
(cd "$B/dist" && ditto -c -k --keepParent "dmd5620-$VERSION-macos" "dmd5620-$VERSION-macos.zip")
rm -rf "$D"

# The DMG: the app and a link to /Applications
DMG=$D.dmg
S=$B/dmg-staging
rm -rf "$S" "$DMG" && mkdir -p "$S"
cp -R "$APP" "$S/"
ln -s /Applications "$S/Applications"
hdiutil create -quiet -volname "DMD5620 $VERSION" -srcfolder "$S" -ov -format UDZO "$DMG"
rm -rf "$S"
[ -n "$DEV_ID" ] && codesign --force --timestamp -s "$DEV_ID" "$DMG"
# hdiutil can leave a diskimages-helper that holds the DMG; notarytool then hangs.
for p in $(lsof -t "$DMG" 2>/dev/null); do
    ps -o comm= -p "$p" | grep -q diskimages-helper && kill -9 "$p"
done

if [ -n "$DEV_ID" ] && [ -n "$NOTARY_PROFILE" ]; then
    notarize "$D.zip"
    notarize "$DMG"
    xcrun stapler staple -q "$DMG"
    xcrun stapler validate -q "$DMG" && echo "DMG stapled"
fi
echo "$D.zip"
echo "$DMG"
