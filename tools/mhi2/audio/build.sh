#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")/../../.."
cc=${MHI2_ARM_CC:-arm-none-eabi-gcc}
out=${MHI2_BUILD_DIR:-/tmp}
mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d /tmp/mhi2-audio-build.XXXXXX)
trap 'rm -rf "$work"' EXIT
# Link-only ABI stubs. These are never installed in the guest; the executable
# dynamically resolves the firmware's original libc and libsocket at runtime.
cat > "$work/libc.c" <<'STUB'
int open(void){return 0;} int close(void){return 0;} int write(void){return 0;} int snprintf(void){return 0;} void *mmap64(void){return 0;} void exit(void){} int pthread_mutex_lock(void){return 0;} int pthread_mutex_unlock(void){return 0;} int pthread_cond_signal(void){return 0;} void *memcpy(void){return 0;} void *memset(void){return 0;} int usleep(void){return 0;}
STUB
cat > "$work/socket.c" <<'STUB'
int socket(void){return 0;} int connect(void){return 0;} int send(void){return 0;} int recv(void){return 0;}
STUB
flags=(-mfloat-abi=soft -mcpu=cortex-a9 -marm -mno-unaligned-access -ffreestanding -fno-builtin -nostdlib -O2)
"$cc" "${flags[@]}" -fPIC -shared -Wl,-soname,libc.so.3 -o "$work/libc.so" "$work/libc.c"
"$cc" "${flags[@]}" -fPIC -shared -Wl,-soname,libsocket.so.3 -o "$work/socket.so" "$work/socket.c"
"$cc" "${flags[@]}" -Wl,--dynamic-linker=/usr/lib/ldqnx.so.2,-e,_start -o "$out/mhi2-pcm-endpoint" tools/mhi2/audio/pcm_endpoint.c "$work/libc.so" "$work/socket.so"
"${cc%gcc}strip" "$out/mhi2-pcm-endpoint"
