# XM5 Head Tracker Bridge

Head tracking on the **Sony WH-1000XM5** — and any other headset that speaks
the Android Head Tracker protocol — on Windows, in a single C++ file.

[![Build](https://github.com/NicholasSlattery/xm5-head-tracker/actions/workflows/build.yml/badge.svg)](https://github.com/NicholasSlattery/xm5-head-tracker/actions/workflows/build.yml)
[![Latest release](https://img.shields.io/github/v/release/NicholasSlattery/xm5-head-tracker)](https://github.com/NicholasSlattery/xm5-head-tracker/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform: Windows 11](https://img.shields.io/badge/platform-Windows%2011-0078D6.svg)](#build)
[![Language: C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg)](#build)

Your WH-1000XM5 has a full motion sensor in it — Sony uses it for head-tracked
spatial audio on phones, and the headset exposes it over Bluetooth as an
[Android Head Tracker HID sensor](https://source.android.com/docs/core/interaction/sensors/head-tracker-hid-protocol).
On a Windows PC that data just sits there: no Sony software reads it, no
Windows API surfaces it, and (as far as I can find) no other tool exists for
it. **This bridge is how you get live head-tracking data out of a WH-1000XM5
on Windows.**

It discovers the sensor, detects **which headset** it belongs to (the paired
Bluetooth device's name is shown in the GUI, the CLI, and the JSON stream),
enables reporting, parses the headset's orientation, and streams it out over
UDP — as
[OpenTrack](https://github.com/opentrack/opentrack) doubles **and** a JSON
datagram — so you can drive sim/game head-look, spatial audio, or anything else
that wants to know where your head is pointing. It also ships a flicker-free
diagnostics GUI with a live yaw/pitch/roll graph and a one-click **Repair
Tracker** button for the times Windows refuses to enumerate the sensor.

All the code is one file: [`xm5_head_tracker.cpp`](xm5_head_tracker.cpp) —
plus a small resource bundle ([`app.rc`](app.rc): icon, version info, and the
Common Controls manifest) that gets embedded at build time.

## Quick start

1. Grab `xm5-headtracker.exe` from the
   [latest release](https://github.com/NicholasSlattery/xm5-head-tracker/releases/latest)
   (or [build it yourself](#build) — it's one `cl` command).
2. [Pair the headphones](#pair-the-headphones) in Windows 11.
3. Double-click the exe. The GUI finds the tracker, draws your head's live
   yaw/pitch/roll, and is already streaming to UDP `4242`/`4243`.
4. For games: point OpenTrack's "UDP over network" input at port `4242` — see
   [OpenTrack](#opentrack). For your own code: read one JSON object per sample
   from port `4243` — see [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

If the sensor doesn't show up, press **Repair Tracker** once and approve the
administrator prompt — that's what it's for.

> **Hardware status:** changing orientation reports have been received and parsed
> from a physical WH-1000XM5. Behaviour varies with Sony firmware and the Windows
> Bluetooth stack; this project never spoofs another headset, changes Bluetooth
> identities, or modifies firmware.

## Contents

- [Quick start](#quick-start)
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
- [Protocol & security notes](#protocol--security-notes)
- [Contributing](#contributing)
- [License](#license)

## Compatibility

This bridge doesn't care about the brand or model name — it detects **any**
headset that exposes the standard
[Android Head Tracker HID sensor](https://source.android.com/docs/core/interaction/sensors/head-tracker-hid-protocol)
over Bluetooth (usage `0x0020:0x00E1` with an `#AndroidHeadTracker#` marker). So
whether a given pair works comes down to one thing: does that headset advertise
the sensor to Windows?

**You can check in seconds, with zero risk.** Pair the headphones, then run:

```bat
xm5-headtracker.exe probe
```

`probe` is read-only — it never writes to the device, changes drivers, or
touches firmware. If it finds a verified Android head tracker it prints the
descriptor and exits `0`; if not, it just lists what it saw and exits `2`.

| Headset | Head tracking | Reported by |
| ------- | ------------- | ----------- |
| Sony WH-1000XM5 \* | ✅ Works | maintainer (tested) |
| Sony WF-1000XM6 | ✅ Works | community-confirmed |
| Sony WH-1000XM6 | ❓ Untested — likely | Sony lists head-tracking support |
| Sony WF-1000XM5 | ❓ Untested — likely | Sony lists head-tracking support |
| Sony LinkBuds Open (WF-L910) | ❓ Untested — likely | Sony lists head-tracking support |
| Sony LinkBuds Fit (WF-LS910N) | ❓ Untested — likely | Sony lists head-tracking support |
| Sony ULT WEAR (WH-ULT900N) | ❓ Untested — likely | Sony lists head-tracking support |
| Sony LinkBuds (WF-L900) \* | ❓ Untested — likely | Sony lists head-tracking support |
| Sony LinkBuds S (WF-LS900N) \* | ❓ Untested — likely | Sony lists head-tracking support |
| Sony WH-1000XM4 | ❌ Does not work | community-confirmed |
| Sony WH-1000XM3 | ❌ Unlikely | not on Sony's head-tracking list |
| Bose QuietComfort Ultra Headphones | ❓ Untested — possibly in the future | has head-tracked spatial audio |
| Bose QuietComfort Ultra Earbuds | ❓ Untested — possibly in the future | has head-tracked spatial audio |
| Other Bose models | ❌ Unlikely | no head-tracked spatial audio |
| Apple AirPods (all models, incl. AirPods Pro) | ❌ Not possible on Windows | see below |

\* These models need the latest firmware first: install Sony's **Sound Connect**
app, connect the headphones, and update before probing.

Sony's own documentation lists the models above as supporting its head-tracking
feature. The **WH-1000XM5** (maintainer) and **WF-1000XM6** (community) are
confirmed working, both exposing the sensor to Windows through the standard
Android Head Tracker protocol; the other listed Sony models are therefore
likely to work (possibly not today on current firmware, but very likely as
firmware catches up). The **WH-1000XM4** has been confirmed *not* to work, which
matches its absence from Sony's head-tracking list — models not on that list
almost certainly carry no usable sensor. The Bose QuietComfort Ultra pair do
head-tracked spatial audio, so they may eventually expose the same protocol, but
that is unverified; other Bose models are unlikely. If you run `probe` on
anything listed as untested, please
[open an issue](https://github.com/NicholasSlattery/xm5-head-tracker/issues/new/choose)
with the output — working or not — and I'll update this table.

### Why AirPods can't work (yet)

AirPods absolutely have the motion sensors — but Apple does not implement the
Android Head Tracker HID protocol. Head tracking (and ANC control, ear
detection, etc.) travels over **Apple's proprietary accessory protocol (AAP)**
on a raw Bluetooth Classic L2CAP channel (PSM `0x1001`). Two hard blockers
follow:

1. **Windows has no user-mode API for that channel.** Desktop Windows exposes
   RFCOMM sockets to applications, but custom Bluetooth Classic L2CAP channels
   can only be opened by a **kernel-mode profile driver**
   ([Microsoft's docs](https://learn.microsoft.com/en-us/windows-hardware/drivers/bluetooth/creating-a-l2cap-client-connection-to-a-remote-device)).
   That's how [MagicPods](https://magicpods.app/magicaap/) does it on Windows —
   it ships a custom driver. This project's hard rule is that it **never
   installs a custom kernel driver**, so that path is out of scope here.
2. **The head-tracking payload isn't publicly documented.** The
   [LibrePods](https://github.com/kavishdevar/librepods) project has
   reverse-engineered much of AAP, but head-orientation packets remain
   unexplored territory even there.

What this bridge does instead: it **recognises paired AirPods** and tells you
exactly this — in `probe`, in `bridge`, and in the GUI — instead of failing
silently. If Windows ever exposes user-mode L2CAP, or a maintained open
AAP-bridge appears, an AirPods backend becomes feasible and contributions are
welcome.

## Where the data goes (ports)

In `bridge` mode (and while the GUI is open) head-tracking data is streamed over
**UDP to loopback (`127.0.0.1`)** on two adjacent ports:

| Port            | Format                          | Consumer                          |
| --------------- | ------------------------------- | --------------------------------- |
| **`4242`** (`--port`) | Six little-endian `double`s `(x, y, z, yaw, pitch, roll)` | OpenTrack "UDP over network" |
| **`4243`** (`--port` + 1) | UTF-8 JSON object (see below)   | Your own apps / scripts           |

Change the base port with `--port N`; the JSON port is always `N + 1`. The
bridge prints both destinations on startup, and the GUI shows them along the
bottom edge. Translation axes `(x, y, z)` are always zero — this protocol
reports orientation only.

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

- **Gyroscope** — angular velocity in **rad/s**, emitted as the JSON
  `gyroscope` array (and shown live in the GUI). This is the WH-1000XM5's
  on-board gyro as carried by the Android Head Tracker protocol.
- **Accelerometer** — linear acceleration in **m/s²**, emitted as the JSON
  `accelerometer` array **when the device reports it**.

  The Android Head Tracker HID profile that the XM5 advertises defines only
  orientation + gyro fields, so on current Sony firmware `accelerometer` is
  typically `null`. The bridge nonetheless parses the standard HID sensor-page
  acceleration usages (`0x0453`–`0x0455`, plus the vector form `0x0452`) from
  every input report, so if your firmware exposes them they are surfaced
  automatically — no code change needed. Gyro is likewise parsed from both the
  head-tracker custom field (`0x0545`) and the standard sensor-page usages.

## Default orientation: YXZ, X and Z inverted

The default axis convention is **YXZ order with the X and Z axes inverted** — the
mapping that produces correct head tracking on the WH-1000XM5. It is fully
overridable:

- **CLI:** `--axis-map XYZ` (any permutation) and `--invert X` (any axes). The
  `--invert` flag is a complete override — `--invert xz` reproduces the default,
  `--invert z` inverts Z only, and `--invert none` clears all inversions.
- **GUI:** the axis-order dropdown (defaults to YXZ) and the Invert X/Y/Z
  checkboxes (Invert X and Z start checked).

The same axis convention is applied to the gyroscope and accelerometer vectors
so all streams share one coordinate frame.

## Build

Requires a C++20-capable MSVC and a current Windows 11 SDK
(install **Desktop development with C++** in the Visual Studio Installer).

```bat
build.cmd
```

`build.cmd` locates the Visual Studio C++ tools automatically. Or build by hand
from a *x64 Native Tools Command Prompt for VS*:

```bat
rc /nologo app.rc
cl /std:c++latest /EHsc /permissive- /utf-8 /O2 /W4 /DUNICODE /D_UNICODE xm5_head_tracker.cpp app.res /Fe:xm5-headtracker.exe
```

`app.rc` embeds the app icon, version info, and the Common Controls v6 manifest
(the icon itself is generated by [`tools/make-icon.ps1`](tools/make-icon.ps1)
and committed as `app.ico`). All required import libraries are pulled in via
`#pragma comment(lib, ...)`, so no extra linker arguments are needed. The build
is warning-clean at `/W4`.
Every push is built in CI on `windows-latest`, and pushing a `v*` tag publishes
the exe as a GitHub Release — see
[`.github/workflows/build.yml`](.github/workflows/build.yml).

## Pair the headphones

1. Update the XM5 with Sony's app and enable its spatial-audio / head-tracking
   feature if available, then put the headset in pairing mode.
2. In Windows 11: **Settings → Bluetooth & devices → Add device → Bluetooth**,
   pair **WH-1000XM5**.
3. Run `xm5-headtracker.exe probe`. A usable collection shows usage
   `0x0020:0x00E1` and a feature description beginning with `#AndroidHeadTracker#`.

## Usage

```text
xm5-headtracker.exe                 (no args -> diagnostics GUI)
xm5-headtracker.exe probe [--include-disabled]
xm5-headtracker.exe dump [--seconds N]
xm5-headtracker.exe repair
xm5-headtracker.exe bluetooth-probe [--all-le] [--name FILTER]
xm5-headtracker.exe bluetooth-rebind [--name FILTER]   (default: auto-detect)
xm5-headtracker.exe bluetooth-generic-hid        (run from an elevated prompt)
xm5-headtracker.exe bridge [--port 4242] [--seconds N]
                           [--axis-map YXZ] [--invert XZ] [--smoothing 0.18]
xm5-headtracker.exe help | version
```

- **`bridge`** is the main mode. It prints which headset it is tracking,
  reconnects automatically, and streams to the ports described in
  [Where the data goes](#where-the-data-goes-ports).
- **`probe`** prints discovered HID collections and Sensor API custom sensors,
  names the headset a verified tracker belongs to, and explains why nothing was
  found (including the AirPods case).
- **`bluetooth-rebind`** (and `repair`) auto-detect the headset by checking
  which paired device's SDP record carries the Android Head Tracker descriptor —
  no other Bluetooth device's services are ever touched. Pass
  `--name "<device name>"` to select one explicitly.
- **`dump`** prints untouched HID input reports (`--seconds N` for a bounded run).
- **`repair`** is the one-click recovery (see below).

## The GUI

Launch with no arguments. The window is organised top to bottom:

- **Status banner** — a live headline under the title: green
  *"Tracking WH-1000XM5"* while connected, or amber guidance while not —
  including the one that matters: **if you see nothing, press Repair Tracker
  (it needs one administrator approval)**. If AirPods are your only paired
  headphones, it says so and explains why they can't work.
- **Toolbar** — **Refresh** (re-enumerate and reconnect), **Repair Tracker**
  (one-click recovery, marked with the Windows UAC shield because it asks for
  a single administrator prompt), **Recenter** (set the current pose as
  forward; global hotkey **Ctrl+Alt+C**), and live axis tuning (axis order /
  Invert X·Y·Z / smoothing, defaulting to YXZ + invert-X-and-Z).
- **Devices** — only head-tracker candidates are listed by default, by their
  Bluetooth name (`✔ WH-1000XM5 — Android head tracker`); tick **Show all
  devices** to inspect every HID collection Windows exposes. When the list is
  empty it says exactly what to do next.
- **Details & activity log** — full descriptor detail for the selection plus
  the discovery/permission log; shows step-by-step recovery guidance when no
  tracker is visible.
- **Output panel** — spells out where the data goes: OpenTrack doubles on
  `UDP 127.0.0.1:4242`, JSON telemetry on `4243`, loopback only. No guessing.
- **Live orientation graph** — flicker-free (double-buffered) yaw/pitch/roll
  with a degree grid and a legend showing the current values, plus raw-packet
  and gyroscope/accelerometer readouts above it.

The connected headset's name is shown in the title bar, and the GUI streams to
UDP `4242` (+JSON on `4243`) the whole time it is open.

## OpenTrack

Choose **UDP over network** as the input, set its port to `4242`, start
`xm5-headtracker.exe bridge --port 4242`, then press **Start** in OpenTrack. The
application uses yaw/pitch/roll in degrees.

## When the sensor won't show up

Windows sometimes pairs the headset but never creates the head-tracker HID
node, or parks it with Device Manager **Code 10**. The bridge has read-only
diagnostics and a targeted, driver-only recovery (it never installs a custom
kernel driver):

1. **Repair Tracker** in the GUI, or `xm5-headtracker.exe repair`. This closes
   stale instances, auto-detects which paired headset advertises the head
   tracker, re-enables only that device's standard HID service, binds the
   failed node to Microsoft's inbox generic HID driver, verifies the
   `#AndroidHeadTracker#` marker, and reopens.
2. If `bluetooth-probe` finds the Android descriptor but `probe` doesn't, run
   `bluetooth-rebind`, wait for reconnection, and probe again.
3. If Device Manager shows the **HID Custom Sensor** with Code 10, open an
   **elevated** prompt and run `bluetooth-generic-hid` once.

Other tips:

- Close Sony utilities, spatial-audio tools, and anything else that may hold the
  device with exclusive access, then **Refresh**.
- If reports stay still, confirm Full Power, All Events, and a nonzero supported
  interval were accepted in the log. The tested XM5 advertises 40 ms and produces
  about 25 packets/second.

## Protocol & security notes

The implementation follows the official Android Head Tracker HID protocol: it
accepts compatible version strings, reads report IDs and lengths from
`HIDP_CAPS`, accesses fields through `HidP_*`, and honours each value
capability's logical/physical ranges and HID unit exponent.

UDP output is loopback-only by default and has **no authentication** — do not
bind or forward it to an untrusted network.

## Contributing

Contributions are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md) for build,
style, and PR guidance, and [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) for
community expectations. Notable changes are tracked in
[CHANGELOG.md](CHANGELOG.md).

## License

[MIT](LICENSE).
