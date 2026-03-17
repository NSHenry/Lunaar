#include <errno.h>
#include <hidapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOGITECH_VID 0x046D
#define REPORT_ID_LONG 0x11
#define MAX_PAYLOAD 18
#define DEFAULT_TIMEOUT_MS 4000
#define FAST_TIMEOUT_MS 500
#define FEATURE_CHANGE_HOST 0x1814
#define FEATURE_FEATURE_SET 0x0001

enum cache_mode {
    CACHE_MODE_AUTO = 0,
    CACHE_MODE_OFF,
    CACHE_MODE_REFRESH
};

static int parse_long_strict(const char *text, int base, long min_value, long max_value, long *out) {
    char *end = NULL;
    long value;

    if (!text || !out) {
        return -1;
    }

    errno = 0;
    value = strtol(text, &end, base);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }
    if (value < min_value || value > max_value) {
        return -1;
    }
    *out = value;
    return 0;
}

static int get_default_cache_path(char *out, size_t out_cap) {
    int n;
    if (!out || out_cap == 0) {
        return -1;
    }
    n = snprintf(out, out_cap, "/tmp/lunaar-device-cache-%u", (unsigned)getuid());
    if (n < 0 || (size_t)n >= out_cap) {
        return -1;
    }
    return 0;
}

static int load_cache(const char *cache_path,
                      char *path_out,
                      size_t path_out_cap,
                      uint8_t *devnum_out,
                      uint8_t *feature_index_out) {
    FILE *fp;
    char line[1024];
    char cached_path[768] = {0};
    long cached_devnum = -1;
    long cached_feature_index = -1;

    if (!cache_path || !path_out || path_out_cap == 0 || !devnum_out || !feature_index_out) {
        return -1;
    }

    fp = fopen(cache_path, "r");
    if (!fp) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        char *key;
        char *value;
        size_t value_len;

        if (!eq) {
            continue;
        }
        *eq = '\0';
        key = line;
        value = eq + 1;

        value_len = strcspn(value, "\r\n");
        value[value_len] = '\0';

        if (strcmp(key, "path") == 0) {
            if (value_len == 0 || value_len >= sizeof(cached_path)) {
                fclose(fp);
                return -1;
            }
            memcpy(cached_path, value, value_len + 1);
        } else if (strcmp(key, "devnum") == 0) {
            long parsed = 0;
            if (parse_long_strict(value, 10, 0, 255, &parsed) != 0) {
                fclose(fp);
                return -1;
            }
            cached_devnum = parsed;
        } else if (strcmp(key, "feature_index") == 0) {
            long parsed = 0;
            if (parse_long_strict(value, 10, 0, 255, &parsed) != 0) {
                fclose(fp);
                return -1;
            }
            cached_feature_index = parsed;
        }
    }

    fclose(fp);

    if (cached_path[0] == '\0' || cached_devnum < 0 || cached_feature_index < 0) {
        return -1;
    }
    if (strlen(cached_path) >= path_out_cap) {
        return -1;
    }

    strcpy(path_out, cached_path);
    *devnum_out = (uint8_t)cached_devnum;
    *feature_index_out = (uint8_t)cached_feature_index;
    return 0;
}

