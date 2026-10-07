# LSD diagnostic shells

Investigation of the original Audi A3 P5089/MU1326 and VW K3342/MU1427
`/ifs/lsd.jxe` payloads, 2026-10-07. The Java classes were converted with the
existing JXE-to-JAR tooling and inspected with CFR 0.152. Decompiled classes
and firmware binaries are kept outside Git.

## Findings

| Interface | Transport / activation | What it provides |
| --- | --- | --- |
| COMM Agent Doctor | Plain text, default `127.0.0.1:21021`; started by the COMM OSGi activator | COMM/DSI snapshots, service/proxy/stub information, error logs and generated diagnostic reports |
| Java Trace Terminal | JVM `System.in` / `System.out`; tracing `CommandShellBackend` must be selected | Trace entity/filter controls, registered callback invocation, optional remote file transfers |
| Native trace command shell | Separate C++ `libCommandShellBackend.so`; configuration contains TCP port `39999`, **disabled** | Native trace controls and callback invocation; not the Java terminal whose banner occurs in LSD |
| Native trace logger | Binary tracing protocol, configured TCP port `21002` | Trace traffic and callbacks; production file-transfer configuration declares a two-file allowlist |

The banners do not establish general filesystem or operating-system shell
access. In particular, an external computer cannot reach a loopback-only
Doctor socket just by connecting to the head unit's Ethernet address.

## Doctor: activation and commands

`de.esolutions.fw.comm.agent.config.CommConfigDoctor` defaults to:

```json
{"enabled": true, "host": "localhost", "port": 21021, "useInfoProvider": true}
```

Overrides come from `framework.json` at `comm_settings.java.doctor`.
`AgentActivator.start()` reads those settings and starts
`SocketDoctorShellServer`, which binds a `ServerSocket` to the resolved host.
The socket handler immediately prints the welcome banner and reads UTF-8
newline-delimited commands. No password exchange appears in this handler.

With an existing TCP client on the head unit, or a forwarding connection that
can reach its loopback interface, connect to `127.0.0.1:21021`. For example,
**if netcat is available in that environment**:

```sh
nc 127.0.0.1 21021
```

The tested firmware does not ship `nc` or `telnet` at the usual `/bin` or
`/usr/bin` paths. The command above is a protocol example, not a claim that
netcat is preinstalled. Commands to start with:

```text
help
as
ai
help dr
exit
```

`as` takes a snapshot; `ai` describes it. `help` prints the actual registry,
including the DSI commands registered by `de.esolutions.fw.dsi.AdapterActivator`.
Useful commands include:

- `client_info`, `proxy_info`, `stub_info`, `service_registry`,
  `service_directory`, and their error-report counterparts.
- `dsi_snapshot` (`dsis`), `dsi_providers` (`dsipi`),
  `dsi_dispatchers` (`dsidi`), and `dsi_diagnosis_report` (`dsir`).
- `diagnosis_report` (`dr`), with `brief`, `size`, `zip` and an optional output
  filename. This last option writes **generated report contents** through
  `FileOutputStream`; it is not a general arbitrary-content upload command.

The inspected registry has no general `cat`, filesystem `ls`, download,
upload, or operating-system command execution handler. Normal Java/QNX file
permissions still apply to report output. No report-writing command was used
for this investigation.

There is also a trace callback named **`comm_doctor`**.
`AgentCallbacks.DoctorShellCommand.executeTraceCallback()` feeds its argument
to the same Doctor command handler and emits the result on the Doctor trace
channel. This can be useful with an existing compatible trace client even
when the direct Doctor socket is reachable only locally. Find the callback
in the trace entity list and invoke it with `help`; the entity path/id and
trace filters depend on the client. This callback path was inspected
statically, not exercised through the external binary trace protocol.

## Java Trace Terminal: what `get` and `put` mean

The banner comes from
`de.esolutions.fw.util.tracing.backend.CommandShellBackend.run()`.
It constructs a reader on `System.in`; this Java class does not open a TCP
listening socket. `TraceCore` registers it as `CommandShellBackend`. It can
also be the default console for a tracing core whose class is `server` when
no other backend is enabled and `core.useDefaultConsole` is true.

