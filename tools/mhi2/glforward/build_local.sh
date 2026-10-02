set -e
cd "$(dirname "$0")/../../.."
cc -O2 tools/mhi2/glforward/host/glhost.c -o /tmp/mhi2-glhost -lEGL -lGLESv2 -pthread
/home/iscle/xpack-arm-none-eabi-gcc-13.3.1-1.1/bin/arm-none-eabi-gcc -mfloat-abi=soft -mcpu=cortex-a9 -marm -fPIC -ffreestanding -fno-builtin -O2 -c tools/mhi2/glforward/guest/gl_shim.c -o /tmp/mhi2-gl-shim.o
for name in libGLESv2.so libEGL.so; do
 /home/iscle/xpack-arm-none-eabi-gcc-13.3.1-1.1/bin/arm-none-eabi-ld -shared --hash-style=sysv -soname "$name" -o "/tmp/mhi2-$name" /tmp/mhi2-gl-shim.o
done
/home/iscle/xpack-arm-none-eabi-gcc-13.3.1-1.1/bin/arm-none-eabi-gcc -mfloat-abi=soft -mcpu=cortex-a9 -marm -fPIC -ffreestanding -fno-builtin -O2 -c tools/mhi2/glforward/guest/nvvsenc_shim.c -o /tmp/mhi2-nvvsenc-shim.o
/home/iscle/xpack-arm-none-eabi-gcc-13.3.1-1.1/bin/arm-none-eabi-ld -shared --hash-style=sysv -soname libnvvsenc.so -o /tmp/mhi2-libnvvsenc.so /tmp/mhi2-nvvsenc-shim.o
bash tools/mhi2/audio/build.sh
if [[ "${MHI2_BUILD_LIBRARIES_ONLY:-0}" != 1 ]]; then
 python3 tools/mhi2/glforward/build_media.py > /tmp/mhi2-build-gl-media.log 2>&1
fi
