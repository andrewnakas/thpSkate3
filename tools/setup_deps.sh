#!/bin/sh
# Clone the dependencies into deps/ at the revisions thpSkate3 was built against and apply its patches
# (patches/: RT64 trainer overlay, frame capture and a vertex-index bounds check; skatecore-cpp live tuning;
# librecomp: atomic stored-ROM write so several instances can start at once).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
D="$ROOT/deps"; mkdir -p "$D"
fetch() {  # dir url rev
    [ -d "$D/$1/.git" ] || git clone "$2" "$D/$1"
    git -C "$D/$1" checkout -q "$3"
}
fetch skatecore-cpp https://github.com/andrewnakas/skatecore-cpp 1da3871d5ce09e4b0ec7be4911e776878b1dfb73
fetch pw64 https://github.com/danielgomesvieira2000/pilotwings-64-recomp ce3634ad4be6ee5666dec2cd0bbcc35c4571bf29
git -C "$D/pw64" submodule update --init --recursive
apply() {  # dir patch
    if git -C "$1" apply --check -R "$ROOT/patches/$2" 2>/dev/null; then echo "$2: already applied"
    else git -C "$1" apply "$ROOT/patches/$2" && echo "$2: applied"; fi
}
apply "$D/skatecore-cpp" skatecore-cpp.patch
apply "$D/pw64/lib/RT64" rt64.patch
apply "$D/pw64/lib/RT64/src/contrib/plume" plume.patch
apply "$D/pw64/lib/N64ModernRuntime" n64modernruntime.patch
