# Native Windows build slice (draft)

This source-only candidate targets `iscle/qemu-mhi2` at `78b8cb2d34d7c08f34fc33c0cab1ab960edfc000`. It adds a Windows build-only switch for the install-tree link step and a native ANGLE/libuv path for the existing GL protocol and encoder. The normal POSIX branches remain in place.

## Graphics bridge build

Use an MSYS2 MINGW64 shell with the matching MinGW-w64 GCC, `pkgconf`, ANGLE (`angleproject` pkg-config module), libuv, FFmpeg development libraries (`libavcodec`, `libavutil`, `libswscale` pkg-config modules), and EGL/GLES headers. FFmpeg is a runtime executable for the encoder and must be installed separately; set `MHI2_ENCODER_EXE` to its path when it is not `ffmpeg.exe` on PATH. No firmware or private Research files are required to compile.

From the repository root:

```sh
bash tools/mhi2/glforward/build_windows.sh [output-directory]
```

The script compiles the GL host and asynchronous encoder fixture using `pkg-config --cflags --libs angleproject libuv libavcodec libavutil libswscale`. If this is an additive overlay outside a full checkout, set `MHI2_UPSTREAM_ROOT` to the checkout so unchanged shader headers are found. The `real` fixture exercises software encoding; inspect its reported delivered/dropped frame counts because the fixed 40 ms submit interval can overload slower hosts. The `stalled` fixture needs `MHI2_ENCODER_EXE` set to a test child that does not read stdin; the C fixture does not create that child itself. It times both the close request and the actual `encoder_stop` join, and fails on encoder-worker error or a full stop over three seconds. A successful compile or synthetic fixture does not establish guest graphics or stock HMI acceptance.

## QEMU build-only configuration

Use the normal upstream Windows/MSYS2 configure/build flow with its existing target and feature options. If the host cannot create install-tree symlinks, set `QEMU_BUILD_ONLY_NO_INSTALL_LINKS=1` for the configure post-configuration step. This skips only the install-bundle symlink script and explicitly does not create or validate a distributable install tree. Omit the variable for normal install packaging. Existing behavior is unchanged when unset and on non-Windows hosts.

The retained private build used GCC 16.2.0, Meson 1.12.0, Ninja 1.13.2 and flags `--target-list=arm-softmmu --disable-docs --disable-sdl --disable-gtk --disable-fuse --disable-werror --disable-rust --enable-libusb --enable-slirp`, with slirp resolved from the local libslirp 4.7.0 fallback. Current-upstream configuration auto-detected optional curl, SSH, GnuTLS/OpenSSL, JPEG/PNG and LZO dependencies; the resulting runtime bundle contained 40 DLLs, versus 15 in the earlier environment. Therefore these builds are not a controlled performance comparison, and any binary distribution needs a complete pinned dependency/license audit.

## Existing Windows evidence versus fresh validation

Previously retained on Windows: QEMU current-source native build (2,092 Ninja actions), restricted-PATH version and MHI2 registration, firmware-free wireless/modem initialization; ANGLE/D3D11 EGL and shader checks; GL protocol/frame tests; asynchronous FFmpeg encoding (30/30 frames delivered) and stalled-child cleanup. Evidence is tied to the private source manifest and is not repeated here.

The GL bridge and encoder fixture were rebuilt from the assembled clean-base public candidate. The complete QEMU executable was not rebuilt from this isolated diff; ordinary install packaging and POSIX graphics/encoder regression runs remain open. No binaries, DLLs, firmware, maps, logs or captures are distributed here.

Fresh Windows validation used MSYS2 MINGW64 (GCC 16.2.0, ANGLE 2.1.r25748.890b5d8f-6, libuv 1.52.1-1, FFmpeg 9.0.1-3). Both executables compiled, ANGLE/D3D11 shader checks passed 75/75, and clean stdin EOF returned 0. An earlier fixed-cadence real-encoder run delivered 3/30 frames and dropped 27; a subsequent current-source run delivered 30/30 with zero drops. This timing-sensitive discrepancy still needs stable-load repetition. The fixture verifies MPEG-TS packet alignment/sync bytes, not decoding or continuity counters. Full stop measured 31 ms for the real-encoder run and 33 ms with a non-reading child; captured child PIDs were absent after each process exited. Closing host stdout during an active encoder returned exit 1 after common cleanup, and the captured child PID was absent. POSIX runtime and actual guest integration were not rerun for this public candidate.
