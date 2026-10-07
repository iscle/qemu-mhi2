# Linux and macOS host backends

The shared emulator, firmware profiles and wire protocols expose the same
operations on both hosts. Host facilities use the following backends:

| Operation | Linux | macOS |
| --- | --- | --- |
| GLES HMI/cluster rendering | Mesa EGL/GLES | Mesa EGL/GLES |
| Native NvSS H.264 decoding | FFmpeg VA-API or software | FFmpeg VideoToolbox or software |
| Asynchronous MOST H.264 encoding | FFmpeg VA-API or software | FFmpeg VideoToolbox or software |
| Announcement playback and active microphone | PipeWire, or SoX default device | SoX CoreAudio default device |
| Physical USB phone passthrough | QEMU libusb | QEMU libusb |
| Android Studio AVD link | ADB, default `~/Android/Sdk` | ADB, default `~/Library/Android/sdk` |
| Map/speech/media disk preparation | Portable sparse copying | Portable sparse copying |

This is emulator host parity, not a claim that every firmware service or a
physical Android Auto phone connection has been completed. Existing firmware
limitations still apply equally; for example the audio mailbox currently taps
announcement slots, not every native media output slot. No Pico checkout,
firmware, SDK or hardware is required.

## Video and encoding

`MHI2_DECODER=auto` (default) first requests the host hardware decoder. A driver
that lacks the stream's H.264 profile triggers a logged software retry before
any frame has been published. `MHI2_DECODER=hardware` prohibits this fallback;
`MHI2_DECODER=software` selects software explicitly. On Linux,
`MHI2_DECODE_DEVICE=/dev/dri/renderD129` selects a particular VA-API device and
requires hardware. Do not combine a device override with software mode.

The existing guest NvSS adapter must be present in the selected experimental
media to use native video forwarding. This change does not redirect additional
firmware functions; the ordinary Audi boot test alone does not validate a
firmware video application.

Both paths share Annex-B/AVCC parsing, parameter-set updates, bounded dimensions
(up to 2048×2048), packet limits (1 MiB), four owner-isolated decoder slots,
source cropping, destination placement, visibility and teardown. Frames go to
the guest-selected center or cluster plane. Cluster video never substitutes a
copy of the center display. The actual decoded frame format determines the
reported hardware status. FFmpeg's H.264 VideoToolbox path requests hardware;
software operation is a separate, explicitly logged decoder session.

`MHI2_ENCODER=auto` probes a real frame before selecting VA-API on Linux or
VideoToolbox on macOS. If unavailable, it probes `libx264`, then `libopenh264`.
The selected codec is passed to the bounded asynchronous worker, with its
matching low-latency options. VideoToolbox uses `allow_sw=0`. Unsupported
explicit requests fail instead of silently selecting software:

```sh
MHI2_ENCODER=hardware MHI2_DECODER=hardware bash /path/to/build/run.sh
```

Encoder modes also include `vaapi`, `videotoolbox` and `software`.
`MHI2_VAAPI_DEVICE` selects a Linux encoding device. Encoder and decoder devices
are independent. Software codecs provide the same guest operations, with
different performance. These settings do not force software GLES rendering.

## Dependencies and build

Use the [Linux/macOS installation instructions](AUDI_A3.md#reproduce-on-another-computer).
The host bridge requires FFmpeg development packages for `libavcodec`,
`libavutil` and `libswscale`, in addition to EGL/GLES. FFmpeg must include an
H.264 decoder; hardware decoding requires its native `h264` decoder and the
appropriate hardware acceleration support. The `ffmpeg` executable needs at
least one of the supported H.264 encoders. Distribution builds and GPU drivers
can disable these codecs even when the underlying GPU supports them.

For a custom FFmpeg installation, export its `lib/pkgconfig` directory in
`PKG_CONFIG_PATH` when building. The host executable records the selected
libavcodec library directory as an rpath. The tested local decoder libraries
were built separately from a full-history FFmpeg checkout at tag `n8.0.1`
(commit `894da5ca7d742e4429ffb2af534fcda0103ef593`); no system packages were replaced.

The builder enables libusb explicitly on both platforms. The AVD path honors
`ANDROID_HOME`, then `ANDROID_SDK_ROOT`, before using the host-specific default.
The older VW media helper also accepts `MHI2_WORKSPACE`; it uses the in-tree
QNX filesystem reader and portable LZO/sparse-copy helpers.

Microphone capture still begins only when the guest activates its input queue
and stops after the queue is idle. macOS may require microphone permission for
the terminal/application running the emulator. Device access is subject to the
host's ordinary audio and USB permissions.

## Validation

Run without proprietary firmware:

```sh
python3 tools/mhi2/glforward/check_video.py
python3 tools/mhi2/check_encoder.py
python3 tools/mhi2/check_audio.py
python3 tools/mhi2/check_emulator_config.py
```

The video test generates real H.264 access units, checks decoded pixels for
Annex-B and AVCC, then exercises the actual bridge wire protocol and published
center/cluster images, crop, hiding and restoration. It checks owner isolation,
slot/dimension limits and malformed input. On Linux it also verifies that an
explicit invalid hardware device returns failure without publishing frames.
The default test uses software codecs for repeatable CI. Set
`MHI2_DECODER=hardware` to exercise physical hardware strictly.

Linux passed these tests and an Audi boot with the updated bridge. The encoder
round trip and deliberately stalled worker tests passed. Both local VA-API
render devices rejected H.264 profiles, so local video tests exercised actual
software frames and automatic fallback; hardware success is not claimed.
The native English Audi radio/menu and rotary controls remain functional.

Local macOS validation on 2026-10-07 used macOS 26.5.1 on Apple Silicon,
Mesa 26.2.4 and libavcodec 62.28.102. A fresh QEMU build with libusb enabled,
the portable host/ARM guest bridge builds, image-preparation checks, all three
firmware input profiles and the codec/audio tests above passed. All 75 shader
entries compiled; client-array/EBO pixel tests and device/transport checks passed.

Strict `MHI2_DECODER=hardware` passed both Annex-B and AVCC decoding and the
center/cluster wire-protocol tests, reporting actual hardware frames. Strict
hardware encoding selected `h264_videotoolbox` with software fallback disabled;
the asynchronous worker delivered all 30 test frames with no drops and its MPEG-TS
output decoded successfully. SoX CoreAudio opened the default playback device,
accepted a short silent PCM stream and closed successfully. This checks device
access, not audible output or microphone recording, which remain unverified.
GLES pixel tests used Mesa llvmpipe; GPU-accelerated GLES is not claimed.

The fresh executable also booted Porsche K5126 into QNX userspace, repeated
after reset and rejected the intentionally corrupted stage2 fixture. A graphical
session rendered the native Porsche radio screen in the macOS viewer and
received the HOME key through RCC; `pidin info` reported all four Cortex-A9
MPCore processors. Audi/VW
full firmware boots have not been repeated on this Mac. The workflow remains
blocked by the account billing/spending limit; these results are from local
execution, not a successful GitHub Actions run.
