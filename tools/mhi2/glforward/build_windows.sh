#!/usr/bin/env bash
# Build MHI2's GL host bridge with native MSYS2 MinGW64 ANGLE and libuv.
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
source_root=${MHI2_UPSTREAM_ROOT:-$root}
out=${1:-"$root/build/windows"}
mkdir -p "$out"
: "${CC:=gcc}"
: "${PKG_CONFIG:=pkg-config}"
read -r -a native_flags <<< "$("$PKG_CONFIG" --cflags --libs angleproject libuv libavcodec libavutil libswscale)"
include=(-I"$root/tools/mhi2/glforward/host")
if [[ "$source_root" != "$root" ]]; then
    include+=(-I"$source_root/tools/mhi2/glforward/host")
fi
"$CC" -O2 "${include[@]}" "$root/tools/mhi2/glforward/host/glhost.c" -o "$out/mhi2-glhost.exe" "${native_flags[@]}" -pthread
"$CC" -O2 "${include[@]}" "$root/tools/mhi2/glforward/tests/encoder_async.c" -o "$out/encoder-async.exe" "${native_flags[@]}" -pthread
printf 'Built %s and %s\n' "$out/mhi2-glhost.exe" "$out/encoder-async.exe"

