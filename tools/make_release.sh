#!/bin/sh
# Package a Linux x86-64 release: tools/make_release.sh VERSION -> work/release/thpSkate3-VERSION-linux-x86_64.tar.gz
# The archive holds the built game (stripped), its SDL2, the launcher, setup tools and docs -- no game data:
# users run tools/setup.sh with their own THPS ROM and Skate 3 disc image.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=${1:?usage: $0 VERSION}
NAME=thpSkate3-$VER-linux-x86_64
OUT=$ROOT/work/release/$NAME
rm -rf "$OUT"
mkdir -p "$OUT/port/build" "$OUT/lib" "$OUT/tools/native"
strip -o "$OUT/port/build/THPS64Recomp" "$ROOT/port/build/THPS64Recomp"
cp -L "$ROOT/deps/prefix/lib/libSDL2-2.0.so.0" "$OUT/lib/"
cp "$ROOT/thpskate3" "$ROOT/README.md" "$ROOT/LICENSE" "$ROOT/LICENSE.thps64" "$OUT/"
cp "$ROOT/port/LICENSE.pilotwings64-recomp" "$OUT/"
for f in setup.sh import_skate3.sh s3_import.py s3_pack_files.txt thps_pack.py thps_level.py thps_skel.py; do
    cp "$ROOT/tools/$f" "$OUT/tools/"
done
(cd "$ROOT/work/release" && tar -czf "$NAME.tar.gz" "$NAME")
ls -la "$ROOT/work/release/$NAME.tar.gz"
