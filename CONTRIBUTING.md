# Contributing

Thanks for your interest in improving Sony Head Tracker for Windows! It's a small
project with a hardware-independent core, so most changes are easy to make and to
test without a headset.

## Ground rules

- Be respectful — see the [Code of Conduct](CODE_OF_CONDUCT.md).
- This project **never** spoofs another device, changes Bluetooth identities, or
  modifies headset firmware. Recovery is limited to read-only diagnostics and
  binding to Microsoft's inbox drivers. PRs that cross that line won't be merged.

## Building

You need a C++20-capable MSVC and a current Windows 11 SDK (install **Desktop
development with C++** in the Visual Studio Installer).

```bat
build.cmd
```

or, from a *x64 Native Tools Command Prompt for VS*:

```bat
cl /std:c++latest /EHsc /permissive- /utf-8 /O2 /W4 /DUNICODE /D_UNICODE /I include src\*.cpp app.res /Fe:sony-head-tracker.exe
```

The build must be **warning-clean at `/W4`**. CI compiles and unit-tests every push
on `windows-latest`; please make sure your change builds and passes there too.

## Project layout

Headers live in [`include/sony_head_tracker/`](include/sony_head_tracker) and
implementations in [`src/`](src), inside the `sony` namespace:

- **Pure core (no Windows):** `types` (incl. the normalized `MotionSample`),
  `math`, `hid_descriptor`, `orientation`, `protocol`. These compile into both the
  executable and the test binary.
- **Platform layer (Windows):** `logger`, `hid_backend`, `sensor_api_backend`,
  `bluetooth` (repair), `output_udp`, `gui`, `main`. These include
  `windows_prelude.hpp`, which owns the Windows/HID/Sensor/Bluetooth headers and
  the `#pragma comment(lib, ...)` link directives.

Keep hardware access in the platform layer and tracking maths in the pure core:
every backend should emit a `MotionSample`, and anything that can be tested without
a headset belongs in a pure unit rather than behind a Windows API.

## Tests

`build-tests.cmd` builds and runs the pure-core unit tests (no headset, no Windows
APIs) in [`tests/`](tests) — quaternion/Euler maths, signed bitfield decoding,
recenter/smoothing/axis mapping, JSON serialisation, and malformed-input handling.
Add a test alongside any change to the core, and run `build-tests.cmd` before you
open a PR.

## Style

- Match the surrounding code: C++20, RAII wrappers for Win32 handles, no raw
  `new`/`delete` for ownership, `std::format` for strings.
- Prefer narrow, well-named helpers over inline Win32 boilerplate.
- Don't reformat unrelated lines — keep diffs focused.
- If you change the UDP/JSON output, update [`docs/PROTOCOL.md`](docs/PROTOCOL.md)
  and bump the JSON `version` when the change is not backward-compatible.

## Testing your change

Because this talks to real Bluetooth hardware, describe in your PR how you
verified the change. Useful evidence:

- `sony-head-tracker.exe probe` output showing the descriptor.
- `sony-head-tracker.exe dump --seconds 5` for raw report changes.
- A short screen capture of the GUI for visual changes.

## Pull requests

1. Fork and create a topic branch.
2. Keep the change focused; one logical change per PR.
3. Fill in the PR template, including how you tested.
4. Add a line to the **Unreleased** section of [CHANGELOG.md](CHANGELOG.md).

## Reporting bugs

Open an issue using the bug report template. Bluetooth/HID behaviour varies a
lot across firmware and Windows builds, so please include your Windows version,
your Sony headset model and firmware version, and the relevant `probe`/log output.
