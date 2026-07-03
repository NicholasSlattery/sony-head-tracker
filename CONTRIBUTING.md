# Contributing

Thanks for your interest in improving Sony Head Tracker for Windows! This is a
small, single-file project, which keeps contributing simple.

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
cl /std:c++latest /EHsc /permissive- /utf-8 /O2 /W4 /DUNICODE /D_UNICODE sony_head_tracker.cpp /Fe:sony-head-tracker.exe
```

The build must be **warning-clean at `/W4`**. CI compiles every push on
`windows-latest`; please make sure your change builds there too.

## Project layout

Everything lives in [`sony_head_tracker.cpp`](sony_head_tracker.cpp), organised into
clearly delimited sections (core types, logger, math, HID backend, Sensor API
backend, Bluetooth recovery, orientation filter, UDP output, GUI, CLI). Keep new
code in the section it belongs to, inside the `sony` namespace where practical.

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
