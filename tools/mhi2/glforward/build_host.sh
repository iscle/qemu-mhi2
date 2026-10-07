#!/usr/bin/env bash
# Host graphics bridge: Mesa EGL/GLES on Linux and macOS.
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-${MHI2_BUILD_DIR:-/tmp}/mhi2-glhost}
source=${2:-tools/mhi2/glforward/host/glhost.c}
# pkg-config emits shell words, including Homebrew's non-system include/lib paths.
mesa_config=$(pkg-config --cflags --libs egl glesv2 libavcodec libavutil libswscale)
read -r -a mesa_flags <<< "$mesa_config"
avcodec_libdir=$(pkg-config --variable=libdir libavcodec)
mkdir -p "$(dirname "$out")"
"${CC:-cc}" -O2 "$source" -o "$out" "${mesa_flags[@]}" "-Wl,-rpath,$avcodec_libdir" -pthread
