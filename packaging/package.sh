#!/bin/sh
# Builds the player download for one platform:
#   packaging/package.sh windows|linux
# -> build/dist/CanopusProtocol-<platform>-x64.zip|.tar.gz
#
# The download holds the launcher, the setup tool, the recompiler, the
# runtime (prebuilt), Zig (the C compiler the player's machine builds the
# game with) and the icon. Nothing from the game: the player's own dump is
# extracted, recompiled and compiled on their machine the first time it
# starts.
#
# Needs: Go, Zig (the one on your PATH, or ZIG=/path/to/zig), zip/tar, and
# for the Windows package from Linux, curl (or ZIG_WINDOWS, below). Runs on
# Linux (or WSL / MSYS2 on Windows).
#
# The download carries Zig for the player's platform, the same version as
# yours (the prebuilt runtime has to match the C++ library the player's
# machine links it with):
#   - for your own platform, your installed Zig is copied;
#   - for the other one, the official build of that version is downloaded
#     from ziglang.org, unless ZIG_WINDOWS / ZIG_LINUX points to an
#     unpacked official Zig folder (the one with the zig program and lib/).
set -eu

PLATFORM=${1:?usage: package.sh windows|linux}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$ROOT/build/package
DIST=$ROOT/build/dist
NAME="Canopus Protocol"

case "$PLATFORM" in
windows) TARGET=x86_64-windows-gnu; RT_TARGET=windows-zig; EXE=.exe; GOOS=windows; ZOS=windows; ZEXT=zip ;;
linux)   TARGET=x86_64-linux-gnu.2.28; RT_TARGET=linux-compat; EXE=; GOOS=linux; ZOS=linux; ZEXT=tar.xz ;;
*) echo "usage: package.sh windows|linux"; exit 1 ;;
esac
case "$(uname -s)" in
Linux*) HOST=linux ;;
MINGW*|MSYS*|CYGWIN*) HOST=windows ;;
*) echo "build the package on Linux, WSL or MSYS2"; exit 1 ;;
esac

