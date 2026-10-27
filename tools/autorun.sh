#!/bin/sh
# Unattended port run: tools/autorun.sh NAME SECONDS "PRESSES" "SHOT_TIMES" ["DUMP_TIMES"]
# -> work/runs/NAME/{shot_NN.png, sheet.png, rdram_NN.bin, stderr.log}. Window hidden.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NAME=$1; SECS=$2; PRESS=$3; SHOTS=$4; DUMPS=${5:-}
OUT="$ROOT/work/runs/$NAME"; mkdir -p "$OUT"
cd "$ROOT/port/build"
SDL_AUDIODRIVER=dummy THPS_HIDDEN=1 THPS_OUT_DIR="$OUT" THPS_PRESS="$PRESS" THPS_SHOT_AT="$SHOTS" THPS_DUMP_AT="$DUMPS" THPS_QUIT_AT="$SECS" \
  timeout $((SECS + 40)) ./THPS64Recomp "$ROOT/work/thps.z64" > "$OUT/stdout.log" 2> "$OUT/stderr.log" || echo "exit $?"
cd "$OUT"
python3 -c "
import glob
from PIL import Image
for f in sorted(glob.glob('shot_*.ppm')):
    Image.open(f).save(f[:-4] + '.png')
"
ls shot_*.png >/dev/null 2>&1 && python3 "$ROOT/tools/sheet.py" sheet.png shot_*.png
echo "$OUT"
