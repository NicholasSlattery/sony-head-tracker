# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims to
follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Native macOS 14+ support with a descriptor-driven IOHID backend, POSIX
  loopback UDP output, a command-line bridge, and a SwiftUI application.
- A small C ABI around the shared C++ tracking engine so SwiftUI and the CLI use
  the same parsing, filtering, recentering, and protocol serialization.
- Redacted shareable macOS diagnostics and explicit permission, visibility,
  marker-verification, feature-write, stream-timeout, and UDP error categories.
- Persistent macOS axis mapping, inversion, smoothing, and UDP port settings
  under the user's Application Support directory.
- Address-scoped recovery for the previously verified paired headset after a
  Bluetooth power cycle. Recovery uses public IOBluetooth connection and SDP
  APIs and never scans, pairs, forgets, or modifies another device.
- A zero-filled Core Audio A2DP keepalive for headsets that expose the tracker
  only while their Bluetooth audio path is active. This behavior is required by
  the tested ULT WEAR and remains active only for the tracking session.
- macOS core/C API tests in GitHub Actions while retaining the existing Windows
  build, tests, and release jobs.

### Changed
- The macOS App now handles a stalled sample stream with a separate short
  backoff, one SDP refresh, and IOHID/silent-audio recycling without deliberately
  disconnecting the headset's Bluetooth baseband connection.
- macOS reconnect backoff now wakes as soon as the exact paired headset reconnects
  or its Android Head Tracker IOHID collection becomes visible.
- The macOS build/run script can use a unique local Apple Development identity,
  only after explicit opt-in with `SHT_ENABLE_STABLE_SIGNING=1`, so Input
  Monitoring permission can survive local rebuilds without default Keychain access.
- Portable floating-point configuration parsing now uses a classic-locale
  stream instead of libc++ floating-point `from_chars`, preserving the macOS 14
  deployment target.

### Compatibility
- ULT WEAR is hardware-validated on macOS with changing orientation data and
  both UDP outputs. The macOS discovery and report parsing remain protocol-based:
  they do not hardcode Sony model names, VID/PID values, report IDs, report
  lengths, or feature bytes. XM5, XM6, and other compatible models therefore use
  the same path but still require model-specific macOS community confirmation.

## [2.1.0] - 2026-07-09

Performance release. The per-packet hot path and the GUI were made faster and
lighter with **no change to behaviour or output**: every produced byte, decoded
sensor value, UI string, port, and the OpenTrack/JSON wire format is identical to
2.0.0, and all 41 unit tests still pass. A local benchmark harness with built-in
equivalence checks was added to prove and guard this.

### Changed
- **Faster JSON telemetry serialisation.** The JSON datagram is now built with
  `std::to_chars` appends into a reused buffer instead of a per-packet
  `std::format` call with two intermediate string allocations, and the gyroscope
  vector is formatted once and reused for its deprecated `angularVelocity` alias.
  Output is byte-for-byte identical (verified against the previous implementation
  over a randomized sample corpus); roughly 3× faster in local benchmarks.
- **Lower-overhead UDP output.** The JSON destination address is computed once at
  `open()` and one serialisation buffer is reused per `send()`, removing a
  per-packet address rebuild and heap allocation. Both datagrams, their bytes, and
  their destinations are unchanged.
- **Faster HID packet decoding.** Packed descriptor fields are read with a
  little-endian word assembly instead of a per-bit loop, and the HID
  unit-exponent powers of ten are cached, so decoded values are bit-identical
  while decoding is roughly 4× faster locally. The HID reader thread now reuses
  scratch buffers, so the per-packet parse loop no longer allocates.
- **Cheaper GUI under load.** Visible yaw/pitch/roll, motion, and raw-packet text
  is now *formatted* at the display refresh rate (10 Hz, raw 5 Hz) from the newest
  sample, rather than formatted on every packet and only throttled at draw time,
  so the GUI thread does progressively less work as the packet rate rises. The
  double-buffered window paint caches its back-buffer bitmap across frames
  (rebuilt on resize) and blits only the invalidated region, instead of
  allocating a window-sized bitmap on every repaint. The visible result is
  identical, including the existing flicker-free graph.

### Added
- **Local benchmark harness** (`bench/`, built and run by `build-bench.cmd`) that
  times the per-packet hot path against a simulated packet source (no headset
  required) and, on every run, verifies that decoded values stay bit-identical and
  the JSON output stays byte-identical to reference implementations. A
  `performance_optimization_report.md` records the before/after measurements and
  methodology.
- Two additive internal helpers used by the above (`toJsonTo`,
  `decodePackedDescriptorValuesInto`); no existing function was changed or removed.

### Compatibility
- No functional change. OpenTrack UDP on port 4242, JSON telemetry on port 4243,
  the JSON schema and its exact bytes, sensor parsing, orientation/filter/axis
  math, supported devices, GUI text and behaviour, and repair/probe/bridge
  behaviour are all unchanged.

