# MHI2 history and validation

## Official upstream base

On 2026-10-07, `main` was reconstructed and rebased onto official QEMU master
[`f9587d4045c67cd0d8d8bdcd5d0bb5b6b395b63c`](https://github.com/qemu/qemu/commit/f9587d4045c67cd0d8d8bdcd5d0bb5b6b395b63c).
The previous tip was `c4c6bbda7cdf4ac19f117ea361946dcfb5cb42e2`.

The old graph contained an unrelated-root source snapshot and a reconciliation
merge that made upstream QEMU 9.2 maintenance commits appear in the fork's
changes. The replacement history starts from official upstream, carries only
MHI2 changes, and has no fork merge commits. Existing linear MHI2 changes keep
their authorship; the initial snapshot and Audi integration are represented by
their actual MHI2 deltas. The old history remains in local backup refs and the
previously published archive tags, not as an ancestor of the new `main`.

Before rebasing, the reconstructed MHI2 tree was compared byte-for-byte with
the old tip. Excluded changes were the vendored softfloat/testfloat/keycodemapdb
snapshots and accidental deletions of upstream ROM/test submodule entries.
There are no fork changes under `roms/`, `subprojects/`, or
`tests/lcitool/libvirt-ci`; the build fetches the pinned dependencies separately.

Existing clones should save local work before switching to the rewritten
history. A fresh shallow clone or source ZIP is the simplest option. For a
clean existing checkout with no local work to retain:

```sh
git fetch origin
git switch main
git reset --hard origin/main
```

## Earlier branch integration

The combined implementation originated in the Porsche/VW `mhi2-quickboot`
branch (`20c110b8af9a35722f5f916ccc09914891b9c216`) and Audi `mhi2-audi-a3`
branch (`a61c6b0021fa857742f28eb6e43626d3319e0b5d`). Their historical tips
remain available as `archive/mhi2-quickboot-2026-10-07` and
`archive/mhi2-audi-a3-2026-10-07` tags. Those tags predate the history cleanup.

## Reconciled behavior

The Porsche/VW branch's Tegra devices, SMP fixes, SD readers, Android Auto USB
transport, feature configuration, companion services, native frame sharing and
VideoToolbox decoder remain present. The Audi branch's firmware builder,
bootstrap export, QNX6 reader, diagnostic-shell documentation, Kanzi shader
extraction, EAL rendering and FCC rotary-controller input are included.

Firmware profiles support Volkswagen (`vw`), Audi A3 (`audi-a3`) and Porsche
(`porsche`). Existing `MHI2_FIRMWARE_META` and Audi's `MHI2_METADATA` both supply
metadata. Porsche retains its fascia, feature configuration and 408 × 448
cluster viewport; VW retains touch and ABT keyboard 13; Audi retains its
non-touch MMI controller and FCC keyboard 1. Audi's launcher selects 12 MHz;
the existing Porsche/VW clock choice remains unchanged. Existing workspace
Porsche launchers can continue passing metadata directly.

Graphics opcode 128 had two independent definitions. Client vertex uploads
remain opcode 128 and inline index draws remain 129. The Audi maximum-EBO-index
query now uses 138. For already-prepared Audi media, the host still accepts the
old 12-byte opcode-128 query; vertex uploads have at least 24 bytes. This keeps
both older bridge formats usable. The merged guest handles client vertices
with either host EBOs or inline indices and retains the thread-safe transport.

All 75 shader entries remain available. Two masked-compositor hashes have
conflicting translations in the original branches. The host uses the Audi
versions when `MHI2_FIRMWARE=audi-a3`; otherwise it preserves the Porsche/VW
versions. Neither translation is silently discarded or claimed more accurate.
Kanzi source-cache loading and all five baked-blending modes remain available.

## Validation on macOS, 2026-10-07

- QEMU `arm-softmmu`, ARM guest bridges and host graphics bridge build.
- Porsche K5126 reaches QNX userspace, repeats after reset, and rejects corrupted
  stage2 authentication in the negative test. Four-core configuration retained.
- Production key/touch/encoder routing and desktop controls pass for all three
  profiles; 10 configuration/media/USB ownership checks and 24 service checks pass.
- I2C, SMMU, HOST1X, GR2D, PCIe, RCC/MOST backpressure, audio DMA and IRQs,
  clock/timer continuity, IOC/MC handshakes, ATA mailbox and Android USB checks pass.
- All 75 shader entries compile. Original Porsche/VW blending (36 checks),
  18 Porsche fragment programs, both masked-compositor profile variants,
  cross-process native frames, and old/new vertex/index wire formats pass pixel tests.
- KZB shader-reference extraction and two audio queue/host checks pass.
- VideoToolbox reports hardware decoding and passes pixel, crop, visibility,
  ownership, malformed-input and cleanup checks.

Graphics pixel tests used Mesa llvmpipe; the H.264 decode test used VideoToolbox
hardware. Full Audi/VW firmware UI boots were not repeated on this Mac because
the corresponding firmware media is not available locally. This merge preserves
and tests the combined implementations; it does not establish physical-car
Android Auto/cluster completion or resolve previously documented emulator limits.
