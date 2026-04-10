/* ============================================================================
 * LUNAAR: Logitech Multi-Host Device Switcher
 * 
 * This program allows you to programmatically switch a Logitech device
 * (like a keyboard or mouse) between different host computers.
 * 
 * Logitech devices that support multiple hosts use the HID++ protocol
 * to communicate. This program uses that protocol to tell the device
 * which host to connect to (slot 1, 2, or 3).
 * ============================================================================ */

/* Standard C library headers */
#include <errno.h>           /* For error handling during parsing */
#include <hidapi.h>          /* HID API for USB device communication */
#include <stdint.h>          /* For fixed-size integer types (uint8_t, etc) */
#include <stdio.h>           /* For printf, fprintf, file I/O */
#include <stdlib.h>          /* For malloc, free, general utilities */
#include <string.h>          /* For string operations (strcpy, memcpy, etc) */
#include <sys/stat.h>        /* For file permission operations (fchmod) */
#include <unistd.h>          /* For POSIX operations (unlink, rename, etc) */

/* ============================================================================
 * CONSTANTS - Define fixed values used throughout the program
 * ============================================================================ */

#define LOGITECH_VID 0x046D       /* Vendor ID for all Logitech devices */
#define REPORT_ID_LONG 0x11       /* Protocol ID for "long" HID++ reports (20 bytes) */
#define MAX_PAYLOAD 18            /* Maximum payload size (18 bytes) in a message */
#define DEFAULT_TIMEOUT_MS 4000   /* Default timeout (4 seconds) for waiting for responses */
#define FAST_TIMEOUT_MS 500       /* Quick timeout (0.5 seconds) when we don't expect a reply */
#define FEATURE_CHANGE_HOST 0x1814   /* Device feature ID for changing host */
#define FEATURE_FEATURE_SET 0x0001   /* Device feature ID for querying available features */

/* ============================================================================
 * ENUM - Define cache operating modes
 * ============================================================================ */

enum cache_mode {
    CACHE_MODE_AUTO = 0,    /* Try to use cached device info, fall back to discovery */
    CACHE_MODE_OFF,         /* Never use cache, always do full device discovery */
    CACHE_MODE_REFRESH      /* Ignore cache and force rediscovery, then update cache */
};

/* ============================================================================
 * HELPER FUNCTION: Parse command-line integer arguments safely
 * 
 * This function converts a string to an integer with validation:
 * - Ensures the entire string is consumed (no trailing garbage)
 * - Bounds-checks that value is between min_value and max_value
 * - Returns the parsed value through a pointer to avoid conflicts
 *
 * Returns: 0 on success, -1 on any error
 * ============================================================================ */
static int parse_long_strict(const char *text, int base, long min_value, long max_value, long *out) {
    char *end = NULL;      /* Pointer to where strtol stopped parsing */
    long value;

    /* Check for null pointers */
    if (!text || !out) {
        return -1;
    }

    /* strtol can't tell the difference between "0" (success) and
       "0 invalid input" without clearing errno first */
    errno = 0;
    value = strtol(text, &end, base);
    
    /* Check for parsing errors: errno set, nothing was parsed, or extra chars remain */
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }
    
    /* Validate that parsed value is within acceptable range */
    if (value < min_value || value > max_value) {
        return -1;
    }
    
    /* Return successful result through output pointer */
    *out = value;
    return 0;
}

/* ============================================================================
 * CACHE: Build default cache file path
 * 
 * The cache stores device information (path, device number, feature index)
 * so we don't have to re-discover it every time. This creates a path like:
 * /tmp/lunaar-device-cache-1000 (where 1000 is the user's UID)
 * 
 * This makes the cache per-user so different users don't interfere.
 *
 * Returns: 0 on success, -1 on error
 * ============================================================================ */
static int get_default_cache_path(char *out, size_t out_cap) {
    int n;
    if (!out || out_cap == 0) {
        return -1;
    }
    
    /* Create path: /tmp/lunaar-device-cache-<user-id>
       snprintf returns the number of chars written (or -1 on error) */
    n = snprintf(out, out_cap, "/tmp/lunaar-device-cache-%u", (unsigned)getuid());
    
    /* Check for errors or buffer overflow */
    if (n < 0 || (size_t)n >= out_cap) {
        return -1;
    }
    return 0;
}