static int save_cache(const char *cache_path, const char *device_path, uint8_t devnum, uint8_t feature_index) {
    char tmp_path[1024];
    FILE *fp;
    int n;

    if (!cache_path || !device_path || device_path[0] == '\0') {
        return -1;
    }

    n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", cache_path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        return -1;
    }

    fp = fopen(tmp_path, "w");
    if (!fp) {
        return -1;
    }

    (void)fchmod(fileno(fp), 0600);

    if (fprintf(fp,
                "version=1\n"
                "path=%s\n"
                "devnum=%u\n"
                "feature_index=%u\n",
                device_path,
                (unsigned)devnum,
                (unsigned)feature_index) < 0) {
        fclose(fp);
        unlink(tmp_path);
        return -1;
    }

    if (fclose(fp) != 0) {
        unlink(tmp_path);
        return -1;
    }

    if (rename(tmp_path, cache_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

static void invalidate_cache(const char *cache_path) {
    if (!cache_path || cache_path[0] == '\0') {
        return;
    }
    (void)unlink(cache_path);
}

static uint16_t next_sw_id(void) {
    static uint8_t sw = 0x0F;
    sw = (sw < 0x0F) ? (uint8_t)(sw + 1) : 0x02;
    return sw;
}

static int write_long(hid_device *dev, uint8_t devnum, const uint8_t *payload, size_t payload_len) {
    uint8_t buf[2 + MAX_PAYLOAD] = {0};
    buf[0] = REPORT_ID_LONG;
    buf[1] = devnum;
    memcpy(buf + 2, payload, payload_len);
    return (hid_write(dev, buf, sizeof(buf)) == (int)sizeof(buf)) ? 0 : -1;
}

static int read_matching(hid_device *dev,
                         uint8_t expected_devnum,
                         uint16_t expected_req,
                         uint8_t *out,
                         size_t out_cap,
                         int timeout_ms) {
    const int slice_ms = 200;
    int elapsed = 0;
    uint8_t buf[32];

    while (elapsed < timeout_ms) {
        int this_timeout = (timeout_ms - elapsed) < slice_ms ? (timeout_ms - elapsed) : slice_ms;
        int n = hid_read_timeout(dev, buf, sizeof(buf), this_timeout);
        elapsed += this_timeout;
        if (n <= 0) {
            continue;
        }
        if (buf[0] != 0x10 && buf[0] != 0x11 && buf[0] != 0x21 && buf[0] != 0x20) {
            continue; /* not HID++ */
        }
        uint8_t devnum = buf[1];
        if (!(devnum == expected_devnum || devnum == (uint8_t)(expected_devnum ^ 0xFF))) {
            continue; /* other device */
        }
        size_t data_len = (size_t)(n - 2);
        if (data_len < 2) {
            continue;
        }
        uint16_t resp_req = ((uint16_t)buf[2] << 8) | buf[3];
        if (resp_req == expected_req) {
            size_t copy_len = data_len - 2;
            if (out && out_cap > 0) {
                if (copy_len > out_cap) {
                    copy_len = out_cap;
                }
                memcpy(out, buf + 4, copy_len);
                return (int)copy_len;
            }
            return (int)copy_len;
        }
        /* ignore mismatched replies */
    }
    return -1; /* timeout */
}

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
    if (devnum != 0xFF && (request_id & 0x8000) == 0) {
        sw_id = next_sw_id();
        request_id = (uint16_t)((request_id & 0xFFF0) | (sw_id & 0x0F));
    }

    uint8_t payload[MAX_PAYLOAD] = {0};
    if (params_len > (MAX_PAYLOAD - 2)) {
        return -1;
    }
    payload[0] = (uint8_t)(request_id >> 8);
    payload[1] = (uint8_t)(request_id & 0xFF);
    if (params_len) {
        memcpy(payload + 2, params, params_len);
    }

    if (write_long(dev, devnum, payload, sizeof(payload)) != 0) {
        return -1;
    }
    if (!expect_reply) {
        return 0;
    }
    return read_matching(dev, devnum, request_id, out, out_cap, timeout_ms);
}

static int get_feature_set_index(hid_device *dev, uint8_t devnum, uint8_t *index_out) {
    uint8_t params[2] = {(uint8_t)(FEATURE_FEATURE_SET >> 8), (uint8_t)(FEATURE_FEATURE_SET & 0xFF)};
    uint8_t resp[8] = {0};
    int n = hidpp_request(dev, devnum, 0x0000, params, sizeof(params), resp, sizeof(resp), 1, DEFAULT_TIMEOUT_MS);
    if (n < 1) {
        return -1;
    }
    *index_out = resp[0];
    return 0;
}

static int get_feature_index(hid_device *dev, uint8_t devnum, uint16_t feature_id, uint8_t *index_out) {
    uint8_t params[2] = {(uint8_t)(feature_id >> 8), (uint8_t)(feature_id & 0xFF)};
    uint8_t resp[8] = {0};
    int n = hidpp_request(dev, devnum, 0x0000, params, sizeof(params), resp, sizeof(resp), 1, DEFAULT_TIMEOUT_MS);
    if (n < 1 || resp[0] == 0) {
        return -1;
    }
    *index_out = resp[0];
    return 0;
}

static int switch_host(hid_device *dev, uint8_t devnum, uint8_t feature_index, uint8_t host_slot) {
    uint16_t request_id = (uint16_t)((feature_index << 8) | 0x10);
    uint8_t params[1] = {host_slot};
    /* Use fast timeout since we don't expect a reply */
    return hidpp_request(dev, devnum, request_id, params, sizeof(params), NULL, 0, 0, FAST_TIMEOUT_MS);
}

static hid_device *open_first_device(uint8_t *devnum_out, uint8_t *change_host_index_out, char **path_out) {
    struct hid_device_info *devs = hid_enumerate(LOGITECH_VID, 0x0);
    struct hid_device_info *cur = devs;
    hid_device *found = NULL;

    for (; cur; cur = cur->next) {
        hid_device *handle = hid_open_path(cur->path);
        if (!handle) {
            continue;
        }
        for (uint8_t dn = 0; dn <= 7; dn++) {
            uint8_t fs_index = 0;
            if (get_feature_set_index(handle, dn, &fs_index) != 0 || fs_index == 0) {
                continue;
            }
            uint8_t ch_index = 0;
            if (get_feature_index(handle, dn, FEATURE_CHANGE_HOST, &ch_index) != 0) {
                continue;
            }
            *devnum_out = dn;
            *change_host_index_out = ch_index;
            found = handle;
            if (path_out) {
                *path_out = strdup(cur->path);
            }
            goto done;
        }
        hid_close(handle);
    }

done:
    hid_free_enumeration(devs);
    return found;
}

static hid_device *open_device_by_path(const char *dev_path, uint8_t *devnum_out, uint8_t *change_host_index_out) {
    hid_device *handle = hid_open_path(dev_path);
    if (!handle) {
        return NULL;
    }
    for (uint8_t dn = 0; dn <= 7; dn++) {
        uint8_t fs_index = 0;
        if (get_feature_set_index(handle, dn, &fs_index) != 0 || fs_index == 0) {
            continue;
        }
        uint8_t ch_index = 0;
        if (get_feature_index(handle, dn, FEATURE_CHANGE_HOST, &ch_index) != 0) {
            continue;
        }
        *devnum_out = dn;
        *change_host_index_out = ch_index;
        return handle;
    }
    hid_close(handle);
    return NULL;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [-s] [--path PATH] [--devnum DEVNUM] [--feature-index INDEX] [--slot SLOT] "
            "[--cache auto|off|refresh] [--cache-file PATH] <host-number-1-3>\n",
            prog);
}

