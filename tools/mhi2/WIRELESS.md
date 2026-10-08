# Wi-Fi and Bluetooth

The soldered Marvell 8787 on SDMMC2 implements the SDIO register/packet
interfaces used by the original QNX drivers. Wi-Fi uses QEMU's `NetClientState`;
Bluetooth can use a QEMU `CharFrontend` carrying standard H4 HCI packets.
These paths do not patch the firmware's Wi-Fi driver, supplicant or Bluetooth
stack. They are shared by the Audi, VW and Porsche machine profiles.

## Wi-Fi

The default launcher creates an open infrastructure access point named
`QEMU Wi-Fi`, BSSID `52:54:00:87:87:01`, on channel 6. The AP terminates the
virtual wireless link and bridges Ethernet frames to the selected QEMU network
backend. It is not a host Wi-Fi scan or an RF transmitter. Select it using the
firmware's Wi-Fi **client** settings, rather than its hotspot settings.

Run these options with any generated `run.sh`:

```sh
# Default: NAT, DHCP, DNS through libslirp; no host privileges required.
bash /path/to/porsche-build/run.sh --wifi user

# Change the advertised name and add a normal QEMU host forwarding rule.
bash /path/to/porsche-build/run.sh --wifi-ssid 'Emulator network' \
  --wifi 'user,hostfwd=tcp:127.0.0.1:2222-:22'

# Disconnect Wi-Fi (no AP is reported by scans).
bash /path/to/porsche-build/run.sh --wifi none

# Example for an already configured Linux TAP interface.
bash /path/to/porsche-build/run.sh --wifi 'tap,ifname=tap0,script=no,downscript=no'
```

`--wifi` takes the backend/options portion of QEMU `-nic`; the launcher supplies
`model=mv8787-sdio`. `user`, socket, TAP and other backends follow ordinary QEMU
host support and privilege requirements. Host forwarding only reaches services
actually listening in the guest. Environment equivalents are `MHI2_WIFI` and
`MHI2_WIFI_SSID`. SSIDs must contain 1–32 UTF-8 bytes.

For direct QEMU invocation, use either:

```sh
-nic user,model=mv8787-sdio,id=wifi
```

or the explicitly wired netdev form:

```sh
-nic none -netdev user,id=wifi -global mv8787-sdio.netdev=wifi
```

Device properties `ssid`, `bssid`, `channel` (1–11) and `mac` are configurable
with `-global mv8787-sdio.PROPERTY=VALUE`. With a named backend, QMP
`set_link` with `{"name":"wifi","up":false}` disconnects the station and makes
scans empty; `up:true` makes the AP available again.

The portable builder requires `libslirp` (`brew install libslirp`, or
`libslirp-dev` on Debian/Ubuntu) and configures `--enable-slirp`. Existing builds
without slirp need rebuilding to use the default NAT backend.

### Guest diagnostics

If the HMI has not enabled Wi-Fi client mode yet, the disposable emulator's
diagnostic shell can exercise the original supplicant directly:

```sh
wpa_cli -i mlan0 scan
# Wait for the scan to complete, then:
wpa_cli -i mlan0 scan_results
wpa_cli -i mlan0 add_network
# Use the returned network ID below (0 in a fresh session).
wpa_cli -i mlan0 set_network 0 ssid '"QEMU Wi-Fi"'
wpa_cli -i mlan0 set_network 0 key_mgmt NONE
wpa_cli -i mlan0 enable_network 0
wpa_cli -i mlan0 status
```

Once `wpa_state=COMPLETED`, the guest connection manager normally starts DHCP.
For manual diagnostics when it has not started a client:

```sh
dhcp.client -h qemu -H -A 1 -n -k -i mlan0 &
# Wait for the address to appear before pinging.
ifconfig mlan0
ping -c 3 10.0.2.2
```

Default user networking supplies `10.0.2.15`, gateway `10.0.2.2` and DNS
`10.0.2.3`. No `save_config` or guest filesystem changes are needed for this
test. The model currently supplies one open AP, without WPA authentication,
roaming, monitor mode or external clients joining the HU's own hotspot.

## Bluetooth

Without a backend the built-in controller initializes, retains local settings
and completes inquiry with no devices. It does not invent a paired phone.