Configuration is selected by `TraceConfigProvider` (`tracing.json`, with the
`ipl.config.dir` / `ipl.config.dir.tracing` JVM properties controlling the
location). `TraceConfig` overlays the selected process on the core-class
`default` entry. The production HMI entry selects `RemoteConnectBackend` to
connect to the native trace server. Merely typing into the boot serial console
does not route input to this Java backend.

The Java terminal accepts `help`, `ls`, `channel`, `thread`, `wait`, `call`,
`start`, `stop`, `stat`, `mute`, `unmute`, `get`, `fstat`, and `put`.
Here `ls` lists **trace entities**, not filesystem directories.
`call` invokes an already registered trace callback, not an OS command.
`q` / `quit` requests application shutdown through the trace listener.

The transfer commands require an instantiated `FileTransferManager` and a
connected transport peer. `TraceConfigFileTransfer` defaults to disabled;
`fileTransfer.enabled=true` creates the manager. Without it the terminal
prints `Could not found FileTransferManager, commando skipped.`

- `get <remote-path>` requests a remote download and saves it under the local
  download directory (default `/tmp/`).
- `fstat <remote-path>` requests remote file status.
- `put <local-path>` reads a local file and sends it to the remote peer.

Thus, running this terminal *on* the head unit does not turn `get` into a local
filesystem reader. Direction and access depend on the peer. Java's
`RealTransferFile` uses `RandomAccessFile`; its implementation is not evidence
that the separate native server accepts unrestricted paths.

## Native production tracing configuration

Both supplied `MMX2/efs-sys/70/default/efs-system.img` images contain:

```json
"CommandShellBackend": {
  "enabled": false,
  "enableListenSocket": true,
  "port": 39999
}
```

The native server's file-transfer configuration declares:

```json
"fileTransfer": {
  "enabled": true,
  "whiteList": [
    "/mnt/app/version_info.txt",
    "/mnt/app/eso/ems_tables.zip"
  ]
}
```

The allowlist is configuration evidence; its native enforcement and transfer
behavior have not been tested here. No unrestricted transfer or bypass has
been demonstrated. The native `libtracing.so` also contains the `whiteList`
configuration key and its native file-transfer implementation.

The runtime `framework.json` maps the `logger` transport to big-endian binary
TCP at port `21002`, with wildcard address `0.0.0.0`. This is distinct from
the disabled text shell at `39999`; telnet commands are not its protocol.
Physical-unit reachability still depends on its active interfaces and network
configuration.

## Evidence and limits

Audi P5089 in the emulator, with unchanged LSD and production tracing/COMM
configuration, reports these listening sockets:

```text
127.0.0.1.21021  LISTEN
*.21002         LISTEN
```

There is no listener on `39999`. These observations use the existing emulator's
diagnostic console; they do not establish a new way to obtain console access
on an unmodified physical head unit.

A temporary ARM TCP client in guest `/tmp` connected to Doctor and sent
`help`, `as`, `ai`, and `exit`. The firmware returned the original welcome
banner, all 19 registered commands, and a snapshot containing 75 proxies,
75 stubs, 17 clients and 157 service handlers; the client exited successfully.
The test did not change LSD, tracing/COMM configuration, or persistent media.
The client was needed because the guest has no preinstalled netcat/telnet.
Its source and transcript are retained in the local analysis directory below.

The Doctor shell and its default configuration classes are byte-identical
between the converted VW and Audi payloads. The Java terminal differs in its
UTF-8 conversion helper for callback arguments; its transport and command
behavior are otherwise the same in the inspected decompilation. VW runtime
sockets were not checked in this investigation.

Original JXE SHA-256:

| Firmware | SHA-256 |
| --- | --- |
| Audi P5089 | `6aae98bbbcdd68b03fef4499d31a90205dcf9954459f3ef9a3df02ee937b4423` |
| VW K3342 | `f46290b4a33b98d957170f6207073ad329316902a87cd4c27da4168548ad911a` |

Local detailed analysis and logs are under
`/home/iscle/mhi2-audi-port/analysis/diagnostic-shells/`.
