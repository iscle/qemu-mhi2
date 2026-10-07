#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")/../../.."
out=${MHI2_BUILD_DIR:-/tmp}
mkdir -p "$out"
out=$(realpath "$out")
arm_cc=${MHI2_ARM_CC:-arm-none-eabi-gcc}
arm_ld=${MHI2_ARM_LD:-${arm_cc%gcc}ld}
export MHI2_BUILD_DIR="$out" MHI2_ARM_CC="$arm_cc"
"${CC:-cc}" -O2 tools/mhi2/glforward/host/glhost.c -o "$out/mhi2-glhost" -lEGL -lGLESv2 -pthread
"$arm_cc" -mfloat-abi=soft -mcpu=cortex-a9 -marm -fPIC -ffreestanding -fno-builtin -O2 -c tools/mhi2/glforward/guest/gl_shim.c -o "$out/mhi2-gl-shim.o"
for name in libGLESv2.so libEGL.so; do
 "$arm_ld" -shared --hash-style=sysv -soname "$name" -o "$out/mhi2-$name" "$out/mhi2-gl-shim.o"
done
"$arm_cc" -mfloat-abi=soft -mcpu=cortex-a9 -marm -fPIC -ffreestanding -fno-builtin -O2 -c tools/mhi2/glforward/guest/nvvsenc_shim.c -o "$out/mhi2-nvvsenc-shim.o"
"$arm_ld" -shared --hash-style=sysv -soname libnvvsenc.so -o "$out/mhi2-libnvvsenc.so" "$out/mhi2-nvvsenc-shim.o"
bash tools/mhi2/audio/build.sh
if [[ "${MHI2_BUILD_LIBRARIES_ONLY:-0}" != 1 ]]; then
 python3 tools/mhi2/glforward/build_media.py > /tmp/mhi2-build-gl-media.log 2>&1
fi