## [2.0.0] - 2026-07-04

### Added
- **Simple Mode and Advanced Mode.** Simple Mode is the default for new and
  existing configurations and focuses on connection state, Recenter, Repair
  Tracker, Refresh, axis/inversion controls, smoothing, OpenTrack setup, large
  yaw/pitch/roll values, packet health, and the live orientation graph. Advanced
  Mode retains the full device list, HID details, activity log, raw packet and
  motion readouts, connection diagnostics, and configuration/diagnostics tools.
- **Persistent UI mode.** The selected mode is stored in the existing tolerant
  `%LOCALAPPDATA%\SonyHeadTracker\config.json` format. Older configurations
  remain valid and open in Simple Mode.
- **Live smoothing value.** The toolbar now shows the current value as
  `Smoothing: 18%` and updates it while the slider moves.

### Changed
- **Cleaner normal-user experience.** Simple Mode replaces protocol internals
  with concise OpenTrack and JSON destinations plus direct setup guidance. Debug
  controls and the Tools menu are available in Advanced Mode without removing
  any existing repair, recenter, diagnostics, import/export, or reconnect
  functionality.
- **More readable telemetry.** Yaw, pitch, and roll use large fixed-width values;
  advanced gyroscope and accelerometer data use stable X/Y/Z columns with
  consistent degrees, rad/s, and m/s² units. Samples per second, packet age, and
  backend are easier to scan.
- **DPI-aware GUI layout.** Fonts, spacing, controls, graph regions, minimum and
  restored window sizes, and icons scale with the active monitor at 100%, 125%,
  150%, and 200%. The smoothing caption is measured from its widest text instead
  of using a clipping-prone fixed width.
- **Simplified GUI wording.** Em dashes and en dashes were removed from all
  GUI-visible strings, and connection/recovery guidance now uses shorter
  sentences and separators.
- Compatibility: **Sony ULT WEAR (WH-ULT900N)** confirmed working (community);
  promoted from Candidate to Confirmed in the README compatibility table.

### Fixed
- **Live readout flicker.** Sample processing, filtering, graph history, and UDP
  and JSON output still run for every packet, while visible orientation and
  motion text is coalesced to 10 Hz and raw packets to 5 Hz. Each control caches
  its last displayed string so unchanged text does not call `SetWindowTextW`.
- **Smoothing label clipping.** `Smoothing` is fully visible at the default size,
  common Windows scaling levels, and after monitor-DPI changes.
- **Unnecessary repainting.** Live samples continue invalidating only the graph
  rectangle; the existing double-buffered window paint remains in place.

### Compatibility
- OpenTrack UDP on port 4242, JSON telemetry on port 4243, the JSON schema,
  sensor parsing, orientation/filter/axis math, repair behavior, and device
  compatibility behavior are unchanged.

## [1.4.0] - 2026-07-03

### Fixed
- **Windows Sensor API fallback no longer drops orientation-only devices.** The
  fallback previously published a sample only when it received *both* a rotation
  quaternion and three angular-velocity values. The protocol allows the gyroscope
  to be `null`, so a device exposing orientation without angular velocity now
  produces tracking (with `hasAngularVelocity = false`), matching the HID backend.

### Changed
- Compatibility: **WH-1000XM6** confirmed working (community); promoted from
  Candidate to Confirmed in the README compatibility table.
- **Refactored the single source file into modular units.** The code is now split
  into a hardware-independent core (`include/sony_head_tracker/` + `src/`:
  quaternion maths, HID descriptor decoding, the orientation filter, and protocol
  serialisation) and a Windows platform layer (HID/Sensor backends, Bluetooth
  repair, UDP output, GUI). Every backend produces a normalized `MotionSample`
  (`std::optional` angular velocity / acceleration). The executable remains
  dependency-free. No behaviour change to the CLI, GUI, or UDP/JSON output.
- `Vec3::operator[]` is now bounds-checked (`assert`) instead of silently
  returning `z` for out-of-range indices.
- **Single source of truth for the version.** `include/sony_head_tracker/version.h`
  feeds both the C++ code and the resource script (`app.rc`); the manifest's
  assembly-identity version is intentionally static. CI fails a tagged build whose
  executable version does not match the tag.
- **Hardened CI.** All GitHub Actions are pinned to full commit SHAs (with
  Dependabot keeping them updated), and the workflow is split into a least-privilege
  build/test job (`contents: read`) and a release job (`contents: write`, only on
  version tags).

### Added
- **Persisted GUI settings.** Axis mapping, inversion, smoothing, UDP port,
  "show all devices", and window placement are saved to
  `%LOCALAPPDATA%\SonyHeadTracker\config.json` and restored on launch. A **Tools**
  menu adds *Reset settings to defaults*, *Import configuration…*, and *Export
  configuration…*, and the health line flags when a non-default axis mapping is active.
