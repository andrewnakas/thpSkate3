#!/bin/sh
# Build data/cleanpack/core.pack from your own Xbox 360 Skate 3 disc image. Nothing from Skate 3 is in this
# repository: the animation banks, state graphs, physics tables and skater model all come from your disc.
#   tools/import_skate3.sh SKATE3.iso [WORK_DIR]
# WORK_DIR (default work/skate3-import) needs about 1.5 GB. Needs Python 3 and uv (for numpy and Pillow).
# The converter is the asset pipeline of skate-3-rust-engine, fetched at the pinned revision below.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ISO=$1
WORK=${2:-$ROOT/work/skate3-import}
REV=cc41fb9
[ -f "$ISO" ] || { echo "usage: $0 SKATE3.iso [WORK_DIR]"; exit 2; }
mkdir -p "$WORK"
echo "== copying the converter's input files off the disc"
python3 "$ROOT/tools/s3_import.py" extract "$ISO" "$WORK/game"
echo "== fetching the converter (skate-3-rust-engine @ $REV)"
if [ ! -d "$WORK/converter/tools" ]; then
    mkdir -p "$WORK/converter"
    curl -fsSL "https://codeload.github.com/andrewnakas/skate-3-rust-engine/tar.gz/$REV" |
        tar -xz -C "$WORK/converter" --strip-components=1 --wildcards '*/tools/*'
fi
VENV=${THPS_IMPORT_VENV:-$HOME/.local/share/thpskate3-venv}
[ -x "$VENV/bin/python" ] || uv venv -q "$VENV"
uv pip install -q --python "$VENV/bin/python" numpy pillow
echo "== converting (animation banks, physics tables, skater model)"
rm -rf "$WORK/stage"
mkdir -p "$WORK/stage/assets/private" "$WORK/stage/conversion"
(cd "$WORK/converter" && "$VENV/bin/python" - "$WORK/game" "$WORK/stage" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, '.')
from tools.asset_pipeline import asset_exports as X
game, stage = Path(sys.argv[1]), Path(sys.argv[2])
with open(stage / 'setup.log', 'w') as log:
    converted = X.core(game, stage, stage / 'conversion', print, log)
    X.character(game, stage, stage / 'conversion', print, log, converted)
PY
)
mkdir -p "$ROOT/data/cleanpack"
python3 "$ROOT/tools/s3_import.py" make "$WORK/stage/assets" "$ROOT/data/cleanpack/core.pack"
echo "== done: data/cleanpack/core.pack (the converter's working files in $WORK can be deleted)"
