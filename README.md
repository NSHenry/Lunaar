# Lunaar Switch

Fast macOS-only Rust CLI to trigger Logitech Easy-Switch host changes using HID++ 2.0 directly over `hidapi`.

This project was inspired by [Solaar](https://github.com/pwr-Solaar/Solaar)'s `change-host` implementation but optimized for speed and simplicity on macOS. I primarily use it with a keyboard macro tool (Karabiner-Elements) to switch my non-Logitech keyboard and MX Master seamlessly between hosts.

Contributions and improvements are welcome!

## Requirements
- macOS
- Rust toolchain (`cargo`)

```sh
brew install rust
```

## Build

From the repo root:

```sh
make
```

This builds the Rust binary with Cargo and copies it to `bin/lunaar-switch`.

## Usage

Switch the device to host slot 1–3 (1-based):

```sh
./bin/lunaar-switch 2
./bin/lunaar-switch --slot 1
```

For faster performance with cached device parameters, use optional flags:

```sh
./bin/lunaar-switch --path /dev/hidraw0 --devnum 2 --slot 2
```

Or use built-in cache handling (default behavior):

```sh
./bin/lunaar-switch --cache auto 2
```

### Optional Flags

- `-s` — Silent mode; suppress normal output (errors always printed)
- `--path PATH` — Direct hidapi path (e.g., `/dev/hidraw0`); skips enumeration
- `--devnum DEVNUM` — Device number (0–255, often 0xFF for receiver); skips device discovery
- `--feature-index INDEX` — Feature index for CHANGE_HOST (defaults to 14); skips feature lookup
- `--slot SLOT` — Host slot number (1–3); can also be positional argument
- `--cache auto|off|refresh` — Cache mode (default `auto`)
- `--cache-file PATH` — Override cache file location (default `/tmp/lunaar-device-cache-<uid>`)

### Performance Tips

1. **First run (auto-discovery):**
   ```sh
    ./bin/lunaar-switch 2
   ```
    This discovers your device and saves path/devnum/feature index in the native cache.

2. **Subsequent runs (fast cached path):**
    ```sh
    ./bin/lunaar-switch 2
    ```
    By default, the binary attempts the cached path first and falls back to discovery when stale.

3. **Disable cache when debugging:**
   ```sh
    ./bin/lunaar-switch --cache off 2
   ```

4. **Force refresh cache:**
    ```sh
    ./bin/lunaar-switch --cache refresh 2
    ```

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

For seamless integration with Karabiner Elements, call the binary directly with native cache support. The app cache is stored in `/tmp` and is automatically refreshed if stale.

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
            "to": [{ "shell_command": "~/Developer/GitHub/Lunaar/bin/lunaar-switch 2" }],
            "type": "basic"
        }
    ]
}
```


Note: If command execution fails, add /Library/Application Support/org.pqrs/Karabiner-Elements/bin/karabiner_console_user_server to the Input Monitoring list in System Preferences > Privacy & Security > Input Monitoring.