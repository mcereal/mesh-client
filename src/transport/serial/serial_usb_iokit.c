#include "serial_usb_internal.h"

#include "inkwell/base/log.h"
#include "inkwell/base/text.h"

#include <stdio.h>
#include <string.h>

/*
 * The macOS half of the scan. A Mac has no sysfs, so the question "which USB serial ports are
 * there, and what is on the other end of each" goes to the I/O Registry instead: every tty is an
 * IOSerialBSDClient, and the USB interface and device it hangs off carry the same class, vendor
 * and product numbers sysfs would have given.
 *
 * Nothing here needs the Brick's workaround. macOS has a driver for CDC-ACM (AppleUSBACM) and for
 * the common bridges (AppleUSBSLCOM, AppleUSBCHCOM, AppleUSBFTDI), so every port is already
 * bound and DTR is a plain TIOCMBIS.
 */

#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>

#define MESH_USB_CLASS_CDC_DATA 0x0AU
#define MESH_USB_CLASS_MASS_STORAGE 0x08U
#define MESH_USB_SUBCLASS_SCSI 0x06U
#define MESH_USB_PROTOCOL_BULK_ONLY 0x50U

/* A property of this entry, or failing that of the nearest ancestor that has one. */
static CFTypeRef search_up(io_registry_entry_t entry, CFStringRef key) {
    return IORegistryEntrySearchCFProperty(entry, kIOServicePlane, key, kCFAllocatorDefault,
                                           kIORegistryIterateRecursively |
                                               kIORegistryIterateParents);
}

static bool cf_to_long(CFTypeRef value, long *out) {
    if (value == NULL) {
        return false;
    }
    bool ok = CFGetTypeID(value) == CFNumberGetTypeID() &&
              CFNumberGetValue((CFNumberRef)value, kCFNumberLongType, out);
    CFRelease(value);
    return ok;
}

static bool cf_to_string(CFTypeRef value, char *out, size_t out_len) {
    if (value == NULL) {
        return false;
    }
    bool ok = CFGetTypeID(value) == CFStringGetTypeID() &&
              CFStringGetCString((CFStringRef)value, out, (CFIndex)out_len, kCFStringEncodingUTF8);
    CFRelease(value);
    return ok;
}

static bool own_long(io_registry_entry_t entry, CFStringRef key, long *out) {
    return cf_to_long(IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0), out);
}

/* The IOUSBHostDevice this port belongs to, retained, or 0 when it is not on USB at all - the
   Bluetooth-Incoming-Port and the debug console are ttys too. */
static io_registry_entry_t usb_device_of(io_registry_entry_t port) {
    io_registry_entry_t entry = port;
    IOObjectRetain(entry);
    for (;;) {
        if (IOObjectConformsTo(entry, "IOUSBHostDevice") ||
            IOObjectConformsTo(entry, "IOUSBDevice")) {
            return entry;
        }
        io_registry_entry_t parent = 0;
        const kern_return_t kr = IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent);
        IOObjectRelease(entry);
        if (kr != KERN_SUCCESS) {
            return 0;
        }
        entry = parent;
    }
}

/* The same test the sysfs scan makes on a device's siblings: a Bulk-Only mass-storage interface
   beside the CDC pair is a UF2 bootloader. */
static bool device_has_mass_storage(io_registry_entry_t device) {
    io_iterator_t it = 0;
    if (IORegistryEntryCreateIterator(device, kIOServicePlane, kIORegistryIterateRecursively,
                                      &it) != KERN_SUCCESS) {
        return false;
    }
    bool found = false;
    io_registry_entry_t child = 0;
    while (!found && (child = IOIteratorNext(it)) != 0) {
        long cls = -1;
        long sub = -1;
        long proto = -1;
        if (own_long(child, CFSTR("bInterfaceClass"), &cls) &&
            cls == (long)MESH_USB_CLASS_MASS_STORAGE &&
            own_long(child, CFSTR("bInterfaceSubClass"), &sub) &&
            sub == (long)MESH_USB_SUBCLASS_SCSI &&
            own_long(child, CFSTR("bInterfaceProtocol"), &proto) &&
            proto == (long)MESH_USB_PROTOCOL_BULK_ONLY) {
            found = true;
        }
        IOObjectRelease(child);
    }
    IOObjectRelease(it);
    return found;
}

size_t mesh_serial_usb_scan_iokit(struct mesh_serial_device_info *out, size_t capacity) {
    CFMutableDictionaryRef match = IOServiceMatching(kIOSerialBSDServiceValue);
    if (match == NULL) {
        return 0U;
    }
    CFDictionarySetValue(match, CFSTR(kIOSerialBSDTypeKey), CFSTR(kIOSerialBSDAllTypes));

    io_iterator_t it = 0;
    /* Consumes `match`. MACH_PORT_NULL is the default main port under either of its names. */
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, match, &it) != KERN_SUCCESS) {
        inkwell_log_debug("serial", "I/O Registry serial lookup failed");
        return 0U;
    }

    size_t count = 0U;
    io_registry_entry_t port = 0;
    while (count < capacity && (port = IOIteratorNext(it)) != 0) {
        io_registry_entry_t device = usb_device_of(port);
        char path[sizeof out->path];
        if (device == 0 ||
            !cf_to_string(IORegistryEntryCreateCFProperty(port, CFSTR(kIOCalloutDeviceKey),
                                                          kCFAllocatorDefault, 0),
                          path, sizeof path)) {
            if (device != 0) {
                IOObjectRelease(device);
            }
            IOObjectRelease(port);
            continue;
        }

        struct mesh_serial_device_info *info = &out[count];
        memset(info, 0, sizeof *info);
        /* The callout path is the id: macOS names it after the USB location, so it survives a
           replug into the same socket just as a sysfs interface name does. */
        inkwell_str_copy(info->id, sizeof info->id, path);
        inkwell_str_copy(info->path, sizeof info->path, path);
        info->bound = true;
        info->control_interface = -1;
        info->needs_line_state = false;

        long value = 0;
        if (own_long(device, CFSTR("idVendor"), &value)) {
            info->vendor_id = (uint16_t)value;
        }
        if (own_long(device, CFSTR("idProduct"), &value)) {
            info->product_id = (uint16_t)value;
        }

        /* The nearest bInterfaceClass above the tty is the interface it was made from: CDC-Data
           for a native node, vendor-specific for a bridge chip. */
        long iface_class = -1;
        const bool cdc_data = cf_to_long(search_up(port, CFSTR("bInterfaceClass")), &iface_class) &&
                              iface_class == (long)MESH_USB_CLASS_CDC_DATA;
        if (cdc_data) {
            info->role = device_has_mass_storage(device) ? MESH_SERIAL_ROLE_BOOTLOADER
                                                         : MESH_SERIAL_ROLE_NODE;
        } else {
            info->role = MESH_SERIAL_ROLE_BRIDGE;
        }

        if (!cf_to_string(IORegistryEntryCreateCFProperty(device, CFSTR("USB Product Name"),
                                                          kCFAllocatorDefault, 0),
                          info->name, sizeof info->name) ||
            info->name[0] == '\0') {
            snprintf(info->name, sizeof info->name, "USB serial %04x:%04x", info->vendor_id,
                     info->product_id);
        }

        IOObjectRelease(device);
        IOObjectRelease(port);
        ++count;
    }
    IOObjectRelease(it);
    return count;
}

#else

size_t mesh_serial_usb_scan_iokit(struct mesh_serial_device_info *out, size_t capacity) {
    (void)out;
    (void)capacity;
    return 0U;
}

#endif
