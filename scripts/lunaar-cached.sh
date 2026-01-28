#!/bin/bash
# Wrapper script for lunaar-switch that caches device info until reboot
# The cache is stored in /tmp which is cleared on macOS reboots

set -e

CACHE_FILE="/tmp/lunaar-device-cache-$USER"
LUNAAR_BIN="${LUNAAR_BIN:-$(dirname "$0")/../bin/lunaar-switch}"

# Check if we have a valid cache file
if [ -f "$CACHE_FILE" ]; then
    # Read cached values
    source "$CACHE_FILE"
    
    # Use cached values with fast path
    if [ -n "$LUNAAR_PATH" ] && [ -n "$LUNAAR_DEVNUM" ] && [ -n "$LUNAAR_FEATURE_INDEX" ]; then
        exec "$LUNAAR_BIN" --path "$LUNAAR_PATH" --devnum "$LUNAAR_DEVNUM" --feature-index "$LUNAAR_FEATURE_INDEX" "$@"
    fi
fi

# No cache or invalid cache - do auto-discovery and cache the result
# Run with verbose output to capture the device info
OUTPUT=$("$LUNAAR_BIN" "$@" 2>&1)
EXIT_CODE=$?

# If successful, parse and cache the device info
if [ $EXIT_CODE -eq 0 ]; then
    # Extract device info from output like:
    # "Switched host to slot 1 (device 1, feature index 14) via /dev/hidraw0"
    if echo "$OUTPUT" | grep -q "via "; then
        DEVICE_PATH=$(echo "$OUTPUT" | sed -n 's/.*via \(.*\)$/\1/p')
        DEVICE_NUM=$(echo "$OUTPUT" | sed -n 's/.*device \([0-9]*\),.*/\1/p')
        FEATURE_IDX=$(echo "$OUTPUT" | sed -n 's/.*feature index \([0-9]*\).*/\1/p')
        
        if [ -n "$DEVICE_PATH" ] && [ -n "$DEVICE_NUM" ] && [ -n "$FEATURE_IDX" ]; then
            # Cache the values
            cat > "$CACHE_FILE" <<EOF
LUNAAR_PATH="$DEVICE_PATH"
LUNAAR_DEVNUM="$DEVICE_NUM"
LUNAAR_FEATURE_INDEX="$FEATURE_IDX"
EOF
            chmod 600 "$CACHE_FILE"
        fi
    fi
    
    # Print the original output
    echo "$OUTPUT"
fi

exit $EXIT_CODE
