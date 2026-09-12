#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "mesh/transport/ble_hci.h"

#include "mesh/utils/ioctl.h"
#include "mesh/utils/log.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * The handful of kernel definitions this needs, stated here rather than taken from BlueZ's
 * `<bluetooth/hci.h>`: that header belongs to libbluetooth, which the client does not link and
 * the cross container does not carry, and the four numbers below are kernel ABI that has not
 * moved since 2.6.
 */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define MESH_BTPROTO_HCI 1
#define MESH_HCI_CHANNEL_RAW 0
#define MESH_HCI_COMMAND_PKT 0x01U
#define MESH_HCI_LE_LINK 0x80U
#define MESH_HCIGETCONNLIST _IOR('H', 212, int)
/* OGF 0x08 (LE controller) << 10 | OCF 0x0013. */
#define MESH_HCI_OP_LE_CONN_UPDATE 0x2013U
#define MESH_HCI_CONN_MAX 16U

struct mesh_sockaddr_hci {
    sa_family_t hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

/* `struct hci_conn_info`: bdaddr_t is a packed six bytes, so this is 16 bytes with no padding
   and the same layout on every architecture the kernel supports. */
struct mesh_hci_conn_info {
    uint16_t handle;
    uint8_t bdaddr[6];
    uint8_t type;
    uint8_t out;
    uint16_t state;
    uint32_t link_mode;
};

struct mesh_hci_conn_list {
    uint16_t dev_id;
    uint16_t conn_num;
    struct mesh_hci_conn_info info[MESH_HCI_CONN_MAX];
};

const struct mesh_ble_hci_conn_params mesh_ble_hci_ota_params = {
    .min_interval = MESH_BLE_HCI_INTERVAL_7_5_MS,
    .max_interval = MESH_BLE_HCI_INTERVAL_7_5_MS,
    .latency = 0U,
    /* 4 s, which is what the loader asks for itself (`updateConnParams(..., 400)`). */
    .supervision_timeout = 400U,
};

static void put_le16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)(value >> 8);
}

size_t mesh_ble_hci_encode_conn_update(uint16_t handle,
                                       const struct mesh_ble_hci_conn_params *params,
                                       uint8_t out[MESH_BLE_HCI_CONN_UPDATE_LEN]) {
    if (params == NULL || out == NULL || handle > 0x0EFFU) {
        return 0U;
    }
    /* The spec's limits, checked here because a controller that is handed one it does not like
       answers with a status nothing reads - and the transfer then runs slow for no stated
       reason. The timeout has to outlast (1 + latency) * max interval * 2, compared in 1.25 ms
       units against the timeout's 10 ms ones. */
    if (params->min_interval < 6U || params->max_interval > 3200U ||
        params->min_interval > params->max_interval || params->latency > 499U ||
        params->supervision_timeout < 10U || params->supervision_timeout > 3200U ||
        (uint32_t)params->supervision_timeout * 8U <=
            (1U + (uint32_t)params->latency) * (uint32_t)params->max_interval * 2U) {
        return 0U;
    }
    out[0] = MESH_HCI_COMMAND_PKT;
    put_le16(&out[1], MESH_HCI_OP_LE_CONN_UPDATE);
    out[3] = 14U;
    put_le16(&out[4], handle);
    put_le16(&out[6], params->min_interval);
    put_le16(&out[8], params->max_interval);
    put_le16(&out[10], params->latency);
    put_le16(&out[12], params->supervision_timeout);
    /* Minimum and maximum connection-event length: zero, "whatever the controller likes". */
    put_le16(&out[14], 0U);
    put_le16(&out[16], 0U);
    return MESH_BLE_HCI_CONN_UPDATE_LEN;
}

static int hex_value(int c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = tolower(c);
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

bool mesh_ble_hci_parse_address(const char *text, uint8_t out[6]) {
    if (text == NULL || out == NULL || strlen(text) != 17U) {
        return false;
    }
    for (size_t i = 0; i < 6U; ++i) {
        const int hi = hex_value((unsigned char)text[i * 3U]);
        const int lo = hex_value((unsigned char)text[i * 3U + 1U]);
        if (hi < 0 || lo < 0 || (i < 5U && text[i * 3U + 2U] != ':')) {
            return false;
        }
        /* bdaddr_t is least significant byte first, so the last pair written is out[0]. */
        out[5U - i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

int mesh_ble_hci_adapter_index(const char *adapter_path) {
    if (adapter_path == NULL) {
        return -EINVAL;
    }
    const char *const hci = strstr(adapter_path, "hci");
    if (hci == NULL || hci[3] == '\0') {
        return -EINVAL;
    }
    int index = 0;
    for (const char *c = hci + 3; *c != '\0'; ++c) {
        if (!isdigit((unsigned char)*c) || index > 1000) {
            return -EINVAL;
        }
        index = index * 10 + (*c - '0');
    }
    return index;
}

int mesh_ble_hci_request_interval(int dev_id, const char *address,
                                  const struct mesh_ble_hci_conn_params *params) {
    uint8_t bdaddr[6];
    if (dev_id < 0 || params == NULL || !mesh_ble_hci_parse_address(address, bdaddr)) {
        return -EINVAL;
    }

    const int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, MESH_BTPROTO_HCI);
    if (fd < 0) {
        return -errno;
    }
    struct mesh_sockaddr_hci bind_to;
    memset(&bind_to, 0, sizeof bind_to);
    bind_to.hci_family = AF_BLUETOOTH;
    bind_to.hci_dev = (unsigned short)dev_id;
    bind_to.hci_channel = MESH_HCI_CHANNEL_RAW;
    if (bind(fd, (const struct sockaddr *)&bind_to, sizeof bind_to) < 0) {
        const int error = -errno;
        close(fd);
        return error;
    }

    /* BlueZ knows the link by address and the controller by handle, and nothing on D-Bus says
       one in terms of the other - so the kernel's own connection list is where it is read. */
    struct mesh_hci_conn_list list;
    memset(&list, 0, sizeof list);
    list.dev_id = (uint16_t)dev_id;
    list.conn_num = MESH_HCI_CONN_MAX;
    if (ioctl(fd, mesh_ioctl_request_of(MESH_HCIGETCONNLIST), &list) < 0) {
        const int error = -errno;
        close(fd);
        return error;
    }
    int handle = -1;
    for (size_t i = 0; i < list.conn_num && i < MESH_HCI_CONN_MAX; ++i) {
        if (list.info[i].type == MESH_HCI_LE_LINK && memcmp(list.info[i].bdaddr, bdaddr, 6) == 0) {
            handle = list.info[i].handle;
            break;
        }
    }
    if (handle < 0) {
        close(fd);
        return -ENOENT;
    }

    uint8_t packet[MESH_BLE_HCI_CONN_UPDATE_LEN];
    const size_t len = mesh_ble_hci_encode_conn_update((uint16_t)handle, params, packet);
    if (len == 0U) {
        close(fd);
        return -EINVAL;
    }
    const ssize_t sent = write(fd, packet, len);
    const int error = sent < 0 ? -errno : (sent == (ssize_t)len ? 0 : -EIO);
    close(fd);
    if (error == 0) {
        mesh_log_info("ble", "Asked for a %u.%02u ms connection interval on %s (handle %d)",
                      (unsigned)(params->min_interval * 125U / 100U),
                      (unsigned)(params->min_interval * 125U % 100U), address, handle);
    }
    return error;
}
