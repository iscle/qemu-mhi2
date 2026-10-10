# Experimental SKG13 profile

This profile recognizes `MHI2_ER_SKG13_P4526` with MU `1440` and supplies experimental UI/input geometry: main display 1280×640, requested cockpit geometry 1280×480, and a 2×2 touch-wire divisor. The geometry/input path is experimental; physical-unit behavior is not verified. Clock selection is unknown. This profile does not enable firmware building, bootstrap compatibility, or a supported SK boot workflow.

The existing POSIX launcher accepts the profile ID as `--firmware` and requires a separately prepared media directory. From the repository root, the experimental selection would be:

```sh
MHI2_SKODA_INPUT_EXPERIMENT=1 \
MHI2_SKODA_OSCILLATOR_12MHZ=0 \
python3 tools/mhi2/launch_ui.py --firmware skoda-skg13-p4526 --media /path/to/prepared-media
```

`MHI2_SKODA_OSCILLATOR_12MHZ` must be set explicitly to `0` or `1`; neither value is declared correct for the target. The extra input-experiment gate must also be set explicitly. Use only media prepared for an emulator experiment.

This command is not a tested SK boot workflow or a native Windows launcher. A Windows session must separately provide the socket/GL adapters; ordinary POSIX launch acceptance remains unverified.

The media directory must contain non-empty `nor.bin`, `iram.bin`, and `emmc.raw`; `metainfo2.txt`; and `ui-manifest.json` with `firmware_profile: "skoda-skg13-p4526"`, `firmware_train: "MHI2_ER_SKG13_P4526"`, `firmware_mu: "1440"`, `emulator_only: true`, and a relative `metadata` path naming that metadata file. The metadata `[common]` section must identify the same release and MU. No archive hash or Research extraction-manifest pin is part of this public profile.

RCC persistence returns only the known release/MU identity and stays read-only for this profile. It supplies no VIN, vehicle coding, or adaptations. This source slice was checked with firmware-free tests on Windows; Linux/macOS runs and normal SK builder/boot acceptance remain open.