int main(int argc, char **argv) {
    const char *device_path = NULL;
    const char *cache_file_override = NULL;
    int devnum_override = -1;
    int feature_index_override = 14;  /* Default to 14 for CHANGE_HOST */
    int feature_index_set = 0;
    int cache_mode = CACHE_MODE_AUTO;
    int silent = 0;
    long host = -1;
    int host_arg_idx = 1;
    char cache_path[512] = {0};

    /* Parse optional flags */
    while (host_arg_idx < argc) {
        if (strcmp(argv[host_arg_idx], "-s") == 0) {
            silent = 1;
            host_arg_idx++;
        } else if (strcmp(argv[host_arg_idx], "--path") == 0 && host_arg_idx + 1 < argc) {
            device_path = argv[host_arg_idx + 1];
            host_arg_idx += 2;
        } else if (strcmp(argv[host_arg_idx], "--devnum") == 0 && host_arg_idx + 1 < argc) {
            long parsed = 0;
            if (parse_long_strict(argv[host_arg_idx + 1], 0, 0, 255, &parsed) != 0) {
                usage(argv[0]);
                return 1;
            }
            devnum_override = (int)parsed;
            host_arg_idx += 2;
        } else if (strcmp(argv[host_arg_idx], "--feature-index") == 0 && host_arg_idx + 1 < argc) {
            long parsed = 0;
            if (parse_long_strict(argv[host_arg_idx + 1], 0, 0, 255, &parsed) != 0) {
                usage(argv[0]);
                return 1;
            }
            feature_index_override = (int)parsed;
            feature_index_set = 1;
            host_arg_idx += 2;
        } else if (strcmp(argv[host_arg_idx], "--slot") == 0 && host_arg_idx + 1 < argc) {
            if (parse_long_strict(argv[host_arg_idx + 1], 10, 1, 3, &host) != 0) {
                usage(argv[0]);
                return 1;
            }
            host_arg_idx += 2;
        } else if (strcmp(argv[host_arg_idx], "--cache") == 0 && host_arg_idx + 1 < argc) {
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
            cache_file_override = argv[host_arg_idx + 1];
            host_arg_idx += 2;
        } else {
            break;
        }
    }

    /* If host not yet set, try to parse positional argument */
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

    if (host < 1 || host > 3) {
        usage(argv[0]);
        return 1;
    }
    uint8_t host_slot = (uint8_t)(host - 1);

    if (hid_init() != 0) {
        fprintf(stderr, "hidapi init failed\n");
        return 1;
    }

    if (cache_file_override) {
        if (snprintf(cache_path, sizeof(cache_path), "%s", cache_file_override) >= (int)sizeof(cache_path)) {
            fprintf(stderr, "Cache file path is too long\n");
            hid_exit();
            return 1;
        }
    } else if (get_default_cache_path(cache_path, sizeof(cache_path)) != 0) {
        fprintf(stderr, "Failed to build default cache path\n");
        hid_exit();
        return 1;
    }

    uint8_t devnum = 0;
    uint8_t ch_index = 0;
    char *path = NULL;
    hid_device *dev = NULL;
    int has_explicit_fast_path = (device_path && devnum_override >= 0);

    if (cache_mode == CACHE_MODE_AUTO && !has_explicit_fast_path && !device_path && devnum_override < 0 && !feature_index_set) {
        char cached_path[768] = {0};
        uint8_t cached_devnum = 0;
        uint8_t cached_feature_index = 0;

        if (load_cache(cache_path, cached_path, sizeof(cached_path), &cached_devnum, &cached_feature_index) == 0) {
            hid_device *cached_dev = hid_open_path(cached_path);
            if (cached_dev) {
                int cached_rc = switch_host(cached_dev, cached_devnum, cached_feature_index, host_slot);
                hid_close(cached_dev);
                if (cached_rc == 0) {
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
            invalidate_cache(cache_path);
        }
    }

    if (cache_mode == CACHE_MODE_REFRESH) {
        invalidate_cache(cache_path);
    }

    if (device_path && devnum_override >= 0) {
        /* Fast path: open device directly without any feature discovery */
        dev = hid_open_path(device_path);
        if (dev) {
            devnum = (uint8_t)devnum_override;
            ch_index = (uint8_t)feature_index_override;
            /* Skip strdup in silent mode to avoid allocation */
            if (!silent) {
                path = strdup(device_path);
            }
        }
    } else if (device_path) {
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
        dev = open_first_device(&devnum, &ch_index, &path);
    }
    if (!dev) {
        fprintf(stderr, "No Logitech HID++ device with CHANGE_HOST found\n");
        hid_exit();
        return 1;
    }

    int rc = switch_host(dev, devnum, ch_index, host_slot);
    if (rc == 0 && !silent) {
        printf("Switched host to slot %ld (device %u, feature index %u)%s%s\n",
               host,
               devnum,
               ch_index,
               path ? " via " : "",
               path ? path : "");
    } else if (rc != 0) {
        fprintf(stderr, "Failed to switch host\n");
    }

    if (rc == 0 && cache_mode != CACHE_MODE_OFF) {
        const char *path_for_cache = device_path ? device_path : path;
        if (path_for_cache && path_for_cache[0] != '\0') {
            (void)save_cache(cache_path, path_for_cache, devnum, ch_index);
        }
    }

    free(path);
    hid_close(dev);
    hid_exit();
    return rc == 0 ? 0 : 1;
}
