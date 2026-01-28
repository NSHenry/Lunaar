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
./lunaar-switch --slot 1
```

For faster performance with cached device parameters, use optional flags:

```sh
./lunaar-switch --path /dev/hidraw0 --devnum 2 --slot 2
```

### Optional Flags

- `-s` — Silent mode; suppress normal output (errors always printed)
- `--path PATH` — Direct hidapi path (e.g., `/dev/hidraw0`); skips enumeration
- `--devnum DEVNUM` — Device number (0–255, often 0xFF for receiver); skips device discovery
- `--feature-index INDEX` — Feature index for CHANGE_HOST (defaults to 14); skips feature lookup
- `--slot SLOT` — Host slot number (1–3); can also be positional argument

### Performance Tips

1. **First run (auto-discovery):**
   ```sh
   ./lunaar-switch 2
   ```
   This discovers your device and prints its path, device number, and feature index.

2. **Subsequent runs (instant):**
   ```sh
   ./lunaar-switch -s --path /dev/hidraw0 --devnum 2 --slot 2
   ```
   With all parameters cached, only sends the HID++ request—no enumeration or discovery.

### How It Works

The tool automatically:
- Enumerates Logitech devices via `hidapi` (unless `--path` is given)
- Finds the first HID++ 2.0 endpoint exposing `CHANGE_HOST` (feature 0x1814) (unless `--devnum` and `--feature-index` are given)
- Sends the host-switch write (fn 0x10) as a long HID++ report with no expected reply

## Notes
- Only switching is implemented; host-name queries/updates are omitted for speed.
- Automatic device selection stops at the first compatible device; if multiple receivers/devices are attached, unplug extras or use `--path` to target a specific one.
- Feature index defaults to 14 (observed constant on most Logitech receivers); use `--feature-index` to override if needed.
- Tested flow matches Solaar’s `change-host` handling (long report 0x11, no reply expected).

## Karabiner Elements

For seamless integration with Karabiner Elements, use the `lunaar-cached.sh` wrapper script which caches device parameters in `/tmp` (auto-cleared on reboot). This provides fast execution without discovery overhead while handling device path changes after reboots.

### Example Karabiner Rule
```json
{
    "conditions": [
        {
            "identifiers": [{ "vendor_id": "14248" }],
            "type": "device_if"
        }
    ],
    "description": "Execute Logitech Lunaar Easy-Switch Binary",
    "manipulators": [
        {
            "from": { "key_code": "f24" },
            "to": [{ "shell_command": "~/Developer/GitHub/Lunaar/scripts/lunaar-cached.sh 2" }],
            "type": "basic"
        }
    ]
}
```


Note: If script execution fails, then add /Library/Application Support/org.pqrs/Karabiner-Elements/bin/karabiner_console_user_server to the Input Monitoring list in System Preferences > Privacy & Security > Input Monitoring.