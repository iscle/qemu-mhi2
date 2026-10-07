#!/usr/bin/env bash
# Host graphics bridge: Mesa EGL/GLES on Linux and macOS.
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-${MHI2_BUILD_DIR:-/tmp}/mhi2-glhost}
source=${2:-tools/mhi2/glforward/host/glhost.c}
flags=()
if [[ $(uname -s) == Darwin ]]; then
    flags+=(-framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation)
fi
# pkg-config emits shell words, including Homebrew's non-system include/lib paths.
read -r -a mesa_flags <<< "$(pkg-config --cflags --libs egl glesv2)"
mkdir -p "$(dirname "$out")"
"${CC:-cc}" -O2 "$source" -o "$out" "${mesa_flags[@]}" "${flags[@]}" -pthread