# ---- Zig ----
ZIG=${ZIG:-zig}
command -v "$ZIG" >/dev/null 2>&1 || { echo "Zig not found: install it (ziglang.org) or set ZIG=/path/to/zig"; exit 1; }
ZIG_VERSION=$("$ZIG" version)
# Where it keeps its lib/ ("zig env": ZON since 0.15, JSON before).
zig_lib_dir() {
    "$ZIG" env | sed -n 's/.*[."]lib_dir"\{0,1\} *[=:] *"\(.*\)".*/\1/p' | head -n 1
}
ZIG_LIB=$(zig_lib_dir)
case "$ZIG_LIB" in /*|?:*) ;; *) ZIG_LIB=$(pwd)/$ZIG_LIB ;; esac
[ -d "$ZIG_LIB/std" ] || { echo "can't find Zig's lib folder (zig env says $ZIG_LIB)"; exit 1; }
echo "== Zig $ZIG_VERSION ($(command -v "$ZIG"))"

# The Zig to bundle, as a folder with the zig program, lib/ and LICENSE.
bundle_zig() {
    out=$BUILD/zig-$PLATFORM-$ZIG_VERSION
    given=$( [ "$PLATFORM" = windows ] && echo "${ZIG_WINDOWS:-}" || echo "${ZIG_LINUX:-}" )
    if [ -n "$given" ]; then
        [ -x "$given/zig$EXE" ] || [ -f "$given/zig$EXE" ] || { echo "no zig$EXE in $given"; exit 1; }
        v=$( [ "$PLATFORM" = "$HOST" ] && "$given/zig$EXE" version || echo "$ZIG_VERSION" )
        [ "$v" = "$ZIG_VERSION" ] || { echo "$given is Zig $v, but yours is $ZIG_VERSION: they must match"; exit 1; }
        ZIG_BUNDLE=$given
        return
    fi
    if [ "$PLATFORM" = "$HOST" ]; then
        # Your own installation.
        rm -rf "$out" && mkdir -p "$out"
        cp "$(command -v "$ZIG")" "$out/zig$EXE"
        cp -r "$ZIG_LIB" "$out/lib"
        for l in "$(dirname "$(readlink -f "$(command -v "$ZIG")")")/LICENSE" "$ZIG_LIB/../LICENSE" "$ZIG_LIB/../../share/licenses/zig/LICENSE" /usr/share/licenses/zig/LICENSE /usr/share/doc/zig/copyright; do
            if [ -f "$l" ]; then cp "$l" "$out/LICENSE"; break; fi
        done
        [ -f "$out/LICENSE" ] || echo "warning: Zig's LICENSE file was not found; add it to licenses/ in the download by hand"
        ZIG_BUNDLE=$out
        return
    fi
    # The official build from ziglang.org (file names changed in 0.14).
    if [ ! -d "$out" ]; then
        mkdir -p "$BUILD/download"
        file=
        for name in zig-x86_64-$ZOS-$ZIG_VERSION zig-$ZOS-x86_64-$ZIG_VERSION; do
            url=https://ziglang.org/download/$ZIG_VERSION/$name.$ZEXT
            echo "== Downloading $url"
            if curl -fL --retry 2 -o "$BUILD/download/$name.$ZEXT" "$url"; then file=$name; break; fi
        done
        [ -n "$file" ] || { echo "could not download Zig $ZIG_VERSION for $ZOS: download it from ziglang.org, unpack it and set ZIG_WINDOWS (or ZIG_LINUX) to its folder"; exit 1; }
        rm -rf "$BUILD/unpack" && mkdir -p "$BUILD/unpack"
        if [ "$ZEXT" = zip ]; then unzip -q "$BUILD/download/$file.$ZEXT" -d "$BUILD/unpack"
        else tar -xJf "$BUILD/download/$file.$ZEXT" -C "$BUILD/unpack"; fi
        mv "$BUILD/unpack/$file" "$out"
    fi
    ZIG_BUNDLE=$out
}
bundle_zig
export ZIG_GLOBAL_CACHE_DIR=$BUILD/zig-cache

STAGE=$BUILD/stage-$PLATFORM/$NAME
rm -rf "$BUILD/stage-$PLATFORM"
mkdir -p "$STAGE/tools/runtime" "$STAGE/tools/tables" "$STAGE/games"

echo "== Runtime ($RT_TARGET)"
mkdir -p "$BUILD/rtgen"
cp "$ROOT/recomp/runtime/recomp.h" "$BUILD/rtgen/recomp.h"
make -C "$ROOT/runtime" -j"$(nproc 2>/dev/null || echo 4)" lib TARGET=$RT_TARGET GEN="$BUILD/rtgen" BUILD="$BUILD/rt" ZIG="$ZIG" >/dev/null
cp "$BUILD/rt/$RT_TARGET/libcanopus_rt.a" "$STAGE/tools/runtime/"
if [ "$PLATFORM" = windows ]; then
    cp "$BUILD/rt/$RT_TARGET/obj/rt/canopus.res" "$STAGE/tools/runtime/"
fi

echo "== Setup tool"
"$ZIG" c++ -target $TARGET -std=c++17 -O2 -w -s -o "$STAGE/tools/setup$EXE" "$ROOT/setup/setup_main.cpp" "$ROOT/setup/ctr.cpp" >/dev/null 2>&1

echo "== Recompiler"
(cd "$ROOT/recomp" && CGO_ENABLED=0 GOOS=$GOOS GOARCH=amd64 go build -trimpath -ldflags "-s -w" -o "$STAGE/tools/recomp$EXE" .)
cp "$ROOT/recomp/functions.csv" "$ROOT/recomp/hints.txt" "$ROOT/recomp/functions_v11.csv" "$ROOT/recomp/hints_v11.txt" "$STAGE/tools/tables/"

echo "== Launcher"
if [ "$PLATFORM" = windows ]; then
    (cd "$ROOT/runtime/res" && "$ZIG" rc /fo "$BUILD/launcher.res" canopus.rc)
    "$ZIG" c++ -target $TARGET -std=c++17 -O2 -w -s -mwindows -o "$STAGE/$NAME.exe" "$ROOT/launcher/launcher.cpp" "$BUILD/launcher.res" -lshell32 -lgdi32 >/dev/null 2>&1
else
    "$ZIG" c++ -target $TARGET -std=c++17 -O2 -w -s -o "$STAGE/canopus-protocol" "$ROOT/launcher/launcher.cpp" >/dev/null 2>&1
    cp "$ROOT/branding/canopus-protocol-icon-256.png" "$STAGE/canopus-protocol.png"
fi

echo "== Zig $ZIG_VERSION for the player (only what building for $PLATFORM needs)"
mkdir -p "$STAGE/tools/zig"
cp "$ZIG_BUNDLE/zig$EXE" "$STAGE/tools/zig/"
cp -r "$ZIG_BUNDLE/lib" "$STAGE/tools/zig/lib"
[ -f "$ZIG_BUNDLE/LICENSE" ] && cp "$ZIG_BUNDLE/LICENSE" "$STAGE/tools/zig/LICENSE"
rm -rf "$STAGE/tools/zig/lib/docs" "$STAGE/tools/zig/lib/build-web" "$STAGE/tools/zig/lib/init" "$STAGE/tools/zig/lib/libtsan"
for d in musl wasi darwin freebsd netbsd openbsd; do rm -rf "$STAGE/tools/zig/lib/libc/$d"; done
if [ "$PLATFORM" = windows ]; then
    rm -rf "$STAGE/tools/zig/lib/libc/glibc"
    keep="any-windows-any"
else
    rm -rf "$STAGE/tools/zig/lib/libc/mingw"
    keep="any-linux-any generic-glibc x86-linux-any x86-linux-gnu"
fi
for d in "$STAGE/tools/zig/lib/libc/include"/*; do
    case " $keep " in *" $(basename "$d") "*) ;; *) rm -rf "$d" ;; esac
done

echo "== Documents"
cp "$ROOT/packaging/README-player.txt" "$STAGE/README.txt"
cp "$ROOT/packaging/games-README.txt" "$STAGE/games/README.txt"
mkdir -p "$STAGE/licenses"
[ -f "$STAGE/tools/zig/LICENSE" ] && cp "$STAGE/tools/zig/LICENSE" "$STAGE/licenses/zig-LICENSE.txt"
if [ "$PLATFORM" = windows ]; then
    for f in README.txt games/README.txt; do sed 's/$/\r/' "$STAGE/$f" > "$STAGE/$f.crlf" && mv "$STAGE/$f.crlf" "$STAGE/$f"; done
fi

# Nothing from a game may be in the download.
if find "$STAGE" \( -name code.bin -o -name exheader.bin -o -name romfs -o -name 'text_*.c' -o -name '*.3ds' -o -name '*.cia' -o -name save \) | grep -q .; then
    echo "game files found in the package: stopping"; exit 1
fi

mkdir -p "$DIST"
if [ "$PLATFORM" = windows ]; then
    OUT=$DIST/CanopusProtocol-windows-x64.zip
    rm -f "$OUT"
    (cd "$BUILD/stage-$PLATFORM" && zip -qr -9 "$OUT" "$NAME")
else
    OUT=$DIST/CanopusProtocol-linux-x64.tar.gz
    rm -f "$OUT"
    (cd "$BUILD/stage-$PLATFORM" && tar -czf "$OUT" "$NAME")
fi
echo "== $OUT ($(du -h "$OUT" | cut -f1))"
