# Transferring files from Linux or macOS into QNX

Use `tools/mhi2/guest_file.py` to copy a file from your computer into the
running MHI2 emulator. It supports binary and text files, including JARs,
configuration files, images and scripts, and accepts an explicit guest
filename. It uses the existing diagnostic console: no guest networking,
FTP/SSH server, shared folder, QNX SDK or extra host Python package is needed.

This guide is for the QEMU emulator, not a physical head unit. The destination
must be on a writable guest filesystem, and its parent directory must exist.
An immutable boot-image path cannot be made writable just by uploading to it.

## 1. Start the emulator and wait for its shell

In a host Terminal, start your existing build normally:

```sh
# Replace this with your existing Audi, VW or Porsche build directory.
export MHI2_BUILD="$HOME/mhi2-repro/audi-build"
bash "$MHI2_BUILD/run.sh"
```

Keep the emulator running. In a second host Terminal:

```sh
tail -f /tmp/mhi2-debug-console.log
```

Wait for `UI_DIAGNOSTIC_SHELL_READY`. You can leave this Terminal following
output, or press Ctrl-C to stop `tail` without stopping the emulator. Run
only one emulator session at a time: its helpers share FIFO and log paths.

## 2. Upload a file from the host

Run the following **on your Linux or macOS host**, from the repository root
(the directory containing `tools/mhi2`). Both filenames are explicit:

```sh
python3 tools/mhi2/guest_file.py \
  "$HOME/Downloads/Your.jar" /tmp/Your.jar
```

The first argument is an existing **host file**. The second is the complete
**guest destination filename**, not just a directory. On success, the helper
reports the number of bytes copied and confirms the block checksums and
guest file size. `/tmp/Your.jar` now exists inside QNX.

The same command works with any file type; no extension determines how the
transfer works. For example:

```sh
python3 tools/mhi2/guest_file.py \
  "$HOME/Documents/settings.json" /tmp/settings.json
```

Quote filenames containing spaces. Existing destinations are refused by
default. To deliberately replace a regular file, add `--overwrite`:

```sh
python3 tools/mhi2/guest_file.py \
  "$HOME/Documents/settings.json" /tmp/settings.json --overwrite
```

Uploaded files default to mode `0644`. Use `--mode 0755` for an executable
script or a binary compatible with this guest. Uploading a host executable
does not make it compatible with ARM/QNX. The helper refuses directories,
existing symlinks and existing special files as replacement targets.

## 3. Choose the destination and make its filesystem writable

The host and guest have separate filesystems:

| Path | Meaning |
| --- | --- |
| `$HOME/Downloads/Your.jar` in the upload command | Source on your computer |
| `/tmp/Your.jar` as the upload destination | File in QNX RAM; lost when the guest stops |
| `/eso/hmi/lsd/jars/Your.jar` | Guest HMI file, backed by `/mnt/app/eso/hmi/lsd/jars/Your.jar` |
| `/mnt/app/...` | Guest app filesystem on emulated eMMC; initially read-only |
| `/tmp/mhi2-shell-commands` | Host FIFO for sending QNX shell commands |
| `/tmp/mhi2-debug-console.log` | Host log containing QNX console output |

The upload helper does not remount filesystems or create parent directories.
To send a guest shell command, use this pattern in a **host Terminal**:

```sh
printf '%s\n' 'ls -l /tmp/Your.jar' > /tmp/mhi2-shell-commands
```

Read the result in `/tmp/mhi2-debug-console.log`. The quoted command executes
inside QNX; `printf` runs on the host. Do not use the FIFO while an upload is
in progress, since other input would interfere with the transfer protocol.
A write to a stale FIFO can hang when no emulator is reading it; press Ctrl-C
and start the emulator first. The upload helper itself checks for a reader
and has a timeout for each guest acknowledgement.

For example, to upload a configuration file under a new directory on the app
filesystem, send these commands, checking each result in the log:

```sh
# Host Terminal: these quoted commands run in QNX.
printf '%s\n' 'mount -uw /mnt/app' > /tmp/mhi2-shell-commands
printf '%s\n' 'mkdir -p /mnt/app/my-files' > /tmp/mhi2-shell-commands

# Host Terminal, from the repository root:
python3 tools/mhi2/guest_file.py \
  "$HOME/Documents/settings.json" /mnt/app/my-files/settings.json

# Restore the app filesystem to read-only after the transfer.
printf '%s\n' 'sync; mount -ur /mnt/app' > /tmp/mhi2-shell-commands
```

Use the mount point that actually backs your chosen destination; remounting
`/mnt/app` does not make other filesystems writable. Finish by restoring the
mount even if the upload fails. In Audi P5089, writing through `/eso/...`
while `/mnt/app` is read-only can misleadingly report `No such file or
directory`; the backing `/mnt/app/...` path reports `Read-only file system`.

QNX `/tmp` aliases `/dev/shmem`. It accepts files but does not support creating
subdirectories in the tested firmware. Upload a file directly to `/tmp/name`,
or use a writable disk filesystem if you need a directory tree. The helper
copies one file per invocation; it does not recursively upload directories.

## Example: install an HMI JAR directly from your computer

