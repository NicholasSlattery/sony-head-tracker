# Sony Head Tracker

Use the motion sensors already inside compatible Sony headphones and earbuds as
a real-time head tracker on Windows or macOS.

[![Build](https://github.com/NicholasSlattery/sony-head-tracker/actions/workflows/build.yml/badge.svg)](https://github.com/NicholasSlattery/sony-head-tracker/actions/workflows/build.yml)
[![Latest release](https://img.shields.io/github/v/release/NicholasSlattery/sony-head-tracker)](https://github.com/NicholasSlattery/sony-head-tracker/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform: Windows 11](https://img.shields.io/badge/platform-Windows%2011-0078D6.svg)](#build)
[![Platform: macOS 14+](https://img.shields.io/badge/platform-macOS%2014%2B-000000.svg)](docs/MACOS.md)
[![Language: C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg)](#build)
[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20me%20a%20coffee-support-yellow?style=for-the-badge)](https://buymeacoffee.com/nicholasslattery)
> **Unofficial** open-source bridge. Not affiliated with or endorsed by
> Sony.

https://github.com/user-attachments/assets/851e4ff7-3134-45e9-90b4-a0fa42258730

Sony Head Tracker connects to the Android Head Tracker sensor exposed by
supported Sony Bluetooth devices, reads live orientation and gyroscope data, and
sends yaw, pitch, and roll to [OpenTrack](https://github.com/opentrack/opentrack)
or your own applications.

No webcam, infrared tracker, additional hardware, firmware modification, or
custom kernel driver is required.

Originally developed and tested for the Sony WH-1000XM5, the project now supports
any compatible Sony headset that exposes the standard
[Android Head Tracker HID protocol](https://source.android.com/docs/core/interaction/sensors/head-tracker-hid-protocol).

If you arrived searching for **Sony WH-1000XM5 head tracking on Windows**, how to
**use WH-1000XM5 with OpenTrack**, or an **XM5 head tracker for Assetto Corsa** or
other sims, you are in the right place: the WH-1000XM5 is the reference device,
and the same bridge now works across the wider Sony range.

## What you can do with it

- Use compatible Sony headphones for head tracking in racing and flight
  simulators.
- Send live yaw, pitch, and roll to OpenTrack.
- Read orientation, quaternion, and gyroscope data through a local JSON stream.
- Inspect sensor activity through the included native diagnostics interfaces.
- Test additional Sony models with the read-only compatibility probe.

## Featured in

- [PC Gamer — This free software aims to turn Sony headphones into a head tracker for '200+ PC games'](https://www.pcgamer.com/hardware/gaming-headsets/this-free-software-aims-to-turn-sony-headphones-into-a-head-tracker-for-200-pc-games/)
- [Tom's Hardware — You can now use your Sony headphones as a free real-time head tracker for race and flight simulators on PC](https://www.tomshardware.com/video-games/pc-gaming/you-can-now-use-your-sony-headphones-as-a-real-time-head-tracker-for-race-and-flight-simulators-on-pc-several-hundred-games-already-supported-enthusiast-creates-open-source-app-that-translates-live-sensor-data-into-in-game-camera-controls)
- [TechLinked — Sony Headphones Become Head Trackers](https://youtu.be/ITmMIqiJwOg?si=M28zLj3hcO0RshHZ&t=405)
- [GSMArena — Developer turns Sony's headphones into real-time head tracker for PC simulators](https://www.gsmarena.com/developer_turns_sonys_headphones_into_realtime_head_tracker_for_pc_simulators-news-73610.php)
- [Hackaday — Flight Sim Tracking From Spatial Audio](https://hackaday.com/2026/07/06/flight-sim-tracking-from-spatial-audio/)
- [Techspot — A free app lets Sony headphones do head tracking for racing and flight sims](https://www.techspot.com/news/113019-free-app-sony-headphones-do-head-tracking-racing.html)
- [SoundGuys — This free app turns Sony WH-1000XM5 into head trackers for PC gaming](https://www.soundguys.com/sony-headphones-windows-head-tracking-159435/)


## Quick start

### Windows 11

1. Download `sony-head-tracker.exe` from the
   [latest release](https://github.com/NicholasSlattery/sony-head-tracker/releases/latest)
   (or [build it yourself](#build), it is one `cl` command).
2. [Pair your compatible Sony headphones or earbuds](#pair-the-headphones)
   through Windows 11.
3. Open the application. It automatically discovers compatible head-tracking
   sensors, displays live orientation data, and streams tracking output while it
   is open.
4. **Starting the app for the first time on a fresh boot? Press Repair Tracker
   first.** Windows very often pairs a Sony headset but does not create the
   head-tracker sensor node until something nudges it, so on a cold boot the app
   frequently shows nothing at first. Press the **Repair Tracker** button and
   approve the single administrator prompt. This is the normal first step, not a
   sign that anything is broken, and it is exactly what the button is for. After
   the app reopens, your headset should appear.
5. For games: in OpenTrack, select **UDP over network** as the input and use
   port `4242`, then press **Start**. See [OpenTrack](#opentrack).
6. For your own code: read one JSON object per sample from port `4243`. See
   [`docs/PROTOCOL.md`](docs/PROTOCOL.md).
7. Press **Recenter** (or Ctrl+Alt+C) while facing forward.

### macOS 14 or later

The macOS port includes a native SwiftUI application and a command-line bridge.
It discovers devices by the Android Head Tracker HID descriptor instead of by
model name, VID/PID, report ID, or report length.

Choose **one** of the following ways to run Sony Head Tracker. You do not need
to build or run both. Both options require full Xcode selected with
`xcode-select`; the command-line option additionally requires CMake 3.25 or
later.

#### Option 1 — Native macOS application (recommended)

The application does not require a separate CMake build. From the repository
root, run:

```bash
./script/build_and_run.sh
```

The script builds the committed Xcode project, applies a stable local development
signature when one is available, and opens `SonyHeadTracker.app`.

#### Option 2 — Command-line bridge

Use this option when you want a Terminal-only bridge without the SwiftUI
application:

```bash
cmake -S . -B build/macos -DCMAKE_BUILD_TYPE=Release
cmake --build build/macos --target sony-head-tracker-macos --parallel
./build/macos/sony-head-tracker-macos bridge
```

Do not run the application and CLI bridge at the same time; they would compete
for the same tracker and send duplicate data to the same loopback UDP ports.

XcodeGen is **not** required for a normal build. Regenerate the committed Xcode
project only after changing `macos/project.yml`:

```bash
cd macos
xcodegen generate
```

On first use, allow Sony Head Tracker under **System Settings > Privacy &
Security > Input Monitoring**, then quit and reopen the application or CLI.
See the complete setup, device-support status, ULT WEAR audio activation note,
and troubleshooting steps in
[`docs/MACOS.md`](docs/MACOS.md).

> **Hardware status:** changing orientation reports have been received from a
> physical WH-1000XM5 on Windows and a physical ULT WEAR on macOS. Behaviour
> varies with Sony firmware and the platform Bluetooth stack. This project never
> spoofs another headset, changes Bluetooth identities, or modifies firmware.

## Contents

- [Quick start](#quick-start)
- [macOS guide](docs/MACOS.md)
- [Compatibility](#compatibility)
- [Where the data goes (ports)](#where-the-data-goes-ports)
- [Gyroscope and accelerometer](#gyroscope-and-accelerometer)
- [Default orientation](#default-orientation-yxz-x-and-z-inverted)
- [Build](#build)
- [Pair the headphones](#pair-the-headphones)
- [Usage](#usage)
- [The GUI](#the-gui)
- [OpenTrack](#opentrack)
- [When the sensor won't show up](#when-the-sensor-wont-show-up)
- [Protocol and security notes](#protocol-and-security-notes)
- [Contributing](#contributing)
- [License](#license)

## Compatibility

Compatibility is determined by the protocol exposed by the headset, not simply by
the Sony model name. A device is compatible when it exposes an Android Head
Tracker HID sensor with the expected `#AndroidHeadTracker#` descriptor (usage
`0x0020:0x00E1`).

The underlying engine is protocol-based, although the project is currently
focused on testing and supporting Sony headphones. In practice that means
compatible Sony headphones, with experimental support for other devices using
the same protocol.

**You can check any device in seconds, with zero risk.** Pair the headphones,
then run:

```text
Windows: sony-head-tracker.exe probe
macOS:   ./build/macos/sony-head-tracker-macos probe
```

`probe` is read-only. It never writes to the device, changes drivers, or touches
firmware. If it finds a verified Android head tracker it prints the descriptor
and exits `0`. If not, it lists what it saw and exits `2`.

Reports for both working and non-working devices are welcome. Please
[open an issue](https://github.com/NicholasSlattery/sony-head-tracker/issues/new/choose)
with the `probe` output and your platform, OS, and firmware versions, and the table
below will be updated.

### Confirmed

| Device | Reported by |
| ------ | ----------- |
| Sony WH-1000XM5 | maintainer, tested |
| Sony WF-1000XM5 | community confirmed |
| Sony WH-1000XM6 | community confirmed |
| Sony WF-1000XM6 | community confirmed |
| Sony ULT WEAR (WH-ULT900N) | community confirmed |

The table above records project-wide compatibility, primarily from the existing
Windows implementation. The macOS backend uses the same descriptor-defined
Android Head Tracker protocol and is not restricted to ULT WEAR. **ULT WEAR is
the current macOS hardware-validated model; XM5/XM6 and other entries still need
macOS community confirmation.** See [`docs/MACOS.md`](docs/MACOS.md) for the
platform-specific test status.

### Candidate

Sony lists these models as supporting head tracking, but they still require
community testing with this Windows implementation. Please do not assume they are
already validated.

- Sony LinkBuds Open (WF-L910)
- Sony LinkBuds Fit (WF-LS910N)
- Sony LinkBuds (WF-L900)
- Sony LinkBuds S (WF-LS900N)

A candidate model may still need the latest firmware before the sensor appears.
Install Sony's **Sound Connect** app, connect the headphones, and update before
probing.

### Not compatible

| Device | Status | Reported by |
| ------ | ------ | ----------- |
| Sony WH-1000XM4 | Does not work | community confirmed |
| Sony WH-1000XM3 | Not expected to work | not on Sony's head-tracking list |
| Apple AirPods (all models, including AirPods Pro) | Not possible on Windows | see [Why AirPods can't work](#why-airpods-cant-work-yet) |

Models that are not on Sony's head-tracking list almost certainly carry no usable
sensor. Bose QuietComfort Ultra models do head-tracked spatial audio and might one
day expose a compatible protocol, but that is unverified and out of scope for now.

### Why AirPods can't work (yet)

AirPods have the motion sensors, but Apple does not implement the Android Head
Tracker HID protocol. Head tracking (and ANC control, ear detection, and so on)
travels over **Apple's proprietary accessory protocol (AAP)** on a raw Bluetooth
Classic L2CAP channel (PSM `0x1001`). On Windows that leaves one hard blocker:

**Windows has no user-mode API for that channel.** Desktop Windows exposes
RFCOMM sockets to applications, but custom Bluetooth Classic L2CAP channels can
only be opened by a **kernel-mode profile driver**
([Microsoft's docs](https://learn.microsoft.com/en-us/windows-hardware/drivers/bluetooth/creating-a-l2cap-client-connection-to-a-remote-device)).
That is how [MagicPods](https://magicpods.app/magicaap/) does it on Windows: it
ships a custom driver. This project's hard rule is that it **never installs a
custom kernel driver**, so that path is out of scope here.

The payload itself is not unknown. The AAP head-orientation packets are
covered by [pabloaul's `rtbuddy` Wireshark dissector](https://github.com/pabloaul/apple-wireshark/tree/main/plugins/rtbuddy),
linked from the [LibrePods](https://github.com/librepods-org/librepods) README,
so the only thing standing in the way on Windows is the L2CAP access.

What this bridge does instead: it **recognises paired AirPods** and tells you
exactly this, in `probe`, in `bridge`, and in the GUI, instead of failing
silently.

## Where the data goes (ports)

In `bridge` mode (and while the GUI is open) head-tracking data is streamed over
**UDP to loopback (`127.0.0.1`)** on two adjacent ports:

| Port | Format | Consumer |
| ---- | ------ | -------- |
| **`4242`** (`--port`) | Six little-endian `double`s `(x, y, z, yaw, pitch, roll)` | OpenTrack "UDP over network" |
| **`4243`** (`--port` + 1) | UTF-8 JSON object (see below) | Your own apps and scripts |

Change the base port with `--port N`. The JSON port is always `N + 1`. The bridge
prints both destinations on startup, and the GUI shows them along the bottom edge.
Translation axes `(x, y, z)` are always zero, because this protocol reports
orientation only.

The JSON datagram (one per sample, `version: 2`):

```jsonc
{
  "version": 2,
  "device": "WH-1000XM5",                  // connected headset's Bluetooth name, or null
  "rotationVector":  [x, y, z],            // axis-angle, radians
  "quaternion":      [w, x, y, z],         // recentered orientation
  "yprDegrees":      [yaw, pitch, roll],   // degrees
  "gyroscope":       [x, y, z],            // rad/s, or null if unavailable
  "accelerometer":   [x, y, z],            // m/s^2, or null if the device doesn't report it
  "angularVelocity": [x, y, z],            // deprecated alias of "gyroscope"
  "resetCounter":    0,
  "packetsPerSecond": 25.0,
  "receiveLatencyMs": -1.0                 // -1 when the device provides no timestamp
}
```

> **Security:** UDP output is loopback-only by default and has **no
> authentication**. Do not bind or forward it to an untrusted network.

A full wire-format reference lives in [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## Gyroscope and accelerometer

Both inertial streams are exposed in full, in addition to orientation:

- **Gyroscope**: angular velocity in **rad/s**, emitted as the JSON `gyroscope`
  array (and shown live in the GUI). This is the connected headset's on-board gyro
  as carried by the Android Head Tracker protocol.
- **Accelerometer**: linear acceleration in **m/s^2**, emitted as the JSON
  `accelerometer` array **when the device reports it**.

  The Android Head Tracker HID profile that supported Sony firmware advertises
  defines only orientation and gyro fields, so on current firmware
  `accelerometer` is typically `null`. The bridge nonetheless parses the standard
  HID sensor-page acceleration usages (`0x0453` to `0x0455`, plus the vector form
  `0x0452`) from every input report, so if your firmware exposes them they are
  surfaced automatically, with no code change needed. Gyro is likewise parsed from
  both the head-tracker custom field (`0x0545`) and the standard sensor-page
  usages.

## Default orientation: YXZ, X and Z inverted

The default axis convention is **YXZ order with the X and Z axes inverted**, the
mapping that produces correct head tracking on the WH-1000XM5. It is fully
overridable:

- **CLI:** `--axis-map XYZ` (any permutation) and `--invert X` (any axes). The
  `--invert` flag is a complete override: `--invert xz` reproduces the default,
  `--invert z` inverts Z only, and `--invert none` clears all inversions.
- **GUI:** the axis-order dropdown (defaults to YXZ) and the Invert X/Y/Z
  checkboxes (Invert X and Z start checked).

The same axis convention is applied to the gyroscope and accelerometer vectors so
all streams share one coordinate frame.

## Build

### macOS

The macOS CLI and tests require full Xcode with a C++20 compiler and CMake 3.25
or later. The SwiftUI Xcode project is committed; XcodeGen is required only when
regenerating it from `macos/project.yml`.

```bash
cmake -S . -B build/macos -DCMAKE_BUILD_TYPE=Release
cmake --build build/macos --parallel
ctest --test-dir build/macos --output-on-failure

# Optional after project.yml changes: (cd macos && xcodegen generate)
./script/build_and_run.sh
```

The app targets macOS 14 or later. Detailed permissions, CLI usage, build
variants, device recovery, and ULT WEAR's silent A2DP keepalive are documented in
[`docs/MACOS.md`](docs/MACOS.md).

### Windows

Requires a C++20-capable MSVC and a current Windows 11 SDK (install **Desktop
development with C++** in the Visual Studio Installer).

```bat
build.cmd
```

`build.cmd` locates the Visual Studio C++ tools automatically. Or build by hand
from a *x64 Native Tools Command Prompt for VS*:

```bat
rc /nologo app.rc
cl /std:c++latest /EHsc /permissive- /utf-8 /O2 /W4 /DUNICODE /D_UNICODE /I include src\*.cpp app.res /Fe:sony-head-tracker.exe
```

The code is split into a hardware-independent core under
[`include/sony_head_tracker/`](include/sony_head_tracker) +
[`src/`](src) (quaternion maths, HID descriptor decoding, the orientation
filter, and protocol serialisation — all Windows-free) and a platform layer (HID
and Sensor API backends, Bluetooth repair, UDP output, and the GUI). Every backend
produces a normalized `MotionSample`, so recenter, smoothing, axis mapping, Euler
conversion, and serialisation are all unit-testable without a headset — see
[`tests/`](tests) and run them with `build-tests.cmd`. A small resource bundle
([`app.rc`](app.rc): icon, version info, and the Common Controls manifest) is
embedded at build time; the icon is generated by
[`tools/make-icon.ps1`](tools/make-icon.ps1) and committed as `app.ico`. All
required import libraries are pulled in via `#pragma comment(lib, ...)`, so the
executable stays dependency-free with no extra linker arguments. The build is
warning-clean at `/W4`. Every push is built and unit-tested in CI on
`windows-latest`, and pushing a `v*` tag publishes the exe as a GitHub Release.
See [`.github/workflows/build.yml`](.github/workflows/build.yml).

## Pair the headphones

The following pairing, command, GUI, and repair sections describe Windows. For
macOS, use [`docs/MACOS.md`](docs/MACOS.md).

1. Update the headset with Sony's app and enable its spatial-audio or
   head-tracking feature if available, then put the headset in pairing mode.
2. In Windows 11: **Settings > Bluetooth & devices > Add device > Bluetooth**,
   and pair your Sony headset.
3. Run `sony-head-tracker.exe probe`. A usable collection shows usage
   `0x0020:0x00E1` and a feature description beginning with `#AndroidHeadTracker#`.

## Usage

```text
sony-head-tracker.exe                 (no args -> diagnostics GUI)
sony-head-tracker.exe probe [--include-disabled]
sony-head-tracker.exe dump [--seconds N]
sony-head-tracker.exe repair
sony-head-tracker.exe bluetooth-probe [--all-le] [--name FILTER]
sony-head-tracker.exe bluetooth-rebind [--name FILTER]   (default: auto-detect)
sony-head-tracker.exe bluetooth-generic-hid        (run from an elevated prompt)
sony-head-tracker.exe bridge [--port 4242] [--seconds N]
                             [--axis-map YXZ] [--invert XZ] [--smoothing 0.18]
sony-head-tracker.exe help | version
```

- **`bridge`** is the main headless mode. It prints which headset it is tracking,
  reconnects automatically, and streams to the ports described in
  [Where the data goes](#where-the-data-goes-ports).
- **`probe`** prints discovered HID collections and Sensor API custom sensors,
  names the headset a verified tracker belongs to, and explains why nothing was
  found (including the AirPods case).
- **`bluetooth-rebind`** (and `repair`) auto-detect the headset by checking which
  paired device's SDP record carries the Android Head Tracker descriptor, so no
  other Bluetooth device's services are ever touched. Pass `--name "<device
  name>"` to select one explicitly.
- **`dump`** prints untouched HID input reports (`--seconds N` for a bounded run).
- **`diagnostics`** prints a redacted support bundle (app/Windows version, backend,
  model, HID usage, descriptor, settings, and recent log) with Bluetooth addresses,
  your Windows username, computer name, and device names removed — safe to paste
  into an issue. The GUI has the same thing under **Tools → Export diagnostics…**.
- **`repair`** is the one-click recovery (see below).

## The GUI

Launch with no arguments. The window opens in **Simple Mode** by default. Use the
mode button at the upper right to switch views:

- **Header band** with the app icon, title, version, an **Unofficial** tag, and a
  live status line: green *"Tracking <headset>"* while connected, or amber
  guidance while not. The guidance includes the one that matters most: **if you
  see nothing, press Repair Tracker (it needs one administrator approval)**. If
  AirPods are your only paired headphones, it says so and explains why they can't
  work.
- **Shared controls** in both modes include **Refresh** (re-enumerate and reconnect), **Repair Tracker**
  (one-click recovery, marked with the Windows UAC shield because it asks for a
  single administrator prompt), **Recenter** (set the current pose as forward;
  global hotkey **Ctrl+Alt+C**), and live axis tuning: axis order, Invert X/Y/Z,
  and a DPI-aware **Smoothing** slider with its live percentage (defaults to YXZ
  plus invert X and Z).
- **Simple Mode** shows concise OpenTrack setup help, large yaw/pitch/roll values,
  samples per second, packet age, backend, and the live orientation graph. It
  hides device paths, raw packets, protocol internals, and long logs.
- **Advanced Mode** adds the device list, **Show all devices**, descriptor and
  activity details, raw packet and motion readouts, full connection health, and
  the **Tools** menu for diagnostics and configuration import/export.
- **Output panel** shows OpenTrack on `UDP 127.0.0.1:4242` and JSON telemetry on
  `4243`. Simple Mode gives only the setup instruction; Advanced Mode adds the
  technical destination details.
- **Live orientation graph**, flicker-free (double-buffered) yaw/pitch/roll with a
  degree grid and a legend showing the current values in both modes. Visible
  readouts use fixed-width values and refresh at a comfortable visual rate while
  every sample is still streamed.

Settings (axis mapping, inversion, smoothing, port, UI mode, and window placement) are saved
to `%LOCALAPPDATA%\SonyHeadTracker\config.json` and restored on the next launch. If
the headset drops (sleep, output-device change, phone reconnect), the app
reconnects on its own with an increasing back-off (1, 2, 5, 10, 30 s); **Refresh**
or **Tools → Reconnect now** retries immediately.

The connected headset's name is shown in the title bar, and the GUI streams to UDP
`4242` (plus JSON on `4243`) the whole time it is open.

## OpenTrack

Choose **UDP over network** as the input, set its port to `4242`, start
`sony-head-tracker.exe bridge --port 4242` (or just leave the GUI open), then press
**Start** in OpenTrack. The application uses yaw, pitch, and roll in degrees.

## When the sensor won't show up

Windows sometimes pairs the headset but never creates the head-tracker HID node,
or parks it with Device Manager **Code 10**. This is especially common on a fresh
boot, which is why **Repair Tracker is the recommended first step when you open the
app**. The bridge has read-only diagnostics and a targeted, driver-only recovery
(it never installs a custom kernel driver):

1. **Repair Tracker** in the GUI, or `sony-head-tracker.exe repair`. This closes
   stale instances, auto-detects which paired headset advertises the head tracker,
   re-enables only that device's standard HID service, binds the failed node to
   Microsoft's inbox generic HID driver, verifies the `#AndroidHeadTracker#`
   marker, and reopens.
2. If `bluetooth-probe` finds the Android descriptor but `probe` doesn't, run
   `bluetooth-rebind`, wait for reconnection, and probe again.
3. If Device Manager shows the **HID Custom Sensor** with Code 10, open an
   **elevated** prompt and run `bluetooth-generic-hid` once.

Other tips:

- Close Sony utilities, spatial-audio tools, and anything else that may hold the
  device with exclusive access, then **Refresh**.
- If reports stay still, confirm Full Power, All Events, and a nonzero supported
  interval were accepted in the log. The tested WH-1000XM5 advertises 40 ms and
  produces about 25 packets per second.

## Protocol and security notes

The implementation follows the official Android Head Tracker HID protocol: it
accepts compatible version strings, reads report IDs and lengths from `HIDP_CAPS`,
accesses fields through `HidP_*`, and honours each value capability's logical and
physical ranges and HID unit exponent.

UDP output is loopback-only by default and has **no authentication**. Do not bind
or forward it to an untrusted network.

## Support development

Sony Head Tracker is free and open source.

If this project saved you from buying a TrackIR, Tobii, webcam tracker, phone-tracking app, or other head-tracking hardware, consider supporting development:

[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20me%20a%20coffee-support-yellow?style=for-the-badge)](https://buymeacoffee.com/nicholasslattery)

Support helps fund:

- testing more Sony headphones and earbuds
- OpenTrack setup improvements
- compatibility fixes for more devices
- continued free and open-source development

There is no paid version required. Donations are optional and help keep the project easier to install, test, and maintain.

## Contributing

Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for build, style,
and PR guidance, and [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) for community
expectations. Notable changes are tracked in [CHANGELOG.md](CHANGELOG.md).

## License

[MIT](LICENSE).