- **Live connection-health readout** in the GUI: samples/second, time since last
  packet, active backend, whether angular velocity is present, UDP packets sent and
  destination port, reconnection attempts, and sensor data age.
- **Automatic reconnection with back-off** (1, 2, 5, 10, 30 s) plus a **Reconnect
  now** menu item; **Refresh** resets the back-off.
- **Redacted diagnostics export.** *Tools → Export diagnostics…* in the GUI and a
  new `sony-head-tracker.exe diagnostics` command produce a support bundle
  (version, Windows build, backend, model, HID usage, descriptor, packet rate,
  settings, recent log) with Bluetooth addresses, the Windows username, computer
  name, and known device names removed.
- **Unit tests for the pure core** (`tests/`, built and run by `build-tests.cmd`
  and in CI): quaternion/Euler conversion, signed HID bitfield extraction,
  recenter/smoothing/axis mapping, JSON serialisation, malformed/truncated or
  angular-velocity-less input handling, config round-trip, and diagnostics redaction
  — all without a headset.
- **Compatibility-report issue form** (`.github/ISSUE_TEMPLATE/compatibility_report.yml`)
  capturing model, firmware, Windows version, which path worked, probe output,
  yaw/pitch/roll correctness, and table-inclusion consent.
- **Signed, verifiable release packages.** Pushing a `vX.Y.Z` tag publishes
  `sony-head-tracker-vX.Y.Z-windows-x64.zip` plus `SHA256SUMS.txt` (executable,
  README, LICENSE, `docs/PROTOCOL.md`, and a sample OpenTrack profile), with a
  GitHub build-provenance attestation so downloads can be verified.

## [1.3.0] - 2026-07-02

### Changed
- **Rebranded to "Sony Head Tracker for Windows."** The project began as an
  experiment with the WH-1000XM5; community testing confirmed that the same
  Android Head Tracker protocol is shared by other Sony devices, so the project
  is now a general Sony head-tracking bridge for Windows. Renamed the executable
  (`sony-head-tracker.exe`), the source file (`sony_head_tracker.cpp`), the
  internal namespace (`sony`), the window class, the GUI title, and the version
  resources (product name, file description, manifest identity). The
  `XM5 Head Tracker Bridge` name is retained in this changelog as history.
- **README rewritten to be Sony-generic.** New identity and positioning, a
  compatibility model based on the exposed protocol (Confirmed / Community
  confirmed / Candidate / Not compatible) rather than the model name, quick-start
  guidance that emphasises pressing **Repair Tracker** on a fresh boot, and
  preserved WH-1000XM5 search phrases so existing links still land.
- Compatibility: **WF-1000XM6** confirmed working (community); **WH-1000XM4**
  confirmed *not* working (community).

### Added
- **Labelled smoothing slider.** The orientation-smoothing trackbar in the GUI
  toolbar now carries a visible **Smoothing** caption so its purpose is clear.
- **"Unofficial" tag in the GUI header** (and the CLI banner), making clear the
  project is not affiliated with or endorsed by Sony.

## [1.2.0] - 2026-07-02

### Added
- **Application icon and version resources.** The exe now carries a proper
  Windows icon (Explorer, taskbar, Alt-Tab, and the window itself), version
  info, and a Common Controls v6 + PerMonitorV2-DPI manifest, embedded from
  `app.rc` at build time. The icon artwork is generated by
  `tools/make-icon.ps1` and committed as `app.ico`.
- **GUI facelift.** New header band with the app icon and a live status
  banner: green "Tracking <headset>" while connected, amber guidance while
  not — including "press Repair Tracker (admin approval required)" when
  nothing is found, and an AirPods explanation when AirPods are the only
  paired headphones. The Repair Tracker button carries the Windows UAC
  shield so the admin prompt is no surprise.
- **De-noised device list.** Only head-tracker candidates are shown by
  default (sorted first, labelled by Bluetooth name); a "Show all devices"
  checkbox reveals every HID collection. An empty list states exactly what
  to do next, and the details pane shows step-by-step recovery guidance.
- **Clear output panel.** A dedicated OUTPUT section spells out both UDP
  endpoints (OpenTrack doubles on `:4242`, JSON telemetry on `:4243`) with
  what each is for and that the stream is loopback-only.
- Live legend values on the orientation graph, dark-themed buttons,
  scrollbars, and combo box, a minimum window size, and a larger default
  window.

### Fixed
- **CLI output no longer truncates at non-ANSI device names.** Console
  output is UTF-8 now; previously a device name like "Nicholas's AirPods
  Pro" (curly apostrophe) silently killed all further `probe`/`bridge`
  console output.

## [1.1.0] - 2026-07-02