For connections, attach a controller carrying the standard H4 stream over a
QEMU chardev. HCI commands, events, ACL and SCO packets pass between that
controller and the guest's original Bluetooth host stack. Pairing, SDP, HFP,
A2DP and other profiles remain the guest stack's responsibility; the backend
must implement the controller features those profiles require. The Marvell
firmware-health query is handled locally because it is specific to the
emulated chip.

### Dedicated USB adapter via Bumble

Use a BR/EDR-capable Bluetooth USB adapter accessible exclusively to libusb.
This does not expose the Mac's built-in Bluetooth controller or its existing
macOS pairings. Whether an adapter can be claimed depends on its host driver;
on Linux an adapter in use by BlueZ/kernel Bluetooth must first be released.

Install the optional bridge in its own environment (the GUI has separate
Python dependency pins). Use Python 3.11 or newer; on macOS the interpreter
from the README is `"$(brew --prefix python)/bin/python3"`, rather than Apple's
older system Python:

```sh
python3 -m venv .venv-bluetooth
.venv-bluetooth/bin/python -m pip install -r tools/mhi2/requirements-bluetooth.txt
.venv-bluetooth/bin/bumble-usb-probe
```

Select the adapter's `usb:...` transport printed by the probe. Start the bridge
first, then QEMU in another terminal:

```sh
# Replace usb:0 with the transport for your dedicated adapter.
.venv-bluetooth/bin/bumble-hci-bridge tcp-server:127.0.0.1:9000 usb:0
```

```sh
bash /path/to/porsche-build/run.sh \
  --bluetooth 'socket,host=127.0.0.1,port=9000'
```

This uses Bumble's existing bridge; no custom phone profile is installed in
QEMU. For SCO audio, Bumble 0.0.235 accepts `usb:0+sco=0` to select an
isochronous alternate setting automatically (or `sco=N` for a specific one).
See [Bumble USB transport](https://google.github.io/bumble/transports/usb.html)
and its [SCO implementation](https://github.com/google/bumble/blob/v0.0.235/bumble/transport/usb.py).
The guest/controller negotiation and host adapter must support it. Physical
phone pairing, streaming and call audio have **not** been verified here.

### Other controllers

Any compatible H4 endpoint, including a BlueZ `btproxy` controller socket or a
software controller, can use the same chardev. A Unix socket example is:

```sh
bash /path/to/porsche-build/run.sh --bluetooth 'socket,path=/tmp/bt-controller'
```

Environment equivalent: `MHI2_BLUETOOTH`. Direct QEMU equivalent:

```sh
-chardev socket,id=bt,host=127.0.0.1,port=9000 \
-global mv8787-sdio.bluetooth-chardev=bt
```

The backend should be running before boot. A disconnected backend generates
an HCI Hardware Error rather than silently falling back to the empty radio.
QEMU chardev reconnection options can reconnect transport, but recovery of an
active pairing/session depends on the guest stack; rebooting the guest is the
reliable way to start with a different controller. ISO packets for Bluetooth
5.2 LE Audio are outside this older controller's interface.

## Validation

Run the protocol regression without any firmware download or boot assets:

```sh
python3 tools/mhi2/check_wireless.py --qemu /path/to/qemu-system-arm
```

It uses blank NOR/IRAM images with QEMU's qtest accelerator and exercises actual
SDHCI commands: scans, association, command queues, rotating data ports,
batched transmit, bidirectional Ethernet, libslirp ARP, fragmented/coalesced H4,
1021-byte ACL, SCO, oversized input and network/controller disconnects. This
runs in the macOS/Linux CI workflow.

With the optional Bluetooth environment, also test against an independent
[Bumble software controller](https://github.com/google/bumble):

```sh
.venv-bluetooth/bin/python tools/mhi2/check_wireless.py \
  --qemu /path/to/qemu-system-arm --bumble
```

Local Apple Silicon validation with Porsche K5126 and its original QNX drivers
found the AP, reached `wpa_state=COMPLETED`, obtained `10.0.2.15` by DHCP, and
exchanged 30 ICMP packets with 1400-byte payloads with the NAT gateway with
zero loss. Wi-Fi selection was exercised through native `wpa_cli` diagnostics;
brand-specific HMI menu selection has not been validated on every firmware.
Bluetooth transport and independent-controller tests passed; these are not
end-to-end phone/profile tests. The radio remains non-migratable, as before.
