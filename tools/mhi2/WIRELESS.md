# Wi-Fi, Bluetooth and cellular

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

### Porsche K5126 band restrictions

Static inspection of the original `MHI2_ER_POG11_K5126` binaries confirms
different constraints for the driver and the head unit's hotspot:

- `connectionmanager` selects an upper hotspot channel of 11 or 13 in the
  region-dependent routine at `0x1555c0` (stored at WLAN object offset
  `0x478`). Its `WLAN::setDefaultChannel` at `0x16cf4c` clamps the configured
  channel to that limit and defaults to 11. Its generated uAP configuration
  also includes `ChanList=1,6,11`. This stock hotspot path is 2.4 GHz.
- The original `devnp-mrvl_wlan-sdiorm.so` contains 802.11a/5 GHz support.
  `wlan_ret_get_hw_spec` at `0x1addc` extracts the radio-reported band mask;
  its BAND_A branch tests mask `0x04` and selects channel 36 (`0x24`). The driver
  includes 5 GHz region/channel handling and DFS/radar handling. This is
  conditional support, not proof that a physical board advertises/enables it.
- The bundled `sd8787_uapsta.bin` identifies itself as
  `w8787-Ax, RF878X, FP44, 14.44.35.p233, BT_SDIO`. Its presence alone does
  not establish the physical unit's band capabilities.

Joining a 5 GHz access point as a client has not been demonstrated on the
original hardware. QEMU currently advertises only the 2.4 GHz AP described
above, so a channel listing from the emulator cannot establish the physical
PCM's capabilities. These findings apply to K5126, not the unavailable
P5250 firmware from the user's vehicle.

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

## Cellular

```sh
bash /path/to/porsche-build/run.sh --cellular 4g
bash /path/to/porsche-build/run.sh --cellular 3g
```

`MHI2_CELLULAR=4g` is the environment equivalent. The default is `off`.
Both modes emulate a Cinterion ALS6 composite USB device (`1e2d:0060`), with
four serial interfaces and CDC ECM control/data interfaces. `3g` changes the
reported radio access technology to UMTS; it does not substitute a different
USB modem. The SIM identity and network are synthetic, and voice calls, SMS,
remote SIM access and commercial SIM authentication are not implemented.

The launcher connects it to `usb-bus.2,port=1`. The original QNX USB launcher
starts `devc-serusb` and `devnp-ecmplus`, creates `/dev/NAD/AT_port`, and exposes
`ecm0`. The firmware enables the Ethernet link with its normal AT data-session
commands. The data network is `10.0.3.0/24`, separate from Wi-Fi's
`10.0.2.0/24`; libslirp supplies DHCP, gateway and DNS. It uses the host's
network rather than transmitting cellular RF.

For another normal QEMU network backend, direct invocation can use:

```sh
-netdev user,id=cellular,net=10.0.3.0/24 \
-device usb-mhi2-modem,id=modem,bus=usb-bus.2,port=1,netdev=cellular,lte=on
```

Replace the `-netdev` backend/options as needed. Keep the explicit USB bus and
port: the MHI2 guest enables Tegra USB controllers 0 and 2, and the modem is a
high-speed device. QMP `set_link` on `modem` drops the data session; reconnecting
the backend requires a new AT session activation. Migration is not supported.

Porsche K5126's emulated RCC profile supplies normal production-mode status,
telephone/WLAN equipment activation and the native diagnostic data-services
permission. The latter follows the existing `00060700` online-services feature
switch. These are companion responses; no firmware executables are patched.
The settings are scoped to the inspected K5126 train rather than assumed to
have the same layout on every Audi/VW release.

Once the firmware has booted, `python3 tools/mhi2/network.py cellular` requests
the `qemu` APN and automatic connection through the original
`DSIDataConfiguration` provider. It accepts native data-access requests for
that emulator session. `python3 tools/mhi2/network.py wifi --ssid 'QEMU Wi-Fi'`
uses the original `DSIWLAN` provider to enable station mode, scan and associate.
These helper requests currently support Porsche K5126 only. They do not replace
either provider, write firmware binaries, disable packet filtering or run a
parallel DHCP client. Native configuration changes have the same lifetime as
the running VM's snapshot. Firmware network status and
`/tmp/mhi2-rcc-peer.log` show the outcome; sending a request before native
services initialize can fail, in which case repeat it after boot.
Porsche K5126 rejects WLAN tethering with a SIM inserted or a SAP phone
connected; use `--cellular off` when selecting the WLAN uplink. The helper
preserves this native policy.

On Apple Silicon with Porsche K5126, the original NAD service registered on
the simulated LTE network, and the original connection manager activated ECM
and started DHCP. The guest obtained `10.0.3.15`, gateway `10.0.3.2` and DNS
`10.0.3.3`. Guest DNS resolved `github.com` and `porsche.com`; gateway ICMP with
1400-byte payloads and Internet ICMP to GitHub completed without loss in the
validation run. 3G is covered by protocol tests; a separate full firmware boot
in 3G mode has not been validated.

