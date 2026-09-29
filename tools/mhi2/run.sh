#!/usr/bin/env bash
set -euo pipefail
media=${1:?Usage: run.sh MEDIA_DIRECTORY [extra QEMU options]}
shift
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
media=$(realpath -- "$media")
mkdir -p -- "$media/tmp"
# QEMU substitutes /var/tmp for /tmp when creating snapshot files.
export TMPDIR="$media/tmp"
exec "${QEMU_SYSTEM_ARM:-$repo/build/qemu-system-arm}" \
    -M "mhi2-harman,iram=$media/iram.bin" \
    -bios "$media/nor.bin" \
    -drive "if=sd,format=raw,file=$media/emmc.raw,snapshot=on" \
    -display none -monitor none \
    -serial null -serial null -serial null \
    -serial "${MHI2_CONSOLE:-mon:stdio}" "$@"
