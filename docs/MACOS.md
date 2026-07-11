# macOS support

Sony Head Tracker supports macOS 14 or later through a native IOHID backend, a
command-line bridge, and a SwiftUI application. The implementation reuses the
same platform-independent descriptor parser, orientation filter, recentering,
axis mapping, and UDP serializers as the Windows application.

This is an unofficial project and is not affiliated with or endorsed by Sony.

## Device support

Device discovery is based on the Android Head Tracker HID protocol, not a list of
Sony product IDs. A usable device must expose:

- top-level usage page `0x20` and usage `0xE1`;
- a compatible `#AndroidHeadTracker#` descriptor marker;
- descriptor-defined input and feature elements that can be read through
  IOHID.

The macOS implementation does not hardcode model names, VID/PID values, report
IDs, report lengths, or feature report bytes. Therefore WH/WF-1000XM5,
WH/WF-1000XM6, ULT WEAR, and future models exposing the same protocol all pass
through the same discovery and parsing code.

Platform-specific validation is deliberately reported separately from expected
protocol compatibility:

| Model | macOS status |
| --- | --- |
| Sony ULT WEAR (WH-ULT900N) | Hardware validated |
| Sony WH-1000XM5 | Protocol-compatible; macOS hardware confirmation requested |
| Sony WF-1000XM5 | Protocol-compatible; macOS hardware confirmation requested |
| Sony WH-1000XM6 | Protocol-compatible; macOS hardware confirmation requested |
| Sony WF-1000XM6 | Protocol-compatible; macOS hardware confirmation requested |

Other models can be checked without changing the source code by running
`probe`. A verified collection is the compatibility requirement.

## Requirements

- macOS 14 or later;
- full Xcode selected with `xcode-select`;
- CMake 3.25 or later;
- XcodeGen only when regenerating the committed SwiftUI Xcode project;
- a paired compatible headset with current firmware.

Verify the toolchain:

```bash
xcode-select -p
xcrun --show-sdk-path
clang++ --version
cmake --version
sw_vers
uname -m
```

If full Xcode is installed but not selected:

```bash
sudo xcode-select --switch /Applications/Xcode.app/Contents/Developer
```

This changes the active developer toolchain used by terminal builds. It does not
remove, reset, or otherwise prevent normal use of Xcode.

## Permissions

The app and CLI need permission to listen to IOHID input reports. On first use,
allow the executable under:

**System Settings > Privacy & Security > Input Monitoring**

Restart the app or CLI after changing the permission. The macOS app also includes
a Bluetooth usage description because its recovery path uses public IOBluetooth
APIs to reconnect the same previously verified paired headset and refresh its SDP
services after a power cycle.

The first macOS version intentionally does not enable App Sandbox while the
IOHID hardware path is being validated. UDP output is restricted to loopback.

## Build and test the CLI

```bash
cmake -S . -B build/macos -DCMAKE_BUILD_TYPE=Release
cmake --build build/macos --parallel
ctest --test-dir build/macos --output-on-failure
```

The CLI executable is:

```text
build/macos/sony-head-tracker-macos
```

Supported commands:

```text
sony-head-tracker-macos probe
sony-head-tracker-macos dump [--seconds N]
sony-head-tracker-macos bridge [--port 4242] [--seconds N]
                                [--axis-map YXZ]
                                [--invert XZ]
                                [--smoothing 0.18]
sony-head-tracker-macos help | version
```

`probe` lists candidate HID collections and verifies the Android Head Tracker
descriptor. `dump` prints decoded samples for hardware diagnosis. `bridge`
streams full-rate data to OpenTrack on UDP `127.0.0.1:4242` and version 2 JSON on
UDP `127.0.0.1:4243` by default. The binary OpenTrack packet remains exactly six
little-endian doubles `(x, y, z, yaw, pitch, roll)`, with translation set to
zero. Send `SIGUSR1` to recenter the running CLI process.

## Build and run the SwiftUI app

The Xcode project is committed and can be built directly. After changing
`macos/project.yml`, regenerate it from the committed XcodeGen specification:

```bash
# Optional after project.yml changes: (cd macos && xcodegen generate)
./script/build_and_run.sh
```

The build script uses a stable DerivedData directory under `build/DerivedData`,
builds the Debug app, and launches a new instance. It accepts `--debug`, `--logs`,
`--telemetry`, and `--verify` for development diagnostics.

The app displays connection state, the selected device, yaw/pitch/roll,
gyroscope values, sample rate, a bounded orientation graph, loopback output
ports, device details, persistent filter/port settings, and actionable
permission or connection errors. Its shareable diagnostic snapshot reports the
app/OS/architecture, IOHID access, candidate and element counts, sanitized
descriptor and feature read-back, packet/sample/UDP health, reconnect attempts,
settings, and a classified last error without including a Bluetooth address or
personalized device name. Hardware callbacks are copied to value types and delivered to the
MainActor; UDP continues at the headset's full rate while visible UI updates are
limited to about 30 Hz.

