#include <errno.h>
#include <hidapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOGITECH_VID 0x046D
#define REPORT_ID_LONG 0x11
#define MAX_PAYLOAD 18
#define DEFAULT_TIMEOUT_MS 4000
#define FEATURE_CHANGE_HOST 0x1814
#define FEATURE_FEATURE_SET 0x0001

static uint16_t next_sw_id(void) {
    static uint8_t sw = 0x0F;
    sw = (sw < 0x0F) ? (uint8_t)(sw + 1) : 0x02;
    return sw;
}

static int write_long(hid_device *dev, uint8_t devnum, const uint8_t *payload, size_t payload_len) {
    if (payload_len > MAX_PAYLOAD) {
        return -1;
    }
    uint8_t buf[2 + MAX_PAYLOAD] = {0};
    buf[0] = REPORT_ID_LONG;
    buf[1] = devnum;
    if (payload_len) {
        memcpy(buf + 2, payload, payload_len);
    }
    int written = hid_write(dev, buf, sizeof(buf));
    return (written == (int)sizeof(buf)) ? 0 : -1;
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
    return hidpp_request(dev, devnum, request_id, params, sizeof(params), NULL, 0, 0, DEFAULT_TIMEOUT_MS);
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
    fprintf(stderr, "Usage: %s [-s] [--path PATH] [--devnum DEVNUM] [--feature-index INDEX] [--slot SLOT] <host-number-1-3>\n", prog);
}

int main(int argc, char **argv) {
    const char *device_path = NULL;
    int devnum_override = -1;
    int feature_index_override = 14;  /* Default to 14 for CHANGE_HOST */
    int silent = 0;
    long host = -1;
    int host_arg_idx = 1;

    /* Parse optional flags */
    while (host_arg_idx < argc) {
        if (strcmp(argv[host_arg_idx], "-s") == 0) {
            silent = 1;
            host_arg_idx++;
        } else if (strcmp(argv[host_arg_idx], "--path") == 0 && host_arg_idx + 1 < argc) {
            device_path = argv[host_arg_idx + 1];
            host_arg_idx += 2;
        } else if (strcmp(argv[host_arg_idx], "--devnum") == 0 && host_arg_idx + 1 < argc) {
            char *end = NULL;
            devnum_override = (int)strtol(argv[host_arg_idx + 1], &end, 0);
            host_arg_idx += 2;
        } else if (strcmp(argv[host_arg_idx], "--feature-index") == 0 && host_arg_idx + 1 < argc) {
            char *end = NULL;
            feature_index_override = (int)strtol(argv[host_arg_idx + 1], &end, 0);
            host_arg_idx += 2;
        } else if (strcmp(argv[host_arg_idx], "--slot") == 0 && host_arg_idx + 1 < argc) {
            char *end = NULL;
            host = strtol(argv[host_arg_idx + 1], &end, 10);
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
        char *end = NULL;
        host = strtol(argv[host_arg_idx], &end, 10);
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

    uint8_t devnum = 0;
    uint8_t ch_index = 0;
    char *path = NULL;
    hid_device *dev = NULL;

    if (device_path && devnum_override >= 0 && feature_index_override >= 0) {
        /* Fast path: open device directly without any feature discovery */
        dev = hid_open_path(device_path);
        if (dev) {
            devnum = (uint8_t)devnum_override;
            ch_index = (uint8_t)feature_index_override;
            path = strdup(device_path);
        }
    } else if (device_path) {
        dev = open_device_by_path(device_path, &devnum, &ch_index);
        if (!dev) {
            fprintf(stderr, "Failed to open device at path: %s\n", device_path);
            fprintf(stderr, "Use auto-discovery or provide --devnum and --feature-index to skip discovery\n");
            hid_exit();
            return 1;
        }
        path = strdup(device_path);
    } else {
        dev = open_first_device(&devnum, &ch_index, &path);
    }
    if (!dev) {
        fprintf(stderr, "No Logitech HID++ device with CHANGE_HOST found\n");
        hid_exit();
        return 1;
    }

    int rc = switch_host(dev, devnum, ch_index, host_slot);
    if (rc == 0) {
        if (!silent) {
            printf("Switched host to slot %ld (device %u, feature index %u)%s%s\n",
                   host,
                   devnum,
                   ch_index,
                   path ? " via " : "",
                   path ? path : "");
        }
    } else {
        fprintf(stderr, "Failed to switch host\n");
    }

    if (path) {
        free(path);
    }
    hid_close(dev);
    hid_exit();
    return rc == 0 ? 0 : 1;
}