### Added
- **Headset detection.** The bridge now resolves which paired Bluetooth headset
  the head-tracker HID node belongs to (via the PnP parent chain) and shows the
  name in the GUI title bar and device list, in `probe` and `bridge` output,
  and as a new additive `device` field in the JSON telemetry (`null` when
  unresolved).
- **AirPods awareness.** Paired AirPods are recognised in `probe`, `bridge`,
  `bluetooth-probe`, and the GUI, with an explanation of why they cannot
  provide head tracking on Windows: Apple uses its proprietary accessory
  protocol over a raw L2CAP channel (PSM `0x1001`) that desktop Windows only
  exposes to kernel-mode profile drivers, and this project never installs a
  custom kernel driver. A README section documents the details.
- `bluetooth-probe` prints a per-device verdict after each SDP query (Android
  Head Tracker advertised / AirPods explanation).

### Changed
- **Any-headset support, no Sony assumptions.** `repair` and `bluetooth-rebind`
  no longer hardcode `WH-1000XM5`: with no `--name`, they auto-detect the
  headset whose SDP record carries the Android Head Tracker HID descriptor
  (read-only check), so no unrelated Bluetooth device's services are ever
  touched. `bluetooth-probe` now defaults to listing every paired device;
  deep GATT reads still require `--name` or `--all-le`.
- GUI title, usage text, and messages are generic ("Head Tracker Bridge");
  the waiting message no longer assumes an XM5.

## [1.0.0] - 2026-07-01

### Added
- `help` and `version` commands (with `--help` / `--version` aliases); unknown
  commands now print the full usage text, and the GUI title bar shows the
  project version.
- Tagged releases: pushing a `v*` tag builds `xm5-headtracker.exe` in CI and
  attaches it to a GitHub Release automatically.
- `bridge --port` is validated (1–65534, since the JSON stream uses port + 1).
- **Accelerometer exposure.** Input reports are parsed for the standard HID
  sensor-page acceleration usages (`0x0452`–`0x0455`); when present they are
  emitted as the JSON `accelerometer` array and shown live in the GUI.
- **Explicit gyroscope field.** Angular velocity is now also parsed from the
  standard sensor-page usages (`0x0456`–`0x0459`) and emitted as `gyroscope`
  (rad/s). `angularVelocity` is retained as a deprecated alias.
- Bridge startup and the GUI now state exactly which UDP ports the data goes to.
- `docs/PROTOCOL.md` documenting the full UDP/JSON wire format.
- Open-source project scaffolding: contributing guide, code of conduct, issue
  and PR templates, `.editorconfig`, and a CI build workflow.

### Fixed
- **OpenTrack head rotation now works.** The OpenTrack UDP packet was sending the
  head angles in the translation slots (`yaw, pitch, roll, 0, 0, 0`); OpenTrack
  reads six doubles as `x, y, z, yaw, pitch, roll`, so rotation (including roll /
  head tilt) was being fed into translation and the rotation axes stayed zero.
  The angles are now sent in the correct last three slots.

### Changed
- Builds are now warning-clean at **`/W4`** (previously `/W3`); `build.cmd`, CI,
  and the docs all use the stricter level.
- Default axis convention is now **YXZ with X and Z inverted** (previously Z
  only); the GUI's Invert X and Invert Z checkboxes both start checked.
- **GUI no longer flickers.** The live graph is now double-buffered, the window
  clips its children, background erase is suppressed, and only the graph
  rectangle is invalidated per sample. Child controls are dark-themed for a
  cohesive look, and the graph gained a degree grid, legend, and axis labels.
- JSON telemetry schema bumped to `version: 2`.
- The configured axis convention is now applied to the accelerometer vector too,
  so all streams share one coordinate frame.

## [0.1.0]

### Added
- Initial single-file bridge: HID + Windows Sensor API backends, orientation
  filtering with recenter and drift correction, OpenTrack + JSON UDP output,
  diagnostics GUI, and one-click driver-only "Repair Tracker" recovery.

[Unreleased]: https://github.com/NicholasSlattery/sony-head-tracker/compare/v2.1.0...HEAD
[2.1.0]: https://github.com/NicholasSlattery/sony-head-tracker/compare/v2.0.0...v2.1.0
[2.0.0]: https://github.com/NicholasSlattery/sony-head-tracker/compare/v1.4.0...v2.0.0
[1.4.0]: https://github.com/NicholasSlattery/sony-head-tracker/compare/v1.3.0...v1.4.0
[1.3.0]: https://github.com/NicholasSlattery/sony-head-tracker/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/NicholasSlattery/sony-head-tracker/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/NicholasSlattery/sony-head-tracker/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/NicholasSlattery/sony-head-tracker/compare/v0.1.0...v1.0.0
[0.1.0]: https://github.com/NicholasSlattery/sony-head-tracker/releases/tag/v0.1.0
