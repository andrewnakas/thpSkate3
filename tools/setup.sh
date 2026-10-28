#!/bin/sh
# One-time setup from your own games: tools/setup.sh THPS.z64 SKATE3.iso
#   THPS.z64   Tony Hawk's Pro Skater (N64), US V1.1 ROM
#   SKATE3.iso Skate 3, Xbox 360 disc image
# Writes work/thps.z64, the THPS level and rig caches (work/levels) and the Skate 3 data (data/cleanpack).
# Needs Python 3, curl and uv (for the Skate 3 converter's numpy/Pillow). Nothing from either game ships with
# thpSkate3; everything here is made from the files you give it.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ROM=$1
ISO=$2
[ -f "$ROM" ] && [ -f "$ISO" ] || { echo "usage: $0 THPS.z64 SKATE3.iso"; exit 2; }
mkdir -p "$ROOT/work"
cp "$ROM" "$ROOT/work/thps.z64"

echo "== THPS: unpacking the ROM's assets"
# The ERZ unpacker comes from the THPS64 decompilation (CC0), fetched at a pinned revision.
ERZ_DIR=$ROOT/work/thps64/tools
if [ ! -f "$ERZ_DIR/erz.py" ]; then
    mkdir -p "$ERZ_DIR"
    curl -fsSL https://raw.githubusercontent.com/andrewnakas/thps64/82558fda15dec4ee3cf2549dab188e3342b6ed32/tools/erz.py \
        -o "$ERZ_DIR/erz.py"
fi
export THPS64="$ROOT/work/thps64"
python3 "$ROOT/tools/thps_pack.py" "$ROOT/work/thps.z64" "$ROOT/work/pack"

echo "== THPS: level collision, rails and character rigs"
(cd "$ROOT" && python3 -c "import sys; sys.path.insert(0,'tools'); import thps_level as l; l.export_all('work/pack','work/levels')")
(cd "$ROOT" && python3 -c "import sys; sys.path.insert(0,'tools'); import thps_skel as s; s.export_rigs('work/pack','work/levels/rigs.thr')")

echo "== Skate 3: animation, physics data, skater model and trick names"
"$ROOT/tools/import_skate3.sh" "$ISO"

echo "== done. Start the game with ./thpskate3 (or port/build/THPS64Recomp work/thps.z64)"