You do not need to stage the JAR in guest `/tmp`. Once the emulator shell is
ready, run these commands on the host, from the repository root:

```sh
printf '%s\n' 'mount -uw /mnt/app' > /tmp/mhi2-shell-commands
# Check the console log for a successful remount before continuing.
python3 tools/mhi2/guest_file.py \
  "$HOME/Downloads/Your.jar" /eso/hmi/lsd/jars/Your.jar
printf '%s\n' 'ls -l /eso/hmi/lsd/jars/Your.jar' > /tmp/mhi2-shell-commands
printf '%s\n' 'sync; mount -ur /mnt/app' > /tmp/mhi2-shell-commands
```

Add `--overwrite` only when intentionally replacing an existing JAR.
The Audi firmware's `lsd.sh` scans this directory for `.jar` and `.zip` files
and adds them to the Java boot classpath at HMI startup. A copied JAR is not
automatically loaded by the running JVM. Restart the HMI to rebuild the
classpath; a full emulator restart also works if the file was installed on
persistent media as described below.

Classpath presence does not automatically execute a JAR's entry point or
activate an OSGi bundle. That depends on the firmware startup configuration,
and its classes must be compatible with the firmware's J9 runtime. Likewise,
other uploaded files only affect a service when that service reads them.

## Keep disk changes across emulator launches

The default launcher uses a temporary eMMC snapshot: uploaded disk files and
other guest writes disappear when QEMU exits. `sync` flushes guest writes but
does not make this snapshot persistent. **Guest `/tmp` is RAM and never
persists across boots**, even when eMMC snapshots are disabled.

For persistent experiments, stop the emulator, copy the complete prepared
`media/ui` directory to a new working directory, and boot that copy with eMMC
snapshots disabled **before uploading**. Keep the original prepared images.
From a host Terminal:

```sh
export MHI2_BUILD="$HOME/mhi2-repro/audi-build"  # your existing build
# The destination must not exist. Copy only once, while QEMU is stopped.
test ! -e "$MHI2_BUILD/media/jar-work" &&
  cp -R "$MHI2_BUILD/media/ui" "$MHI2_BUILD/media/jar-work"
```

Check that the copy succeeded before proceeding. Plain `cp` may allocate
sparse image holes; allow space for the full image sizes. `jar-work` is just
an example directory name and can hold any guest file changes.

```sh
MHI2_SNAPSHOT=off bash "$MHI2_BUILD/run.sh" \
  --media "$MHI2_BUILD/media/jar-work"
```

Upload to an eMMC-backed guest path such as `/mnt/app/...`. In this mode,
all guest eMMC writes, including ordinary runtime changes, go directly to
the working copy. Use the same launch command for subsequent writable
sessions. Omitting `--media` returns to the original prepared media;
omitting `MHI2_SNAPSHOT=off` makes new disk changes temporary again. This
setting controls eMMC persistence, not NOR persistence.

## Transfer behavior, limits and validation

The helper base64-encodes blocks and uses the firmware's GNU awk to decode
them. It checks each block's decoded byte count and Adler-32 checksum, then
checks the completed guest file size. It writes temporary sibling files and
moves the completed payload to the requested destination only after those
checks pass. Adler-32 detects transmission errors; it is not a cryptographic
signature or an independent hash of the file read back from disk.

This route prioritizes working with existing firmware tools. The serial
console limits speed to roughly a few kilobytes per second of encoded data;
large transfers can take minutes per megabyte. It is suitable for small JARs,
configuration files and scripts, not multi-gigabyte map or firmware archives.
The console log can contain the encoded file contents. No FTP/SFTP or shared
folder setup is required or established by this guide. The
[COMM Agent Doctor and Trace Terminal](DIAGNOSTIC_SHELLS.md) are separate
interfaces and are not used by the upload helper.

On failure, inspect the console log. The helper prints its temporary guest
filenames at startup; an interrupted transfer can leave those files behind.
Wait for queued console input to finish before sending other commands, then
remove those exact temporary files if no longer needed. Do not stop QEMU
during an upload. Default acknowledgements time out after 30 seconds;
`--timeout 60` allows more time per operation on a slow host.

The helper uses Python's standard library and POSIX facilities available on
Linux and macOS. Its guest commands run in QNX on either host. With Audi
P5089 on Linux, uploads of a JAR, binary data covering all 256 byte values,
an empty file and an executable script were verified. After QEMU stopped,
the files were read back from the working eMMC image and matched the host
sources byte for byte, with the requested permissions. Tests also covered
filenames containing spaces, quotes and shell metacharacters, replacement
opt-in, read-only destinations and a stopped emulator.

Run `python3 tools/mhi2/check_guest_file.py` for host-side regression checks
of the decoder, path validation and refusal to install corrupted transfers.
Decoder tests need GNU awk on the host (`gawk`); uploading itself does not.

This specific transfer workflow has not been run end-to-end on macOS.
Other firmware profiles need the same diagnostic shell
and `/armle/usr/bin/gawk` and `wc`; the helper checks those utilities before
starting. General macOS emulator/backend evidence is documented separately
in [HOST_PARITY.md](HOST_PARITY.md).
