# Experimental MHI2 GLES forwarding

This local prototype replaces the extracted main IFS `libEGL.so` and
`libGLESv2.so` in separate experimental media. It forwards the firmware's GLES
calls to a Mesa surfaceless EGL pbuffer via an optional QEMU MMIO aperture at
`0x5f000000`. The normal media and original firmware archive are preserved.

The starting source was the existing workspace `mib/glforward` prototype.
The current host process communicates over stdin/stdout and filesystem FIFOs;
it does not require a guest network driver or a host listening socket.

Local build: `bash tools/mhi2/glforward/build_local.sh`. This uses the installed
ARM cross compiler and the already extracted main IFS under
`/home/iscle/Downloads/mhi2-analysis/qemu/main-ifs`. The media builder requires
the read-only QNX6 reader at `/home/iscle/Documents/mib_zr/qnx6read.py` and
`k3342-full-debug-nor.bin`, plus the prepared/guest-formatted `emmc-full.raw`.
The generated files are `k3342-gl-debug-nor.bin` and `emmc-gl.raw` in that same
analysis directory. Never modify them while a VM is using them.

`fast_startup_local.py` shortens the experimental HMI's DSI/domain timeouts to
1/5 seconds; its purpose is to let startup advance past missing companion
services during rendering tests. The original settings are 20/120 seconds.

`debug_shell_local.py` launches the current diagnostic image. Set `MHI2_GL=1`,
`MHI2_DEBUG_NOR` and `MHI2_EMMC` to select experimental media. It reads shell
commands from `/tmp/mhi2-diag-commands.txt`; the image must contain the diagnostic
startup script. Set `MHI2_TIMEOUT` for the host wall-clock deadline. Logs are
saved under the analysis directory's `qemu/runs`.

The host writes `/tmp/glhost_frame.ppm`, shader source captures and diagnostic
logs. Raw command capture is opt-in with `MHI2_GL_CAPTURE=/tmp/mhi2-gl-stream.bin`;
`MHI2_GL_TRACE=1` enables per-command logging and `MHI2_GL_DUMP_CLIENTS=1`
enables duplicate per-client frame dumps. These expensive diagnostics are off
during normal use. Production RCC key-panel input now operates the
radio UI; see [RCC_INPUT.md](../RCC_INPUT.md) for the mouse viewer and evidence.
Multiple contexts/processes, EGL image sharing and many extension entry points
remain incomplete.

The native display manager submits precompiled NVIDIA shaders. `known_shaders.h`
contains experimental GLSL translations selected by binary length and FNV-1a
hash. Vertex arithmetic follows disassembly; fragment blending is reconstructed
and does not reproduce FX10 quantization. Unknown binaries fail compilation.
Original captured binaries are in the analysis directory's `qemu/shaders`.
The Java HMI submits its own GLSL source, which is forwarded unchanged.

References: [Grate shader decoder](https://github.com/grate-driver/grate/blob/master/src/libcgc/shader.c),
[fragment ISA](https://github.com/grate-driver/grate/blob/master/docs/fragment-shader-isa.md),
[NVIDIA EGL API interface](https://github.com/Project-Google-Tango/vendor_nvidia_jxd_src/blob/master/tegra/gpu/drv/drivers/khronos/egl/interface/partner/eglapiinterface.h).

Native cockpit bring-up now forwards the display manager's EGLImage to the host
encoder and back to the original MOST driver. See [MOST.md](../MOST.md) for
transport evidence and remaining rendering limitations. Navigation shader
translations additionally cover solid fill, vertex color, textured source-over,
linear fog and extruded road geometry. `check_navigation_shaders.py` loads the
captured native binaries through the bridge and checks rendered fog/alpha pixels
against the decoded four-operand Tegra MAD arithmetic. Override
`MHI2_SHADER_CAPTURE` and `MHI2_GLHOST` to select captures and renderer binaries.

Graphics clients retain separate host state by process. The native display
manager identifies its physical output window, so only its swaps update the
head-unit viewer file. Per-client captures remain available for diagnostics;
the cockpit window exclusively consumes the decoded MOST stream.
