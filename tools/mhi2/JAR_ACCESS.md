# Accessing the HMI JAR directory in the emulator

You can inspect `/eso/hmi/lsd/jars/` and make it writable in the QEMU
emulator. The commands below use the launcher's diagnostic QNX shell and
apply to Linux and macOS hosts. This is an emulator guide, not a procedure
for a physical head unit.

There are two separate steps to installing a new JAR: transfer its bytes
into the guest, then copy it into the app filesystem. **The launcher does
not currently provide a general host-to-guest file upload command.** The
copy instructions below assume the JAR is already inside QNX; putting it
in your computer's `/tmp` does not transfer it.

## Open the guest shell connection

Start your prepared build normally and leave it running:

```sh
# Host Terminal: replace this with your existing build directory.
export MHI2_BUILD="$HOME/mhi2-repro/audi-build"
bash "$MHI2_BUILD/run.sh"
```

In a second host Terminal, follow the guest console:

```sh
tail -f /tmp/mhi2-debug-console.log
```

Wait for `UI_DIAGNOSTIC_SHELL_READY`. In another host Terminal, send a
command to the shell through its FIFO:

```sh
printf '%s\n' 'ls -l /eso/hmi/lsd/jars/' > /tmp/mhi2-shell-commands
```

Read the result in the console log. Other firmware messages may appear
between command output lines. The FIFO accepts shell commands, not file
contents; it is not an upload directory or an interactive Terminal.

Only send commands while the emulator is running and the shell is ready.
A write to a stale FIFO can wait indefinitely when no reader exists; use
Ctrl-C if that happens. Run one emulator session at a time because these
paths are shared between sessions.

## Understand the paths

| Path | Location and purpose |
| --- | --- |
| `/tmp/mhi2-shell-commands` | Host FIFO used to send commands to QNX |
| `/tmp/mhi2-debug-console.log` | Host log containing guest console output |
| `/tmp/Your.jar` in a guest command | File inside QNX, separate from the host's `/tmp/Your.jar` |
| `/eso/hmi/lsd/jars/` | Guest HMI JAR directory |
| `/mnt/app/eso/hmi/lsd/jars/` | Backing directory on the guest app filesystem |

The app filesystem at `/mnt/app` initially mounts read-only. In the tested
Audi image, writing through `/eso/...` could misleadingly report
`No such file or directory`; writing through `/mnt/app/...` reported
`Read-only file system`.

## Copy a JAR that is already inside QNX

The following are **guest QNX commands**. Replace `Your.jar` with your actual
guest filename. Use a new name when adding a file; `cp` can overwrite an
existing JAR.

```sh
ls -l /tmp/Your.jar
mount -uw /mnt/app
cp /tmp/Your.jar /eso/hmi/lsd/jars/Your.jar
ls -l /eso/hmi/lsd/jars/Your.jar
sync
mount -ur /mnt/app
```

You can send each line using the host-side `printf` command shown above.
For example, to remount the app filesystem writable:

```sh
# Host Terminal; the quoted command executes inside QNX.
printf '%s\n' 'mount -uw /mnt/app' > /tmp/mhi2-shell-commands
```

Check each result in the log before proceeding. Finish with `sync` and
`mount -ur /mnt/app` even if the copy fails. The `printf` in these examples
runs on your computer; it does not require a guest `printf` executable.

This procedure does not establish a transfer mechanism. No working generic
SFTP, FTP or shared-folder upload is demonstrated here. The
[COMM Agent Doctor and Trace Terminal](DIAGNOSTIC_SHELLS.md) are diagnostic
interfaces, not a verified way to upload arbitrary JARs.

## Keep changes across emulator launches

The default launcher uses a temporary eMMC snapshot. Guest writes, including
new JARs, disappear when QEMU exits. `sync` flushes the guest filesystem but
does not make that temporary snapshot persistent.

For persistent experiments, stop the emulator first and copy the complete
prepared `media/ui` directory to a new working directory. Keep the original
images intact. From a host Terminal:

```sh
export MHI2_BUILD="$HOME/mhi2-repro/audi-build"  # your existing build
# The destination must not already exist. Copy only once.
test ! -e "$MHI2_BUILD/media/jar-work" &&
  cp -R "$MHI2_BUILD/media/ui" "$MHI2_BUILD/media/jar-work"
```

Check that the copy completed successfully before starting it. Plain `cp`
may allocate sparse image holes, so allow space for the full image sizes.
Boot the working copy with eMMC snapshots disabled:

```sh
MHI2_SNAPSHOT=off bash "$MHI2_BUILD/run.sh" \
  --media "$MHI2_BUILD/media/jar-work"
```

In this mode, guest eMMC writes go directly to the working copy, including
ordinary runtime changes. Use that same command for subsequent writable
sessions. Omitting `--media` returns to the build's original prepared media;
omitting `MHI2_SNAPSHOT=off` makes new changes temporary again. This setting
controls eMMC persistence, not NOR persistence.

## Loading the added JAR

The Audi firmware's `lsd.sh` scans the JAR directory for `.jar` and `.zip`
files and adds them to the Java boot classpath when the HMI starts. A file
copied after startup is not automatically loaded into the running JVM.
Restart the HMI to rebuild its classpath; a full emulator restart also works
if you have saved the file in a persistent working copy as described above.

Adding a JAR to the classpath does not automatically execute its entry point
or activate an OSGi bundle. That depends on the firmware's startup/bundle
configuration, and the classes must be compatible with its J9 runtime.

## Linux and macOS validation

The host commands use shell features and utilities available on both Linux
and macOS. The filesystem commands execute inside QNX, so their syntax does
not depend on the host OS.

Directory access, read/write remounting, copying an existing guest JAR,
removing the test copy and restoring read-only mode were verified with Audi
P5089 on Linux in a disposable snapshot. That test did not transfer a new
host JAR, validate activation of a custom JAR or test persistence across
boots. This specific JAR workflow has not been run end-to-end on macOS;
macOS emulator/backend evidence is documented separately in
[HOST_PARITY.md](HOST_PARITY.md).