/* ============================================================================
 * CACHE: Load cached device information from file
 * 
 * The cache file contains lines like:
 *   version=1
 *   path=/dev/hidraw0
 *   devnum=1
 *   feature_index=3
 *
 * This function parses that format and returns the cached values.
 * Returns 0 on success, -1 if file doesn't exist or parsing fails
 * ============================================================================ */
static int load_cache(const char *cache_path,
                      char *path_out,
                      size_t path_out_cap,
                      uint8_t *devnum_out,
                      uint8_t *feature_index_out) {
    FILE *fp;
    char line[1024];
    char cached_path[768] = {0};      /* The device path from cache */
    long cached_devnum = -1;          /* Device number (-1 = not found yet) */
    long cached_feature_index = -1;   /* Feature index (-1 = not found yet) */

    /* Validate arguments */
    if (!cache_path || !path_out || path_out_cap == 0 || !devnum_out || !feature_index_out) {
        return -1;
    }

    /* Open and read cache file. Return -1 if it doesn't exist. */
    fp = fopen(cache_path, "r");
    if (!fp) {
        return -1;
    }

    /* Parse each line in the cache file */
    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');   /* Find the '=' character */
        char *key;
        char *value;
        size_t value_len;

        /* Skip lines without '=' */
        if (!eq) {
            continue;
        }
        
        /* Split line into key and value */
        *eq = '\0';
        key = line;
        value = eq + 1;

        /* Remove trailing newline/carriage return from value */
        value_len = strcspn(value, "\r\n");
        value[value_len] = '\0';

        /* Parse each known cache field */
        if (strcmp(key, "path") == 0) {
            /* Validate length and store device path */
            if (value_len == 0 || value_len >= sizeof(cached_path)) {
                fclose(fp);
                return -1;
            }
            memcpy(cached_path, value, value_len + 1);
        } else if (strcmp(key, "devnum") == 0) {
            /* Parse device number (0-255) */
            long parsed = 0;
            if (parse_long_strict(value, 10, 0, 255, &parsed) != 0) {
                fclose(fp);
                return -1;
            }
            cached_devnum = parsed;
        } else if (strcmp(key, "feature_index") == 0) {
            /* Parse feature index (0-255) */
            long parsed = 0;
            if (parse_long_strict(value, 10, 0, 255, &parsed) != 0) {
                fclose(fp);
                return -1;
            }
            cached_feature_index = parsed;
        }
    }

    fclose(fp);

    /* Make sure all required fields were found in cache */
    if (cached_path[0] == '\0' || cached_devnum < 0 || cached_feature_index < 0) {
        return -1;
    }
    
    /* Make sure output buffer is large enough for cached path */
    if (strlen(cached_path) >= path_out_cap) {
        return -1;
    }

    /* Copy cached values to output parameters */
    strcpy(path_out, cached_path);
    *devnum_out = (uint8_t)cached_devnum;
    *feature_index_out = (uint8_t)cached_feature_index;
    return 0;
}

/* ============================================================================
 * CACHE: Save device information to cache file
 * 
 * After successfully switching hosts, we save the device details so the next
 * run doesn't need to rediscover the device. Uses atomic write (temp file + rename)
 * to prevent corruption if the program crashes mid-write.
 *
 * Returns: 0 on success, -1 on error
 * ============================================================================ */
