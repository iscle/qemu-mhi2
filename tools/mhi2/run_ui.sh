#!/usr/bin/env bash
# Run the prepared experimental media with the production RCC input peer.
set -eu
cd "$(dirname "$0")/../.."
export MHI2_GL=1 MHI2_RCC=1 MHI2_MOST=1
if [[ -x ../build/qemu/qemu-system-arm ]]; then
    export QEMU_SYSTEM_ARM="${QEMU_SYSTEM_ARM:-$PWD/../build/qemu/qemu-system-arm}"
fi
export MHI2_TIMEOUT="${MHI2_TIMEOUT:-86400}"
export MHI2_COMMAND_FIFO=/tmp/mhi2-shell-commands
if [[ ! -x /tmp/mhi2-glhost || tools/mhi2/glforward/host/glhost.c -nt /tmp/mhi2-glhost || tools/mhi2/glforward/host/known_shaders.h -nt /tmp/mhi2-glhost || tools/mhi2/glforward/host/audi_shaders.h -nt /tmp/mhi2-glhost || tools/mhi2/glforward/host/encoder.h -nt /tmp/mhi2-glhost ]]; then
    cc -O2 tools/mhi2/glforward/host/glhost.c -o /tmp/mhi2-glhost -lEGL -lGLESv2 -pthread
fi
cat > /tmp/mhi2-diag-commands.txt <<'GUEST'
echo UI_DIAGNOSTIC_SHELL_READY
(sleep 25; /sbin/mhi2-pcm-endpoint > /tmp/mhi2-pcm-endpoint.log 2>&1) &
(sleep 27; slay -P 25r mhi2-pcm-endpoint) &
(sleep 20; export LD_LIBRARY_PATH=/eso/lib:/armle/lib:/armle/usr/lib:/lib:/usr/lib; export IPL_CONFIG_DIR=/etc/eso/production; devp-iso-mmx-mib2 -T -S188 -i3 -B3 -P64 -Q18 -m/dev/mlb -MisoTX2 -v3 -p16) &
GUEST
exec python3 tools/mhi2/launch_ui.py "$@"
