#!/bin/sh
# Syntax-check every decomp C file with the NDK clang using the port's include order.
# Usage: tools/syntax_check.sh <outdir> [file-list]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NDK=${ANDROID_NDK:-/opt/android-sdk/ndk/28.2.13676358}
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang
OUT=$1; mkdir -p "$OUT"; rm -f "$OUT"/*.err
cd "$ROOT/decomp"
INC="-I$ROOT/port/compat/include -Iinclude $(find libs -type d -name include | sort | sed 's/^/-I/' | tr '\n' ' ')"
LIST=${2:-}
[ -n "$LIST" ] || { find src libs -name '*.c' | sort > "$OUT/files.txt"; LIST="$OUT/files.txt"; }
export CC INC OUT ROOT
xargs -P"$(nproc)" -I{} sh -c '$CC --target=armv7a-linux-androideabi26 -fsyntax-only -w -ferror-limit=20 -Dversion_EUR -include $ROOT/port/compat/port_prefix.h $INC {} >/dev/null 2>"$OUT/$(echo {} | tr / _).err" || echo {}' < "$LIST" > "$OUT/fails.txt" || true
echo "failed: $(wc -l < "$OUT/fails.txt") / $(wc -l < "$LIST")"
cat "$OUT"/*.err | grep -oE "error: .*" | sed -E "s/'[^']*'/'X'/g" | sort | uniq -c | sort -rn | head -30
