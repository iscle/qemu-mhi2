# Missing version and Car menu data

Investigation of the September 30 user session, using the saved UART log and
decompiled K3342 Java classes. The session had already stopped when inspected;
no running VM or media was changed during this investigation.

## Software update / versions

The log reports all four services unavailable: DSISwdlDeviceInfo,
DSISwdlSelection, DSISwdlLogging and DSISwdlProgress. Calls to
DSISwdlSelection.getMedia and DSISwdlLogging.getHistory fail during menu use.
AslTargetCarWorkshopDownload obtains its device inventory, modules and version
rows through DSISwdlDeviceInfo.getDevices/getModules/getVersions/getTargetVersions.
Missing providers therefore leave these lists without their data. The native
swdlclient process starts, but its presence does not establish these HMI services.

Identification fields have another dependency: processDiagIdentification in
AslTargetSystemFeaturesAndCoding reads diagnostic fields 839–842 and publishes
part/version information into the UI. The log contains repeated RCC persistence
backend timeouts, including MMXDiagService requests for namespace 52166966,
key 201. Its HMI status records contain empty part.number and software.mu.version,
while hmisoftware.version is H28.28.210_HIGH2_EU. Locally packaged HMI version
strings remain available through VersionNumberUpdater; this explains why only
some fields are missing.

## Car / Selection

The log reports unavailable DSICarKombi, DSICarVehicleStates, DSICarComfort and
other vehicle DSIs. The UI model initializes MappingList entries as
FunctionState(0,0); FunctionState defines existence as state != 0. Vehicle
view-option updates supply existence, availability and reason values.
For example, BordComputerDevice publishes its main-menu existence/availability
lists as IDs 10803–10805 from these states. Without updates, functions remain
absent and are hidden. Diagnostic coding/adaptation failures are an additional
gap; enabling a menu alone would not provide its vehicle data.

## Original cause (before the service models)

The RCC peer at the time registered DSIKeyPanel only. PCIe Ethernet, ESO transport
and working input do not imply implementation of the other RCC services or
vehicle bus participants. Shortened experimental startup timeouts (1/5 seconds)
let the HMI display despite missing dependencies; restoring timeouts would not
supply these services.

These findings identify missing backend inputs rather than establish a graphics
failure. A complete device/version inventory requires the SWDL service plus
diagnostic/persistent identification. Car menus require a coherent vehicle
configuration and the corresponding DSI view-option/status responses. Firmware
package version metadata can supply some software facts; it cannot safely stand
in for an arbitrary vehicle's installed equipment or unit-specific identity.
Individual ownership of all missing SWDL DSIs was not verified in this pass.

Evidence and recovered classes:
`/home/iscle/Downloads/mhi2-analysis/qemu/protocol-analysis/menu-data/`.
The saved `user-session-console.log` includes SWDL failures around lines
8433–8440, 8734 and 9107; RCC persistence timeouts around 12666–12667; and
empty identification fields at 13250. The original binaries were not patched.

## Implemented corrections

`rcc_services.py` now serves the recovered SWDL, vehicle, clock and cluster-sync
DSI interfaces through the production ESO transport. `rcc_persistence.py`
implements the RCC Attributes backend used by the original native MMX
persistence service. `dsi_schema.json` preserves the firmware's wire method IDs,
attribute IDs and struct layouts; the recovery scripts regenerate this from
decompiled K3342 classes.

`vehicle_profile.json` supplies diagnostic defaults and an explicitly simulated
identity. The profile enables driving data, vehicle status and service data.
Version identification comes from K3342 metadata, with `SIM-QEMU` identifying
the simulated unit; it does not invent a production serial number. The inventory
selects MMX variant 70. Installed external tuner/phone/amplifier hardware is not
inferred from the update package. Updates have no media offered, and unsupported
operations return an error rather than claim installation succeeded.

Real firmware screenshots are saved in `qemu/evidence-ui/`:
`car-selection-fixed.png` and `software-versions-fixed.png`. They show a populated
Selection menu and the K3342 train/MU1427 fields. `check_services.py` checks the
wire encodings, persistence, subscriptions, inventory and TCP send window.

These are service models for a parked simulated vehicle. Audio, navigation map
production, telephone hardware and full vehicle dynamics remain incomplete.
Persistence writes currently last for the session, matching the default
snapshot-media workflow. MOST video support and its limits are documented in
[MOST.md](MOST.md).
