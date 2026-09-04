#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "mesh/transport/serial_usb.h"

#include "mesh/log.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/usbdevice_fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MESH_SERIAL_SYSFS_USB "/sys/bus/usb/devices"
#define MESH_SERIAL_GENERIC_NEW_ID "/sys/bus/usb-serial/drivers/generic/new_id"

/* USB interface classes we care about. */
#define MESH_USB_CLASS_COMM 0x02U
#define MESH_USB_SUBCLASS_ACM 0x02U
#define MESH_USB_CLASS_CDC_DATA 0x0AU

/*
 * The USBDEVFS request codes have the high bit set, and the two libcs disagree on the parameter:
 * glibc takes `unsigned long`, musl (which the release build links against) takes `int`. Narrow
 * explicitly for each rather than letting one of them overflow the constant.
 */
#if defined(__GLIBC__)
#define MESH_IOCTL_REQUEST(req) ((unsigned long)(req))
#else
#define MESH_IOCTL_REQUEST(req) ((int)(req))
#endif

/* CDC SET_CONTROL_LINE_STATE (USB CDC 1.1, 6.2.14). */
#define MESH_CDC_REQUEST_TYPE 0x21U
#define MESH_CDC_SET_CONTROL_LINE_STATE 0x22U

/* How long to wait for the generic driver to publish a tty after a new_id write. */
#define MESH_SERIAL_BIND_TIMEOUT_MS 1000U
#define MESH_SERIAL_BIND_POLL_MS 50U

/* usbserial drivers whose ports are worth offering even when the interface class is
   vendor-specific, which is what the UART-bridge chips report. */
static const char *const k_serial_drivers[] = {"cp210x", "ch341",   "ch341-uart",
                                               "ftdi_sio", "generic", "cdc_acm"};

struct mesh_serial_usb_mock_state {
    bool enabled;
    struct mesh_serial_usb_mock_config config;
    size_t bind_calls;
    size_t line_state_calls;
};

static struct mesh_serial_usb_mock_state g_mock_state;

void mesh_serial_usb_mock_enable(const struct mesh_serial_usb_mock_config *config) {
    memset(&g_mock_state, 0, sizeof g_mock_state);
    g_mock_state.enabled = true;
    if (config != NULL) {
        g_mock_state.config = *config;
    } else {
        g_mock_state.config.open_fd = -1;
    }
}

void mesh_serial_usb_mock_disable(void) {
    memset(&g_mock_state, 0, sizeof g_mock_state);
}

size_t mesh_serial_usb_mock_bind_calls(void) {
    return g_mock_state.bind_calls;
}

size_t mesh_serial_usb_mock_line_state_calls(void) {
    return g_mock_state.line_state_calls;
}

static void mesh_serial_sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000U);
    ts.tv_nsec = (long)(ms % 1000U) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

/* Reads a one-line sysfs attribute with the trailing newline stripped. */
static int read_sysfs_string(const char *dir, const char *attr, char *out, size_t out_len) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s", dir, attr) >= (int)sizeof path) {
        return -ENAMETOOLONG;
    }
    FILE *file = fopen(path, "re");
    if (file == NULL) {
        return -errno;
    }
    if (fgets(out, (int)out_len, file) == NULL) {
        fclose(file);
        return -EIO;
    }
    fclose(file);
    size_t len = strlen(out);
    while (len > 0U && (out[len - 1U] == '\n' || out[len - 1U] == '\r')) {
        out[--len] = '\0';
    }
    return 0;
}

static int read_sysfs_number(const char *dir, const char *attr, int base, unsigned long *out) {
    char text[64];
    int result = read_sysfs_string(dir, attr, text, sizeof text);
    if (result < 0) {
        return result;
    }
    char *end = NULL;
    unsigned long value = strtoul(text, &end, base);
    if (end == text) {
        return -EINVAL;
    }
    *out = value;
    return 0;
}

/* The driver bound to a sysfs device, from the basename of its `driver` symlink. */
static bool sysfs_driver(const char *dir, char *out, size_t out_len) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/driver", dir) >= (int)sizeof path) {
        return false;
    }
    char target[PATH_MAX];
    ssize_t len = readlink(path, target, sizeof target - 1U);
    if (len < 0) {
        return false;
    }
    target[len] = '\0';
    const char *base = strrchr(target, '/');
    /* Explicit precision: sysfs paths are PATH_MAX, the field they land in is not. */
    snprintf(out, out_len, "%.*s", (int)(out_len - 1U), base != NULL ? base + 1 : target);
    return true;
}

