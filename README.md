# Lunaar Switch

Fast macOS-only CLI to trigger Logitech Easy-Switch host changes using HID++ 2.0 directly over `hidapi`.

## Requirements
- macOS
- Homebrew packages: `hidapi` and `pkgconf`

```sh
brew install hidapi pkgconf
```

## Build

From the repo root:

```sh
make
```

This produces the `lunaar-switch` binary.

## Usage

Switch the device to host slot 1–3 (1-based):

```sh
./lunaar-switch 2
```

The tool automatically:
- Enumerates Logitech devices via `hidapi`
- Finds the first HID++ 2.0 endpoint exposing `CHANGE_HOST` (feature 0x1814)
- Sends the host-switch write (fn 0x10) as a long HID++ report with no expected reply

## Notes
- Only switching is implemented; host-name queries/updates are omitted for speed.
- Automatic device selection stops at the first compatible device; if multiple receivers/devices are attached, unplug extras before switching.
- Tested flow matches Solaar’s `change-host` handling (long report 0x11, no reply expected).
