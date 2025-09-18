#define _POSIX_C_SOURCE 200809L

#include "mesh/ui/store.h"

#include "mesh/log.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

static void mesh_ui_store_mark_dirty(struct mesh_ui_store *store, mesh_ui_update_flags flags) {
    if (store == NULL || flags == MESH_UI_UPDATE_NONE) {
        return;
    }

    store->pending_flags |= flags;

    if (store->event_fd >= 0) {
        const uint64_t value = 1U;
        if (write(store->event_fd, &value, sizeof value) < 0) {
            if (errno != EAGAIN) {
                mesh_log_warn("ui", "eventfd write failed: %s", strerror(errno));
            }
        }
    }
}

int mesh_ui_store_init(struct mesh_ui_store *store) {
    if (store == NULL) {
        return -EINVAL;
    }

    memset(store, 0, sizeof *store);
    store->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (store->event_fd < 0) {
        const int err = -errno;
        mesh_log_error("ui", "eventfd create failed: %s", strerror(errno));
        store->event_fd = -1;
        return err;
    }

    return 0;
}

void mesh_ui_store_shutdown(struct mesh_ui_store *store) {
    if (store == NULL) {
        return;
    }

    if (store->event_fd >= 0) {
        close(store->event_fd);
        store->event_fd = -1;
    }
    store->pending_flags = MESH_UI_UPDATE_NONE;
    store->device_count = 0U;
    store->handshake_valid = false;
}

void mesh_ui_store_reset(struct mesh_ui_store *store) {
    if (store == NULL) {
        return;
    }

    struct mesh_ui_store preserved = *store;
    memset(store, 0, sizeof *store);
    store->event_fd = preserved.event_fd;
    store->pending_flags = preserved.pending_flags;
}

int mesh_ui_store_event_fd(const struct mesh_ui_store *store) {
    if (store == NULL) {
        return -1;
    }
    return store->event_fd;
}

void mesh_ui_store_set_discovery(struct mesh_ui_store *store, const struct mesh_ui_device *devices, size_t count) {
    if (store == NULL) {
        return;
    }

    struct mesh_ui_device next[MESH_UI_MAX_DEVICES];
    memset(next, 0, sizeof(next));

    const size_t capped = (count > MESH_UI_MAX_DEVICES) ? MESH_UI_MAX_DEVICES : count;
    if (devices != NULL && capped > 0U) {
        memcpy(next, devices, capped * sizeof(struct mesh_ui_device));
    }

    const bool count_changed = (store->device_count != capped);
    const bool payload_changed = (memcmp(store->devices, next, sizeof(next)) != 0);

    if (!count_changed && !payload_changed) {
        return;
    }

    memcpy(store->devices, next, sizeof(next));
    store->device_count = capped;

    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_DISCOVERY);
}

void mesh_ui_store_set_handshake(struct mesh_ui_store *store, const struct mesh_ui_handshake_state *handshake) {
    if (store == NULL) {
        return;
    }

    bool next_valid = (handshake != NULL);
    struct mesh_ui_handshake_state next_state;
    memset(&next_state, 0, sizeof(next_state));
    if (next_valid) {
        next_state = *handshake;
    }

    const bool validity_changed = (store->handshake_valid != next_valid);
    const bool payload_changed = next_valid && (memcmp(&store->handshake, &next_state, sizeof(next_state)) != 0);

    if (!validity_changed && !payload_changed) {
        return;
    }

    if (next_valid) {
        store->handshake = next_state;
    } else {
        memset(&store->handshake, 0, sizeof store->handshake);
    }
    store->handshake_valid = next_valid;

    mesh_ui_store_mark_dirty(store, MESH_UI_UPDATE_HANDSHAKE);
}

bool mesh_ui_store_consume_updates(struct mesh_ui_store *store, struct mesh_ui_snapshot *snapshot) {
    if (store == NULL || snapshot == NULL) {
        return false;
    }

    if (store->event_fd >= 0) {
        uint64_t value = 0;
        ssize_t read_result = read(store->event_fd, &value, sizeof value);
        if (read_result < 0 && errno != EAGAIN) {
            mesh_log_warn("ui", "eventfd read failed: %s", strerror(errno));
        }
    }

    if (store->pending_flags == MESH_UI_UPDATE_NONE) {
        return false;
    }

    snapshot->update_flags = store->pending_flags;
    snapshot->device_count = store->device_count;
    if (store->device_count > 0U) {
        memcpy(snapshot->devices, store->devices, store->device_count * sizeof(struct mesh_ui_device));
    }
    if (store->device_count < MESH_UI_MAX_DEVICES) {
        memset(&snapshot->devices[store->device_count], 0,
               (MESH_UI_MAX_DEVICES - store->device_count) * sizeof(struct mesh_ui_device));
    }

    snapshot->handshake_valid = store->handshake_valid;
    if (store->handshake_valid) {
        snapshot->handshake = store->handshake;
    } else {
        memset(&snapshot->handshake, 0, sizeof snapshot->handshake);
    }

    store->pending_flags = MESH_UI_UPDATE_NONE;
    return true;
}