/* "/dev/ttyUSB0" for an interface that has one. cdc_acm publishes tty/ttyACM0, usb-serial
   publishes a ttyUSB0 port directory; check for both shapes. */
static bool find_interface_tty(const char *iface_dir, char *out, size_t out_len) {
    DIR *dir = opendir(iface_dir);
    if (dir == NULL) {
        return false;
    }

    bool found = false;
    const struct dirent *entry = NULL;
    while (!found && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, "tty") == 0) {
            char tty_dir[PATH_MAX];
            if (snprintf(tty_dir, sizeof tty_dir, "%s/tty", iface_dir) >= (int)sizeof tty_dir) {
                continue;
            }
            DIR *inner = opendir(tty_dir);
            if (inner == NULL) {
                continue;
            }
            const struct dirent *tty_entry = NULL;
            while ((tty_entry = readdir(inner)) != NULL) {
                if (strncmp(tty_entry->d_name, "tty", 3) == 0) {
                    snprintf(out, out_len, "/dev/%.*s", (int)(out_len - sizeof "/dev/"),
                             tty_entry->d_name);
                    found = true;
                    break;
                }
            }
            closedir(inner);
        } else if (strncmp(entry->d_name, "tty", 3) == 0 && entry->d_name[3] != '\0') {
            snprintf(out, out_len, "/dev/%.*s", (int)(out_len - sizeof "/dev/"), entry->d_name);
            found = true;
        }
    }

    closedir(dir);
    return found;
}