Internet access and manufacturer services are separate checks. Account login,
map content, retired service endpoints and vehicle provisioning are not
emulated. Mobile-app local discovery also requires a suitable shared/bridged
network: default user-mode NAT does not put a physical phone on the head unit's
LAN, and an outbound Internet connection alone does not establish app pairing.

### Vehicle identity

The RCC supplies stable synthetic defaults: VIN `ZZZEMU00XP0000001` and
FAZIT ID `EMU-00009.10.2600000001`. The VIN is 17 uppercase characters,
excludes I/O/Q, and includes its calculated check digit. These are emulator
fixtures, not assigned manufacturer identities or backend credentials.

K5126's original `VehicleRegistrationRequest` contains `vin`, `bg`, and `snr`.
`OnlineRegistrationServiceImpl.getVehicle()` sets `bg` to `5F` and obtains
`snr` from `FazitIdComponent.getFazitIDMIB2()`, which reads raw RCC persistence
key `0:3221291024` as UTF-8. The emulator now supplies that attribute and
delivers the configured VIN through the DSI vehicle service.
The firmware rejected the previous `SIMULATED-VEHICLE` placeholder and could
fall back to a generated test VIN.

To use your own vehicle/unit identifiers, edit the `identity` object in the
local brand configuration (`~/.config/mhi2/porsche.json`, or `MHI2_CONFIG`).
Keep the existing top-level `version`, `features`, and `usb` settings when
editing an existing file. For a new file, this is a complete minimal example:

```json
{
  "version": 1,
  "identity": {
    "vin": "ZZZEMU00XP0000001",
    "fazit_id": "EMU-00009.10.2600000001",
    "bg": "5F"
  }
}
```

Top-level `"version": 1` is required; it identifies the emulator configuration
schema, not the firmware version. Missing or unsupported versions are rejected.
Omitted `features` and `usb` sections use the emulator defaults.

Restart the emulator after editing. Older version-1 configurations that lack
an `identity` section inherit the test defaults automatically.
`emulator_config.py --vin ... --fazit-id ...` can also update the local
configuration; use `--file` to select an explicit file.
VIN validation checks uppercase format and excludes I/O/Q; it does not
require a check digit for imported European VINs. FAZIT validation checks
bounded printable identifier syntax, not manufacturer allocation. `bg` must
remain `5F`; the original firmware owns that registration field.

Real account pairing still requires the manufacturer's pairing flow and
service entitlement. These identifiers alone do not demonstrate backend
authorization; unit-specific provisioning requirements have not been verified.
Keep actual identifiers and credentials outside the repository.

#### My Porsche add-vehicle validation

Static inspection of the user-supplied My Porsche Android 21.26.39-row
(190220) package distinguishes local input checks from backend acceptance:

- The add-VIN view model (`vehicleadd.ui.add.vin.g.v0`) accepts 17-character
  input for lookup. It also supports shorter identifiers for older vehicles.
- The vehicle-info API (`vehicleinfo.api.m.b`) performs a GET to
  `/proof-of-ownership/v1/{locale-part-1}/{locale-part-2}/{vin}/details`.
  There is no FAZIT parameter or HU connection in this request.
- `vehicleinfo.service.h.b` maps HTTP 400 to invalid-format and HTTP 404 to
  vehicle-not-found; the add-VIN UI renders these as separate errors.
- A successful response provides the vehicle description and
  `addRequestAllowed`; the UI reports an already-added vehicle when the latter
  is false. Further ownership verification is a separate flow.

A generated VIN can pass length, checksum and model-decoder checks without
existing in Porsche's records. Changing the emulator's FAZIT ID cannot fix
this app-side lookup. Porsche documents the real-vehicle and ownership
requirements in its [vehicle management instructions](https://ask.porsche.com/us/en-US/manage-vehicles/).
The user's exact rejection response has not been captured, so this static
analysis does not identify which HTTP status their session received.
No production API requests, account registration, or app modifications were
performed for this inspection. Decompiled proprietary code is not included
in the repository.

## Validation

Run the protocol regression without any firmware download or boot assets:

```sh
python3 tools/mhi2/check_wireless.py --qemu /path/to/qemu-system-arm
python3 tools/mhi2/check_modem.py --qemu /path/to/qemu-system-arm --mode 3g
python3 tools/mhi2/check_modem.py --qemu /path/to/qemu-system-arm --mode 4g
```

It uses blank NOR/IRAM images with QEMU's qtest accelerator and exercises actual
SDHCI commands: scans, association, command queues, rotating data ports,
batched transmit, bidirectional Ethernet, libslirp ARP, fragmented/coalesced H4,
1021-byte ACL, SCO, oversized input and network/controller disconnects. This
runs in the macOS/Linux CI workflow.

The modem test exercises EHCI DMA and real USB transfers, descriptor
enumeration, SIM/registration, malformed and oversized AT commands, PDP
activation, libslirp DHCP, ARP and bidirectional UDP traffic. It covers exact
512/1024-byte Ethernet transfer boundaries, full-size packets, backend link
loss/recovery and radio-off teardown. It runs without proprietary boot assets.

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
