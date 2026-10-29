#!/bin/sh
# Package a Windows x86-64 release (run in Git Bash): tools/make_release_win.sh VERSION
#   -> work/release/thpSkate3-VERSION-windows-x86_64.zip
# The archive holds the built game, the DLLs it loads, the launchers, setup tools and docs -- no game data:
# users run setup.bat with their own THPS ROM and Skate 3 disc image.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=${1:?usage: $0 VERSION}
NAME=thpSkate3-$VER-windows-x86_64
OUT=$ROOT/work/release/$NAME
case "$OUT" in */work/release/thpSkate3-*) ;; *) echo "refusing to clear $OUT"; exit 1 ;; esac
rm -rf "$OUT"
mkdir -p "$OUT/port/build" "$OUT/tools"
cp "$ROOT/port/build/THPS64Recomp.exe" "$OUT/port/build/"
cp "$ROOT"/port/build/*.dll "$OUT/port/build/"
# The MSVC C++ runtime, app-local, so the game starts without the Visual C++ redistributable installed.
CRT=${VC_CRT_DIR:-$(ls -d "/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Redist/MSVC/"*/x64/Microsoft.VC143.CRT | tail -1)}
for f in msvcp140.dll msvcp140_atomic_wait.dll vcruntime140.dll vcruntime140_1.dll; do cp "$CRT/$f" "$OUT/port/build/"; done
cp "$ROOT/thpskate3.bat" "$ROOT/setup.bat" "$ROOT/README.md" "$ROOT/LICENSE" "$ROOT/LICENSE.thps64" "$OUT/"
cp "$ROOT/port/LICENSE.pilotwings64-recomp" "$OUT/"
for f in setup.sh import_skate3.sh s3_import.py s3_pack_files.txt thps_pack.py thps_level.py thps_skel.py; do
    cp "$ROOT/tools/$f" "$OUT/tools/"
done
rm -f "$ROOT/work/release/$NAME.zip"
PY=python3; "$PY" -c "" 2>/dev/null || PY=python
(cd "$ROOT/work/release" && "$PY" -c "import shutil, sys; shutil.make_archive(sys.argv[1], 'zip', '.', sys.argv[1])" "$NAME")
ls -la "$ROOT/work/release/$NAME.zip"