static bool is_serial_driver(const char *driver) {
    for (size_t i = 0; i < sizeof k_serial_drivers / sizeof k_serial_drivers[0]; ++i) {
        if (strcmp(driver, k_serial_drivers[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* A sysfs name like "1-1:1.0" is an interface; "1-1" is the device it belongs to. */
static bool split_interface_name(const char *name, char *device_out, size_t device_out_len) {
    const char *colon = strchr(name, ':');
    if (colon == NULL || colon == name) {
        return false;
    }
    size_t len = (size_t)(colon - name);
    if (len >= device_out_len) {
        return false;
    }
    memcpy(device_out, name, len);
    device_out[len] = '\0';
    return true;
}

/* bInterfaceNumber of the device's CDC control interface (class 02 subclass 02), or -1. */
static int find_control_interface(const char *device_name) {
    DIR *dir = opendir(MESH_SERIAL_SYSFS_USB);
    if (dir == NULL) {
        return -1;
    }

    int control = -1;
    const struct dirent *entry = NULL;
    while (control < 0 && (entry = readdir(dir)) != NULL) {
        char owner[64];
        if (!split_interface_name(entry->d_name, owner, sizeof owner) ||
            strcmp(owner, device_name) != 0) {
            continue;
        }

        char iface_dir[PATH_MAX];
        if (snprintf(iface_dir, sizeof iface_dir, "%s/%s", MESH_SERIAL_SYSFS_USB, entry->d_name) >=
            (int)sizeof iface_dir) {
            continue;
        }

        unsigned long iface_class = 0U;
        unsigned long iface_subclass = 0U;
        unsigned long iface_number = 0U;
        if (read_sysfs_number(iface_dir, "bInterfaceClass", 16, &iface_class) < 0 ||
            read_sysfs_number(iface_dir, "bInterfaceSubClass", 16, &iface_subclass) < 0 ||
            read_sysfs_number(iface_dir, "bInterfaceNumber", 16, &iface_number) < 0) {
            continue;
        }
        if (iface_class == MESH_USB_CLASS_COMM && iface_subclass == MESH_USB_SUBCLASS_ACM) {
            control = (int)iface_number;
        }
    }

    closedir(dir);
    return control;
}

static size_t mock_scan(struct mesh_serial_device_info *out, size_t capacity) {
    if (g_mock_state.config.scan_result < 0 || g_mock_state.config.devices == NULL) {
        return 0U;
    }
    size_t count = g_mock_state.config.device_count;
    if (count > capacity) {
        count = capacity;
    }
    for (size_t i = 0; i < count; ++i) {
        out[i] = g_mock_state.config.devices[i];
    }
    return count;
}

size_t mesh_serial_usb_scan(struct mesh_serial_device_info *out, size_t capacity) {
    if (out == NULL || capacity == 0U) {
        return 0U;
    }
    if (g_mock_state.enabled) {
        return mock_scan(out, capacity);
    }

    DIR *dir = opendir(MESH_SERIAL_SYSFS_USB);
    if (dir == NULL) {
        mesh_log_debug("serial", "No USB sysfs at %s: %s", MESH_SERIAL_SYSFS_USB, strerror(errno));
        return 0U;
    }

    size_t count = 0U;
    const struct dirent *entry = NULL;
    while (count < capacity && (entry = readdir(dir)) != NULL) {
        char device_name[64];
        if (!split_interface_name(entry->d_name, device_name, sizeof device_name)) {
            continue; /* a device, not an interface */
        }

        char iface_dir[PATH_MAX];
        char device_dir[PATH_MAX];
        if (snprintf(iface_dir, sizeof iface_dir, "%s/%s", MESH_SERIAL_SYSFS_USB, entry->d_name) >=
                (int)sizeof iface_dir ||
            snprintf(device_dir, sizeof device_dir, "%s/%s", MESH_SERIAL_SYSFS_USB, device_name) >=
                (int)sizeof device_dir) {
            continue;
        }

        unsigned long iface_class = 0U;
        if (read_sysfs_number(iface_dir, "bInterfaceClass", 16, &iface_class) < 0) {
            continue;
        }

        char driver[64] = {0};
        const bool has_driver = sysfs_driver(iface_dir, driver, sizeof driver);
        const bool cdc_data = iface_class == MESH_USB_CLASS_CDC_DATA;
        if (!cdc_data && !(has_driver && is_serial_driver(driver))) {
            continue;
        }

        struct mesh_serial_device_info *info = &out[count];
        memset(info, 0, sizeof *info);
        snprintf(info->id, sizeof info->id, "%.*s", (int)(sizeof info->id - 1U),
                 entry->d_name);
        info->bound = find_interface_tty(iface_dir, info->path, sizeof info->path);
        info->control_interface = find_control_interface(device_name);
        info->needs_line_state =
            info->control_interface >= 0 && (!has_driver || strcmp(driver, "cdc_acm") != 0);

        unsigned long value = 0U;
        if (read_sysfs_number(device_dir, "idVendor", 16, &value) == 0) {
            info->vendor_id = (uint16_t)value;
        }
        if (read_sysfs_number(device_dir, "idProduct", 16, &value) == 0) {
            info->product_id = (uint16_t)value;
        }
        if (read_sysfs_number(device_dir, "busnum", 10, &value) == 0) {
            info->busnum = (uint8_t)value;
        }
        if (read_sysfs_number(device_dir, "devnum", 10, &value) == 0) {
            info->devnum = (uint8_t)value;
        }
        if (read_sysfs_string(device_dir, "product", info->name, sizeof info->name) < 0 ||
            info->name[0] == '\0') {
            snprintf(info->name, sizeof info->name, "USB serial %04x:%04x", info->vendor_id,
                     info->product_id);
        }

        ++count;
    }

    closedir(dir);
    return count;
}

int mesh_serial_usb_bind(struct mesh_serial_device_info *device) {
    if (device == NULL) {
        return -EINVAL;
    }

    if (g_mock_state.enabled) {
        g_mock_state.bind_calls += 1U;
        if (g_mock_state.config.bind_result < 0) {
            return g_mock_state.config.bind_result;
        }
        if (!device->bound) {
            const char *path = g_mock_state.config.bound_path != NULL
                                   ? g_mock_state.config.bound_path
                                   : "/dev/ttyUSB0";
            snprintf(device->path, sizeof device->path, "%s", path);
            device->bound = true;
        }
        return 0;
    }

    if (device->bound && device->path[0] != '\0') {
        return 0;
    }

    FILE *new_id = fopen(MESH_SERIAL_GENERIC_NEW_ID, "we");
    if (new_id == NULL) {
        mesh_log_warn("serial", "Cannot open %s: %s", MESH_SERIAL_GENERIC_NEW_ID, strerror(errno));
        return -errno;
    }
    /* The generic driver rejects the control interface ("no bulk out") and takes the data one. */
    const int printed =
        fprintf(new_id, "%04x %04x\n", (unsigned)device->vendor_id, (unsigned)device->product_id);
    const int flushed = fclose(new_id);
    if (printed < 0 || flushed != 0) {
        mesh_log_warn("serial", "new_id write for %04x:%04x failed: %s", device->vendor_id,
                      device->product_id, strerror(errno));
        return -EIO;
    }
    mesh_log_info("serial", "Bound %04x:%04x to the generic usbserial driver", device->vendor_id,
                  device->product_id);

    char iface_dir[PATH_MAX];
    if (snprintf(iface_dir, sizeof iface_dir, "%s/%s", MESH_SERIAL_SYSFS_USB, device->id) >=
        (int)sizeof iface_dir) {
        return -ENAMETOOLONG;
    }

    for (unsigned waited = 0U; waited < MESH_SERIAL_BIND_TIMEOUT_MS;
         waited += MESH_SERIAL_BIND_POLL_MS) {
        if (find_interface_tty(iface_dir, device->path, sizeof device->path)) {
            device->bound = true;
            device->needs_line_state = device->control_interface >= 0;
            mesh_log_info("serial", "%s is now %s", device->id, device->path);
            return 0;
        }
        mesh_serial_sleep_ms(MESH_SERIAL_BIND_POLL_MS);
    }

    mesh_log_warn("serial", "No tty appeared for %s after %u ms", device->id,
                  MESH_SERIAL_BIND_TIMEOUT_MS);
    return -ENODEV;
}

int mesh_serial_usb_set_line_state(const struct mesh_serial_device_info *device, bool dtr,
                                   bool rts) {
    if (device == NULL) {
        return -EINVAL;
    }

    if (g_mock_state.enabled) {
        g_mock_state.line_state_calls += 1U;
        return g_mock_state.config.line_state_result;
    }

    if (device->control_interface < 0) {
        return -ENOTSUP;
    }

    char usbfs_path[PATH_MAX];
    snprintf(usbfs_path, sizeof usbfs_path, "/dev/bus/usb/%03u/%03u", (unsigned)device->busnum,
             (unsigned)device->devnum);

    const int fd = open(usbfs_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        mesh_log_warn("serial", "Cannot open %s: %s", usbfs_path, strerror(errno));
        return -errno;
    }

    /* The control interface has no driver (the generic one refused it), so claiming it is what
       lets usbfs deliver the request. */
    unsigned int iface = (unsigned int)device->control_interface;
    bool claimed = ioctl(fd, MESH_IOCTL_REQUEST(USBDEVFS_CLAIMINTERFACE), &iface) == 0;
    if (!claimed) {
        mesh_log_debug("serial", "Claim of interface %u on %s failed: %s", iface, usbfs_path,
                       strerror(errno));
    }

    struct usbdevfs_ctrltransfer transfer;
    memset(&transfer, 0, sizeof transfer);
    transfer.bRequestType = MESH_CDC_REQUEST_TYPE;
    transfer.bRequest = MESH_CDC_SET_CONTROL_LINE_STATE;
    transfer.wValue = (uint16_t)((dtr ? 0x01U : 0U) | (rts ? 0x02U : 0U));
    transfer.wIndex = (uint16_t)device->control_interface;
    transfer.wLength = 0U;
    transfer.timeout = 1000U;
    transfer.data = NULL;

    int result = 0;
    if (ioctl(fd, MESH_IOCTL_REQUEST(USBDEVFS_CONTROL), &transfer) < 0) {
        result = -errno;
        mesh_log_warn("serial", "SET_CONTROL_LINE_STATE on %s failed: %s", usbfs_path,
                      strerror(errno));
    } else {
        mesh_log_info("serial", "Asserted DTR on %s interface %u", usbfs_path, iface);
    }

    if (claimed) {
        (void)ioctl(fd, MESH_IOCTL_REQUEST(USBDEVFS_RELEASEINTERFACE), &iface);
    }
    close(fd);
    return result;
}

int mesh_serial_port_open(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return -EINVAL;
    }

    if (g_mock_state.enabled) {
        if (g_mock_state.config.open_result < 0) {
            return g_mock_state.config.open_result;
        }
        if (g_mock_state.config.open_fd < 0) {
            return -ENOENT;
        }
        const int duplicated = dup(g_mock_state.config.open_fd);
        return duplicated < 0 ? -errno : duplicated;
    }

    const int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) < 0) {
        const int saved = errno;
        close(fd);
        return -saved;
    }

    cfmakeraw(&tio);
    /* Baud is meaningless over USB CDC; UART bridges need a real one and Meshtastic uses this. */
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= (tcflag_t)(CLOCAL | CREAD);
    tio.c_cflag &= (tcflag_t)~CRTSCTS;
    tio.c_iflag &= (tcflag_t) ~(IXON | IXOFF | IXANY);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        const int saved = errno;
        close(fd);
        return -saved;
    }
    (void)tcflush(fd, TCIOFLUSH);
    return fd;
}

void mesh_serial_port_close(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

int mesh_serial_port_set_dtr(int fd, bool on) {
    if (fd < 0) {
        return -EINVAL;
    }
    int bits = TIOCM_DTR;
    if (ioctl(fd, MESH_IOCTL_REQUEST(on ? TIOCMBIS : TIOCMBIC), &bits) < 0) {
        return -errno;
    }
    return 0;
}