static int save_cache(const char *cache_path, const char *device_path, uint8_t devnum, uint8_t feature_index) {
    char tmp_path[1024];   /* Temporary file path */
    FILE *fp;
    int n;

    /* Validate arguments */
    if (!cache_path || !device_path || device_path[0] == '\0') {
        return -1;
    }

    /* Create a temporary filename with the current process ID
       This ensures multiple instances don't collide */
    n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", cache_path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        return -1;
    }

    /* Create and write to temporary file */
    fp = fopen(tmp_path, "w");
    if (!fp) {
        return -1;
    }

    /* Set file permissions to 0600 (readable/writable by owner only)
       This keeps the cache file private to the user */
    (void)fchmod(fileno(fp), 0600);

    /* Write cache file format */
    if (fprintf(fp,
                "version=1\n"
                "path=%s\n"
                "devnum=%u\n"
                "feature_index=%u\n",
                device_path,
                (unsigned)devnum,
                (unsigned)feature_index) < 0) {
        fclose(fp);
        unlink(tmp_path);  /* Clean up temp file on error */
        return -1;
    }

    /* Close file and check for errors */
    if (fclose(fp) != 0) {
        unlink(tmp_path);
        return -1;
    }

    /* Atomically replace old cache with new one
       This is atomic on most filesystems, so the cache never gets corrupted */
    if (rename(tmp_path, cache_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

/* ============================================================================
 * CACHE: Delete cache file (mark as invalid)
 * 
 * When we can't use cached info (device not found at cached path, etc),
 * we delete the cache file. Next run will force rediscovery.
 * ============================================================================ */
static void invalidate_cache(const char *cache_path) {
    if (!cache_path || cache_path[0] == '\0') {
        return;
    }
    /* Delete the cache file; ignore errors if it doesn't exist */
    (void)unlink(cache_path);
}

/* ============================================================================
 * HID++ PROTOCOL: Generate next software ID
 * 
 * The HID++ protocol uses a rolling "software ID" (sw_id) in the lower 4 bits
 * of the request ID. This ID cycles through 0x02-0x0F, helping devices match
 * requests with responses.
 * 
 * This function maintains a static counter and returns the next ID in sequence.
 * ============================================================================ */
static uint16_t next_sw_id(void) {
    static uint8_t sw = 0x0F;  /* Start at 0x0F so first call returns 0x02 */
    
    /* Cycle through 0x02 to 0x0F, then wrap back to 0x02 */
    sw = (sw < 0x0F) ? (uint8_t)(sw + 1) : 0x02;
    return sw;
}

/* ============================================================================
 * HID++ PROTOCOL: Send a "long" HID++ message to the device
 * 
 * HID++ messages are formatted as:
 *   [0] Report ID (0x11 for "long" format)
 *   [1] Device number (which device on the receiver)
 *   [2:19] Payload (18 bytes of data)
 *
 * This function wraps the payload in this format and sends it.
 * Returns: 0 on success, -1 on error
 * ============================================================================ */
static int write_long(hid_device *dev, uint8_t devnum, const uint8_t *payload, size_t payload_len) {
    uint8_t buf[2 + MAX_PAYLOAD] = {0};  /* Report ID + devnum + payload */
    
    buf[0] = REPORT_ID_LONG;  /* Mark as long format (0x11) */
    buf[1] = devnum;          /* Device number (which Logitech device on the receiver) */
    
    /* Copy the payload into the message */
    memcpy(buf + 2, payload, payload_len);
    
    /* Send the entire message via HID; hid_write returns bytes written */
    return (hid_write(dev, buf, sizeof(buf)) == (int)sizeof(buf)) ? 0 : -1;
}

/* ============================================================================
 * HID++ PROTOCOL: Read and wait for a matching response from device
 * 
 * After sending a request, we need to read responses until we find the one
 * that matches our request. Devices might send unsolicited events (like battery
 * level notifications), so we ignore those and keep reading until we find
 * a response with matching device number and request ID.
 *
 * Parameters:
 *   dev: HID device handle
 *   expected_devnum: Device number we expect the reply from
 *   expected_req: Request ID we're waiting for
 *   out: Buffer to copy response data into
 *   out_cap: Size of output buffer
 *   timeout_ms: How long to wait (milliseconds)
 *
 * Returns: Number of bytes in response data on success, -1 on timeout
 * ============================================================================ */
static int read_matching(hid_device *dev,
                         uint8_t expected_devnum,
                         uint16_t expected_req,
                         uint8_t *out,
                         size_t out_cap,
                         int timeout_ms) {
    const int slice_ms = 200;          /* Read in 200ms chunks to be responsive */
    int elapsed = 0;                   /* Track how long we've been waiting */
    uint8_t buf[32];                   /* Buffer for incoming HID messages */

    /* Keep reading until timeout */
    while (elapsed < timeout_ms) {
        /* Calculate timeout for this read (remaining time, but max 200ms) */
        int this_timeout = (timeout_ms - elapsed) < slice_ms ? (timeout_ms - elapsed) : slice_ms;
        
        /* Try to read a message from the device */
        int n = hid_read_timeout(dev, buf, sizeof(buf), this_timeout);
        elapsed += this_timeout;
        
        /* No data available, keep trying */
        if (n <= 0) {
            continue;
        }
        
        /* Check if this is a valid HID++ message
           Valid report IDs: 0x10, 0x11 (short), 0x20, 0x21 (long) */
        if (buf[0] != 0x10 && buf[0] != 0x11 && buf[0] != 0x21 && buf[0] != 0x20) {
            continue;  /* Not HID++, ignore */
        }
        
        /* Extract device number from message */
        uint8_t devnum = buf[1];
        
        /* Check if this is from our device
           Note: Device number can be inverted in some response types (0xFF XOR) */
        if (!(devnum == expected_devnum || devnum == (uint8_t)(expected_devnum ^ 0xFF))) {
            continue;  /* Message from different device, ignore */
        }
        
        /* Extract response data (everything after report ID and devnum) */
        size_t data_len = (size_t)(n - 2);
        if (data_len < 2) {
            continue;  /* Response too short */
        }
        
        /* Extract request ID from response (first 2 bytes of data) */
        uint16_t resp_req = ((uint16_t)buf[2] << 8) | buf[3];
        
        /* Check if this response matches our request */
        if (resp_req == expected_req) {
            /* Found it! Copy the response data (excluding the request ID) to output */
            size_t copy_len = data_len - 2;  /* Subtract the request ID bytes */
            if (out && out_cap > 0) {
                /* Don't copy more than the output buffer can hold */
                if (copy_len > out_cap) {
                    copy_len = out_cap;
                }
                memcpy(out, buf + 4, copy_len);  /* buf+4: skip report ID, devnum, request ID */
                return (int)copy_len;
            }
            return (int)copy_len;
        }
        
        /* This response doesn't match, keep looking */
    }
    
    /* Timeout: didn't find matching response */
    return -1;
}

/* ============================================================================
 * HID++ PROTOCOL: Send a complete HID++ request and optionally wait for response
 * 
 * This is the main wrapper function for the HID++ protocol. It:
 * 1. Assigns a software ID to match requests with responses
 * 2. Builds the request message with your parameters
 * 3. Sends it to the device
 * 4. Optionally waits for the response
 *
 * Returns: Number of response bytes on success (if expecting reply),
 *          0 on success (if not expecting reply), -1 on error
 * ============================================================================ */
static int hidpp_request(hid_device *dev,
                         uint8_t devnum,
                         uint16_t request_id,
                         const uint8_t *params,
                         size_t params_len,
                         uint8_t *out,
                         size_t out_cap,
                         int expect_reply,
                         int timeout_ms) {
    uint16_t sw_id = 0;
    
    /* If this is a device-specific request (not broadcast), add software ID
       The software ID is in the lower 4 bits and helps match responses to requests */
    if (devnum != 0xFF && (request_id & 0x8000) == 0) {
        sw_id = next_sw_id();
        request_id = (uint16_t)((request_id & 0xFFF0) | (sw_id & 0x0F));
    }

    /* Build the message: [request_id_high_byte, request_id_low_byte, ...params...] */
    uint8_t payload[MAX_PAYLOAD] = {0};
    
    /* Check that params fit in the message */
    if (params_len > (MAX_PAYLOAD - 2)) {
        return -1;
    }
    
    /* Add request ID to payload (big-endian: high byte first) */
    payload[0] = (uint8_t)(request_id >> 8);
    payload[1] = (uint8_t)(request_id & 0xFF);
    
    /* Add parameters after the request ID */
    if (params_len) {
        memcpy(payload + 2, params, params_len);
    }

    /* Send the message to the device */
    if (write_long(dev, devnum, payload, sizeof(payload)) != 0) {
        return -1;
    }
    
    /* If we don't expect a reply, we're done */
    if (!expect_reply) {
        return 0;
    }
    
    /* Wait for a matching response from the device */
    return read_matching(dev, devnum, request_id, out, out_cap, timeout_ms);
}

/* ============================================================================
 * DEVICE DISCOVERY: Get the index of the "feature set" feature
 * 
 * The feature set feature (0x0001) is special: it lets us query which other
 * features the device supports. We need to know its index first.
 * 
 * We do this by calling function 0 (feature discovery) with feature ID 0x0001.
 * The response tells us which index implements feature set on this device.
 * ============================================================================ */
static int get_feature_set_index(hid_device *dev, uint8_t devnum, uint8_t *index_out) {
    /* Call function 0 (GetFeature) with feature ID 0x0001 (feature set) */
    uint8_t params[2] = {(uint8_t)(FEATURE_FEATURE_SET >> 8), (uint8_t)(FEATURE_FEATURE_SET & 0xFF)};
    uint8_t resp[8] = {0};  /* Response buffer */
    
    /* Send request and wait for response */
    int n = hidpp_request(dev, devnum, 0x0000, params, sizeof(params), resp, sizeof(resp), 1, DEFAULT_TIMEOUT_MS);
    
    /* Response should have at least 1 byte (the index) */
    if (n < 1) {
        return -1;
    }
    
    /* First byte of response is the feature index */
    *index_out = resp[0];
    return 0;
}

/* ============================================================================
 * DEVICE DISCOVERY: Get the index of a specific feature by its ID
 * 
 * Most device functions are accessed by "feature index" which is device-specific.
 * For example, "CHANGE_HOST" (0x1814) might be at index 3 on one device, but
 * index 5 on another. We use the feature set to look it up.
 * 
 * Returns the index where this feature lives on this device, or -1 if not found.
 * ============================================================================ */
static int get_feature_index(hid_device *dev, uint8_t devnum, uint16_t feature_id, uint8_t *index_out) {
    /* Call function 0 (GetFeature) with the feature ID we're looking for */
    uint8_t params[2] = {(uint8_t)(feature_id >> 8), (uint8_t)(feature_id & 0xFF)};
    uint8_t resp[8] = {0};  /* Response buffer */
    
    /* Send request and wait for response */
    int n = hidpp_request(dev, devnum, 0x0000, params, sizeof(params), resp, sizeof(resp), 1, DEFAULT_TIMEOUT_MS);
    
    /* If response is too short or index is 0 (not found), return error */
    if (n < 1 || resp[0] == 0) {
        return -1;
    }
    
    /* First byte of response is the feature index */
    *index_out = resp[0];
    return 0;
}

/* ============================================================================
 * MAIN ACTION: Tell the device to switch to a specific host slot
 * 
 * The device supports up to 3 host connections. This function tells the device
 * to switch input/output to one of those slots (0, 1, or 2).
 * 
 * Note: We use a fast timeout here because we don't expect a reply.
 * ============================================================================ */
static int switch_host(hid_device *dev, uint8_t devnum, uint8_t feature_index, uint8_t host_slot) {
    /* Build the request ID: [feature_index | 0x10]
       0x10 is the "SetActiveHost" function code for the CHANGE_HOST feature */
    uint16_t request_id = (uint16_t)((feature_index << 8) | 0x10);
    
    /* Parameter: which host slot to switch to (0, 1, or 2) */
    uint8_t params[1] = {host_slot};
    
    /* Send the request without expecting a reply (fast timeout OK) */
    return hidpp_request(dev, devnum, request_id, params, sizeof(params), NULL, 0, 0, FAST_TIMEOUT_MS);
}

/* ============================================================================
 * DEVICE DISCOVERY: Find the first Logitech device with CHANGE_HOST support
 * 
 * This function:
 * 1. Enumerates all Logitech USB devices (VID 0x046D)
 * 2. Opens each one and checks which device numbers (0-7) are present
 * 3. Queries each device to see if it has the CHANGE_HOST feature
 * 4. Returns the first device that supports host switching
 * 
 * Returns: HID device handle if found, NULL if not found
 * ============================================================================ */
static hid_device *open_first_device(uint8_t *devnum_out, uint8_t *change_host_index_out, char **path_out) {
    /* Get list of all Logitech USB devices */
    struct hid_device_info *devs = hid_enumerate(LOGITECH_VID, 0x0);
    struct hid_device_info *cur = devs;
    hid_device *found = NULL;

    /* Try each USB device */
    for (; cur; cur = cur->next) {
        /* Open this USB device */
        hid_device *handle = hid_open_path(cur->path);
        if (!handle) {
            continue;  /* Can't open, try next USB device */
        }
        
        /* Each USB device can have up to 8 logical devices (devnum 0-7)
           These are often different functions like keyboard, consumer keys, etc. */
        for (uint8_t dn = 0; dn <= 7; dn++) {
            uint8_t fs_index = 0;
            
            /* Check if this device number exists and has the feature set */
            if (get_feature_set_index(handle, dn, &fs_index) != 0 || fs_index == 0) {
                continue;  /* Device not present at this number */
            }
            
            uint8_t ch_index = 0;
            
            /* Check if this device supports CHANGE_HOST feature */
            if (get_feature_index(handle, dn, FEATURE_CHANGE_HOST, &ch_index) != 0) {
                continue;  /* Device doesn't support host switching */
            }
            
            /* Found it! Save the details and exit */
            *devnum_out = dn;
            *change_host_index_out = ch_index;
            found = handle;
            if (path_out) {
                *path_out = strdup(cur->path);  /* Save the device path for later */
            }
            goto done;  /* Two loops, so we use goto to break out of both */
        }
        
        /* This USB device doesn't support host switching, close it */
        hid_close(handle);
    }

done:
    /* Clean up the device list (we already opened the device, so copy the handle out first) */
    hid_free_enumeration(devs);
    return found;
}

/* ============================================================================
 * DEVICE DISCOVERY: Open device at specific USB path and find host switch feature
 * 
 * If the user specifies a device path (--path), we open that specific device
 * and search through its device numbers to find the CHANGE_HOST feature.
 * 
 * Returns: HID device handle if found, NULL otherwise
 * ============================================================================ */
static hid_device *open_device_by_path(const char *dev_path, uint8_t *devnum_out, uint8_t *change_host_index_out) {
    /* Open the specific device */
    hid_device *handle = hid_open_path(dev_path);
    if (!handle) {
        return NULL;  /* Can't open the device */
    }
    
    /* Search through all device numbers (0-7) on this USB device */
    for (uint8_t dn = 0; dn <= 7; dn++) {
        uint8_t fs_index = 0;
        
        /* Check if this device number exists and has the feature set */
        if (get_feature_set_index(handle, dn, &fs_index) != 0 || fs_index == 0) {
            continue;  /* Device not present at this number */
        }
        
        uint8_t ch_index = 0;
        
        /* Check if this device supports CHANGE_HOST feature */
        if (get_feature_index(handle, dn, FEATURE_CHANGE_HOST, &ch_index) != 0) {
            continue;  /* Device doesn't support host switching */
        }
        
        /* Found it! */
        *devnum_out = dn;
        *change_host_index_out = ch_index;
        return handle;
    }
    
    /* Device at this path doesn't support host switching */
    hid_close(handle);
    return NULL;
}

/* ============================================================================
 * HELP: Print usage message when argument parsing fails
 * ============================================================================ */
static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [-s] [--path PATH] [--devnum DEVNUM] [--feature-index INDEX] [--slot SLOT] "
            "[--cache auto|off|refresh] [--cache-file PATH] <host-number-1-3>\n",
            prog);
}

