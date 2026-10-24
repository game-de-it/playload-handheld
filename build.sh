#!/bin/sh
# Builds the player: build/playload-mac (headless, for development) and
# build/playload.aarch64 (plumOS / PortMaster, glibc 2.36, SDL2).
set -eu
cd "$(dirname "$0")"
mkdir -p build
python3 tools/embed.py build/js_bundle.h js/patch.js js/host.js js/playload/engine.js js/playload/sound.js js/playload/glyphs.js js/playload/crt.js
SRC="src/player.c src/noderun.c src/video.c src/ay.c src/inflate.c vendor/quickjs/quickjs-amalgam.c"
CFLAGS="-O2 -std=gnu11 -Ibuild -Ivendor/quickjs -D_GNU_SOURCE -DNDEBUG -w"
case "${1:-all}" in
  mac|all)
    zig cc $CFLAGS -o build/playload-mac $SRC -lm
    ;;
esac
case "${1:-all}" in
  aarch64|all)
    ln -sf libSDL2-2.0.so.0 vendor/sdl2/libSDL2.so
    zig cc -target aarch64-linux-gnu.2.36 -mcpu=cortex_a55 $CFLAGS -DHAVE_SDL \
      -s -Ivendor/sdl2/include -Lvendor/sdl2 -o build/playload.aarch64 $SRC -lSDL2 -lm -lpthread
    ;;
esac
ls -la build