## ULT WEAR and silent A2DP activation

On the tested ULT WEAR, macOS exposes and configures the Android Head Tracker HID
collection but does not begin sending changing input reports until the Bluetooth
A2DP audio path is active. Manual music playback proves the sensor works, but is
not suitable as an application requirement.

After configuring a verified tracker, the macOS implementation therefore starts
a zero-filled PCM stream on the Core Audio output that belongs to that exact
verified Bluetooth headset. The stream:

- is silent and does not change the system output device or volume;
- is selected by the verified device address, with an unambiguous exact product
  name only as a fallback;
- remains active for the tracking session because stopping it can stop the HID
  samples;
- stops when tracking stops, the device disconnects, recovery begins, or the
  process exits;
- may cause a modest increase in headset battery use while tracking.

This keepalive is not keyed to the text `ULT WEAR`, a Sony VID/PID, or a report
layout. Other verified Android Head Tracker devices use the same generic path;
their model-specific macOS behavior still needs hardware confirmation. ULT WEAR
is called out because it is the model on which the requirement was observed and
hardware-validated.

## Power-cycle recovery

After a headset is verified, its Bluetooth address is stored with mode `0600` in:

```text
~/Library/Application Support/SonyHeadTracker/last-tracker.plist
```

The address is used only to recover that same paired device after it is powered
off and on. If its Android Head Tracker collection is absent, recovery first asks
the paired device to connect and refresh SDP. A later retry may perform one
baseband close/open cycle followed by another SDP refresh. The code does not
scan for new devices, pair or forget devices, alter Bluetooth identities, open a
private HID transport, or act on a different device. Addresses are not printed
or included in UDP output.

Some ULT WEAR sessions expose the collection before samples resume. The CLI can
perform one clean child-process recovery rather than looping indefinitely. The
SwiftUI engine treats a configured connection with no valid sample for five
seconds as a stalled stream: it closes the current silent-audio and IOHID
session, refreshes SDP, and retries with bounded backoff. A second consecutive
stall performs one paired-device baseband reconnect; later retries reopen IOHID
without repeatedly dropping Bluetooth. Recovery state is cleared only after a
valid sample arrives.

## Differences from the original Windows implementation

| Area | Windows | macOS |
| --- | --- | --- |
| HID access | SetupAPI, HidD/HidP, Sensor API fallback | IOHIDManager, IOHIDDevice, CFRunLoop |
| UDP | Winsock | POSIX/BSD sockets |
| Bluetooth recovery | SetupAPI/device rebind and generic HID repair | Address-scoped public IOBluetooth connection/SDP refresh |
| UI | Win32/GDI | SwiftUI using a narrow C ABI over the C++ engine |
| Configuration path | `%LOCALAPPDATA%` | `~/Library/Application Support/SonyHeadTracker/` |
| Permissions | Windows device/UAC flow | Input Monitoring plus Bluetooth usage description |
| ULT WEAR activation | Normal Windows audio/device behavior | Session-lifetime silent Core Audio A2DP keepalive |

The Windows-only `repair`, `bluetooth-rebind`, `bluetooth-generic-hid`, Sensor
API fallback, Win32 GUI, and executable resources are not presented as macOS
features. Both platforms preserve the same filtering behavior and loopback UDP
wire formats.

## Troubleshooting

### No verified tracker is found

1. Confirm the headset is connected in System Settings > Bluetooth.
2. Run `sony-head-tracker-macos probe`.
3. Confirm Input Monitoring permission and restart the executable.
4. Update the headset firmware using Sony Sound Connect.
5. Temporarily disconnect phones or other multipoint hosts that may own the
   sensor transport.
6. Power-cycle the headset and allow automatic paired-device recovery to run.

Removing and pairing the device again can refresh macOS's HID services, but it
should be a last diagnostic step rather than a normal startup requirement.

### The tracker is configured but no YPR appears

Allow several seconds for the silent A2DP keepalive to activate the audio path.
The log should report the keepalive and then the first valid sample. If no sample
arrives, confirm that macOS still lists the headset as the active connected
Bluetooth device and rerun `probe`.

### OpenTrack or JSON receives nothing

Only one process can bind a given local UDP receiver port. Start the bridge first,
then configure OpenTrack for UDP input port `4242`. A JSON receiver must listen
on `127.0.0.1:4243`. If `--port N` is used, JSON always uses `N + 1`.

## Hardware validation completed

The ULT WEAR validation completed the following checks on Apple Silicon:

- descriptor verification at usage `0x20:0xE1`;
- changing rotation and gyroscope samples at approximately 50 to 60 packets per
  second;
- recentering and reconnect after a headset power cycle;
- 48-byte OpenTrack packets with zero translation;
- version 2 JSON telemetry;
- 1,134 UDP samples in a 20-second post-review run with the silent A2DP
  keepalive active, including recovery from an initially absent HID collection.

Reports for XM5, XM6, and other descriptor-compatible models are welcome. Include
the model, firmware, macOS version, Mac architecture, and redacted `probe` output.
