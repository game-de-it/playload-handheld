#!/bin/sh
# Builds the release archive dist/playload-handheld-<version>.zip:
#
#   Playload.sh                 PortMaster launch script
#   playload/
#     playload.aarch64          the player
#     games/*.url               bundled games
#     node/node, node/lib/      Node.js and the libraries it needs
#     node/host.mjs, node/builtin/   Node.js host and the built-in Playload engine
#     licenses/                 license texts and third-party notices
#
# Node.js and the Debian libraries are downloaded from their official servers
# and checked against pinned SHA-256 checksums.
set -eu
cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/^#define PLAYLOAD_HANDHELD_VERSION "\(.*\)"/\1/p' src/player.c)
NODE_VERSION=v24.22.1
NODE_SHA256=dbabf7a1cbf947e6059ffad2c27b80041db97dea1e553b62afdee7f13fca336e
DEBIAN=http://deb.debian.org/debian/pool/main
# package file, sha256
DEBS="
g/glibc/libc6_2.36-9+deb12u14_arm64.deb 01f4330719fd4f65580e16ea5a0527f372fca750e8f588d26deaf09f2d3b1cf4
g/gcc-12/libstdc++6_12.2.0-14+deb12u1_arm64.deb 26e138c677a985775331373828a6c286c551ff397cb735d00e2383cb273d1cb2
g/gcc-12/libgcc-s1_12.2.0-14+deb12u1_arm64.deb 576926b283613db80168ddf76380a3bd877602778cf0d226caa7bfbfa71eacf3
g/gcc-12/gcc-12-base_12.2.0-14+deb12u1_arm64.deb 674cf6cba6d432bd200c45fe866c1652c7a53523cc2e7a613e05bc4abf7b5440
"
CACHE=build/downloads
STAGE=build/stage
OUT=dist/playload-handheld-$VERSION.zip

fetch() {  # url sha256 file
  if [ ! -f "$3" ] || ! echo "$2  $3" | shasum -a 256 -c - >/dev/null 2>&1; then
    curl -fsSL -o "$3" "$1"
  fi
  echo "$2  $3" | shasum -a 256 -c - >/dev/null || { echo "checksum mismatch: $1" >&2; exit 1; }
}

./build.sh aarch64 >/dev/null
mkdir -p "$CACHE" dist
rm -rf "$STAGE"
mkdir -p "$STAGE/playload/node/lib" "$STAGE/playload/node/builtin" "$STAGE/playload/licenses/debian"

# Player, launch script, games, Node.js host and built-in engine
cp port/Playload.sh "$STAGE/"
cp build/playload.aarch64 "$STAGE/playload/"
cp -R port/playload/games "$STAGE/playload/"
cp js/node-host.mjs "$STAGE/playload/node/host.mjs"
cp js/playload/engine.js js/playload/sound.js js/playload/glyphs.js "$STAGE/playload/node/builtin/"

# Node.js
NODE_TAR="$CACHE/node-$NODE_VERSION-linux-arm64.tar.xz"
fetch "https://nodejs.org/dist/$NODE_VERSION/node-$NODE_VERSION-linux-arm64.tar.xz" "$NODE_SHA256" "$NODE_TAR"
tar -xJf "$NODE_TAR" -C "$CACHE" "node-$NODE_VERSION-linux-arm64/bin/node" "node-$NODE_VERSION-linux-arm64/LICENSE"
cp "$CACHE/node-$NODE_VERSION-linux-arm64/bin/node" "$STAGE/playload/node/node"
cp "$CACHE/node-$NODE_VERSION-linux-arm64/LICENSE" "$STAGE/playload/licenses/NODEJS-LICENSE.txt"

# Debian libraries Node.js links against, with their copyright files
# (libstdc++6 and libgcc-s1 keep theirs in gcc-12-base: GPL-3 with the GCC
# Runtime Library Exception; libc6: LGPL-2.1).
echo "$DEBS" | while read -r deb sha; do
  [ -n "$deb" ] || continue
  file="$CACHE/$(basename "$deb")"
  fetch "$DEBIAN/$deb" "$sha" "$file"
  dir="$CACHE/$(basename "$deb" .deb)"
  rm -rf "$dir" && mkdir -p "$dir"
  (cd "$dir" && ar x "../$(basename "$deb")" && tar -xf data.tar.*)
  pkg=$(basename "$deb" | cut -d_ -f1)
  [ -f "$dir/usr/share/doc/$pkg/copyright" ] &&
    cp "$dir/usr/share/doc/$pkg/copyright" "$STAGE/playload/licenses/debian/$pkg.copyright"
done
cp "$CACHE/libc6_2.36-9+deb12u14_arm64/lib/aarch64-linux-gnu/libdl.so.2" \
   "$CACHE/libc6_2.36-9+deb12u14_arm64/lib/aarch64-linux-gnu/libpthread.so.0" \
   "$CACHE/libgcc-s1_12.2.0-14+deb12u1_arm64/lib/aarch64-linux-gnu/libgcc_s.so.1" \
   "$STAGE/playload/node/lib/"
cp "$CACHE/libstdc++6_12.2.0-14+deb12u1_arm64/usr/lib/aarch64-linux-gnu/libstdc++.so.6.0.30" \
   "$STAGE/playload/node/lib/libstdc++.so.6"

# Licenses
cp LICENSE "$STAGE/playload/licenses/LICENSE.txt"
cp THIRD_PARTY_NOTICES.md "$STAGE/playload/licenses/"
cp licenses/* "$STAGE/playload/licenses/"

chmod 755 "$STAGE/Playload.sh" "$STAGE/playload/playload.aarch64" "$STAGE/playload/node/node"
rm -f "$OUT"
(cd "$STAGE" && zip -qrX "../../$OUT" Playload.sh playload)
shasum -a 256 "$OUT" | tee "$OUT.sha256"
ls -la "$OUT"
