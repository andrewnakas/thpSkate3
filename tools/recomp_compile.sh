#!/bin/sh
# Compile-check N64Recomp's output (recomp/RecompiledFuncs) with clang: -> work/recomp_obj/*.o + work/recomp_compile.log
# Flags follow the PW64 port: generated code needs -fno-strict-aliasing -fwrapv. -j3 keeps RAM in check.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LIB=${N64MR:-$ROOT/deps/pw64/lib/N64ModernRuntime}
OUT="$ROOT/work/recomp_obj"; mkdir -p "$OUT"
LOG="$ROOT/work/recomp_compile.log"; : > "$LOG"
ls "$ROOT"/recomp/RecompiledFuncs/*.c | xargs -P 3 -I{} sh -c '
  f="{}"; b=$(basename "$f" .c)
  clang -c -O1 -fno-strict-aliasing -fwrapv -w -I"'"$LIB"'/N64Recomp/include" -I"'"$LIB"'/librecomp/include" \
    -I"'"$LIB"'/ultramodern/include" -I"'"$ROOT"'/recomp/RecompiledFuncs" -I"'"$ROOT"'/recomp/include" "$f" -o "'"$OUT"'/$b.o" 2>>"'"$LOG"'" \
    && echo "ok $b" || echo "FAIL $b"' | sort | uniq -c | awk '{print}' | tail -3
echo "objects: $(ls "$OUT"/*.o 2>/dev/null | wc -l) / $(ls "$ROOT"/recomp/RecompiledFuncs/*.c | wc -l); errors: $(grep -c 'error:' "$LOG" || true)"