/* ============================================================================
 * MAIN: Entry point for the host switching program
 * 
 * Overview of the flow:
 * 1. Parse command-line arguments (host slot, cache mode, device path, etc)
 * 2. Try to use cached device info if available (fast path)
 * 3. If cache doesn't work, discover the device
 * 4. Call switch_host to actually switch to the selected host
 * 5. Save device info to cache for next time
 * ============================================================================ */
int main(int argc, char **argv) {
    /* Command-line options (all optional except host number) */
    const char *device_path = NULL;              /* --path: specific USB device path */
    const char *cache_file_override = NULL;      /* --cache-file: use non-default cache file */
    int devnum_override = -1;                    /* --devnum: specific device number */
    int feature_index_override = 14;             /* --feature-index: specific feature index */
    int feature_index_set = 0;                   /* Was --feature-index explicitly provided? */
    int cache_mode = CACHE_MODE_AUTO;            /* --cache: auto/off/refresh */
    int silent = 0;                              /* -s: silent mode (no output) */
    long host = -1;                              /* Required: which host slot (1-3) */
    int host_arg_idx = 1;                        /* Current position while parsing args */
    char cache_path[512] = {0};                  /* Full path to cache file */

    /* ====================================================================
     * PHASE 1: Parse command-line arguments
     * ==================================================================== */
    while (host_arg_idx < argc) {
        if (strcmp(argv[host_arg_idx], "-s") == 0) {
            /* Silent mode: suppress output */
            silent = 1;
            host_arg_idx++;
        } else if (strcmp(argv[host_arg_idx], "--path") == 0 && host_arg_idx + 1 < argc) {
            /* Specific device path */
            device_path = argv[host_arg_idx + 1];
            host_arg_idx += 2;
        } else if (strcmp(argv[host_arg_idx], "--devnum") == 0 && host_arg_idx + 1 < argc) {
            /* Specific device number (0-255) */
            long parsed = 0;
            if (parse_long_strict(argv[host_arg_idx + 1], 0, 0, 255, &parsed) != 0) {
                usage(argv[0]);
                return 1;
            }
            devnum_override = (int)parsed;
            host_arg_idx += 2;
        } else if (strcmp(argv[host_arg_idx], "--feature-index") == 0 && host_arg_idx + 1 < argc) {
            /* Specific feature index (0-255) */
            long parsed = 0;
            if (parse_long_strict(argv[host_arg_idx + 1], 0, 0, 255, &parsed) != 0) {
                usage(argv[0]);
                return 1;
            }
            feature_index_override = (int)parsed;
            feature_index_set = 1;
            host_arg_idx += 2;
        } else if (strcmp(argv[host_arg_idx], "--slot") == 0 && host_arg_idx + 1 < argc) {
            /* Which host slot to switch to (1-3) */
            if (parse_long_strict(argv[host_arg_idx + 1], 10, 1, 3, &host) != 0) {
                usage(argv[0]);
                return 1;
            }
            host_arg_idx += 2;
        } else if (strcmp(argv[host_arg_idx], "--cache") == 0 && host_arg_idx + 1 < argc) {
            /* Cache mode: auto (use if possible), off (never use), refresh (rebuild) */
            const char *mode = argv[host_arg_idx + 1];
            if (strcmp(mode, "auto") == 0) {
                cache_mode = CACHE_MODE_AUTO;
            } else if (strcmp(mode, "off") == 0) {
                cache_mode = CACHE_MODE_OFF;
            } else if (strcmp(mode, "refresh") == 0) {
                cache_mode = CACHE_MODE_REFRESH;
            } else {
                usage(argv[0]);
                return 1;
            }
            host_arg_idx += 2;
        } else if (strcmp(argv[host_arg_idx], "--cache-file") == 0 && host_arg_idx + 1 < argc) {
            /* Use custom cache file path instead of default */
            cache_file_override = argv[host_arg_idx + 1];
            host_arg_idx += 2;
        } else {
            /* Unknown option or positional argument */
            break;
        }
    }

    /* If host slot wasn't set by --slot, try to parse positional argument */
    if (host < 0) {
        if (host_arg_idx >= argc) {
            usage(argv[0]);
            return 1;
        }
        if (parse_long_strict(argv[host_arg_idx], 10, 1, 3, &host) != 0) {
            usage(argv[0]);
            return 1;
        }
        host_arg_idx++;
    }

    /* Validate host slot is 1, 2, or 3 */
    if (host < 1 || host > 3) {
        usage(argv[0]);
        return 1;
    }
    
    /* Convert from 1-based user input to 0-based device slot (1->0, 2->1, 3->2) */
    uint8_t host_slot = (uint8_t)(host - 1);

    /* ====================================================================
     * PHASE 2: Initialize HID library and set up cache path
     * ==================================================================== */
    
    /* Initialize the HID library */
    if (hid_init() != 0) {
        fprintf(stderr, "hidapi init failed\n");
        return 1;
    }

    /* Determine cache file path */
    if (cache_file_override) {
        /* User provided a specific cache file path */
        if (snprintf(cache_path, sizeof(cache_path), "%s", cache_file_override) >= (int)sizeof(cache_path)) {
            fprintf(stderr, "Cache file path is too long\n");
            hid_exit();
            return 1;
        }
    } else if (get_default_cache_path(cache_path, sizeof(cache_path)) != 0) {
        /* Failed to build default path (/tmp/lunaar-device-cache-<uid>) */
        fprintf(stderr, "Failed to build default cache path\n");
        hid_exit();
        return 1;
    }

    /* ====================================================================
     * PHASE 3: Device discovery/opening
     * ==================================================================== */
    
    uint8_t devnum = 0;
    uint8_t ch_index = 0;
    char *path = NULL;
    hid_device *dev = NULL;
    
    /* Check if user provided enough info to skip discovery
       (specific path + device number = no need to probe) */
    int has_explicit_fast_path = (device_path && devnum_override >= 0);

    /* Try to use cache if in AUTO mode and no explicit path was given */
    if (cache_mode == CACHE_MODE_AUTO && !has_explicit_fast_path && !device_path && 
        devnum_override < 0 && !feature_index_set) {
        char cached_path[768] = {0};
        uint8_t cached_devnum = 0;
        uint8_t cached_feature_index = 0;

        /* Load cached device info */
        if (load_cache(cache_path, cached_path, sizeof(cached_path), &cached_devnum, &cached_feature_index) == 0) {
            /* Try to use cached info */
            hid_device *cached_dev = hid_open_path(cached_path);
            if (cached_dev) {
                int cached_rc = switch_host(cached_dev, cached_devnum, cached_feature_index, host_slot);
                hid_close(cached_dev);
                if (cached_rc == 0) {
                    /* Success! */
                    if (!silent) {
                        printf("Switched host to slot %ld (device %u, feature index %u) via %s\n",
                               host,
                               (unsigned)cached_devnum,
                               (unsigned)cached_feature_index,
                               cached_path);
                    }
                    hid_exit();
                    return 0;
                }
            }
            /* Cache didn't work, invalidate it for next time */
            invalidate_cache(cache_path);
        }
    }

    /* If REFRESH mode, clear cache before discovery */
    if (cache_mode == CACHE_MODE_REFRESH) {
        invalidate_cache(cache_path);
    }

    /* Now actually open the device */
    
    if (device_path && devnum_override >= 0) {
        /* FAST PATH: User provided path + devnum, open directly without discovery */
        dev = hid_open_path(device_path);
        if (dev) {
            devnum = (uint8_t)devnum_override;
            ch_index = (uint8_t)feature_index_override;
            /* Skip saving path if in silent mode to avoid unnecessary allocation */
            if (!silent) {
                path = strdup(device_path);
            }
        }
    } else if (device_path) {
        /* User provided path but not device number, need feature discovery */
        dev = open_device_by_path(device_path, &devnum, &ch_index);
        if (!dev) {
            fprintf(stderr, "Failed to open device at path: %s\n", device_path);
            fprintf(stderr, "Use auto-discovery or provide --devnum and --feature-index to skip discovery\n");
            hid_exit();
            return 1;
        }
        if (!silent) {
            path = strdup(device_path);
        }
    } else {
        /* DISCOVERY: Auto-find first compatible device */
        dev = open_first_device(&devnum, &ch_index, &path);
    }
    
    if (!dev) {
        fprintf(stderr, "No Logitech HID++ device with CHANGE_HOST found\n");
        hid_exit();
        return 1;
    }

    /* ====================================================================
     * PHASE 4: Actually switch the host
     * ==================================================================== */
    
    int rc = switch_host(dev, devnum, ch_index, host_slot);
    if (rc == 0 && !silent) {
        /* Success! Print result */
        printf("Switched host to slot %ld (device %u, feature index %u)%s%s\n",
               host,
               devnum,
               ch_index,
               path ? " via " : "",
               path ? path : "");
    } else if (rc != 0) {
        /* Failed to switch */
        fprintf(stderr, "Failed to switch host\n");
    }

    /* ====================================================================
     * PHASE 5: Save device info to cache (if successful and not disabled)
     * ==================================================================== */
    
    if (rc == 0 && cache_mode != CACHE_MODE_OFF) {
        const char *path_for_cache = device_path ? device_path : path;
        if (path_for_cache && path_for_cache[0] != '\0') {
            (void)save_cache(cache_path, path_for_cache, devnum, ch_index);
        }
    }

    /* ====================================================================
     * CLEANUP
     * ==================================================================== */
    
    free(path);
    hid_close(dev);
    hid_exit();
    
    /* Return 0 on success, 1 on failure */
    return rc == 0 ? 0 : 1;
}
