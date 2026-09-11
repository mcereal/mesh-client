#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "mesh/transport/stream_link.h"

#include "mesh/utils/log.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define MESH_STREAM_LINK_READ_CHUNK 1024U
/* Reads per event-loop turn. A NodeDB sync arrives as a burst; bound it so UI input still flows. */
#define MESH_STREAM_LINK_READS_PER_TURN 8U

void mesh_stream_link_init(struct mesh_stream_link *link, const char *tag,
                           struct mesh_session *session) {
    if (link == NULL) {
        return;
    }
    memset(link, 0, sizeof *link);
    link->fd = -1;
    link->tag = tag != NULL ? tag : "link";
    link->session = session;
    mesh_stream_parser_reset(&link->parser);
}

void mesh_stream_link_set_session(struct mesh_stream_link *link, struct mesh_session *session) {
    if (link != NULL) {
        link->session = session;
    }
}

bool mesh_stream_link_is_open(const struct mesh_stream_link *link) {
    return link != NULL && link->fd >= 0;
}

/* ------------------------------------------------------------------ write queue */

static void mesh_stream_link_clear_write_queue(struct mesh_stream_link *link) {
    for (size_t i = 0; i < link->write_queue_len; ++i) {
        const size_t index = (link->write_queue_head + i) % MESH_STREAM_LINK_MAX_OUTBOUND;
        const uint32_t packet_id = link->write_queue[index].packet_id;
        if (packet_id != 0U) {
            mesh_session_packet_failed(link->session, packet_id);
        }
    }
    link->write_queue_head = 0U;
    link->write_queue_len = 0U;
}

static int mesh_stream_link_queue_packet(struct mesh_stream_link *link, const uint8_t *packet,
                                         size_t len, uint32_t packet_id) {
    if (link->write_queue_len >= MESH_STREAM_LINK_MAX_OUTBOUND) {
        return -ENOSPC;
    }

    const size_t index =
        (link->write_queue_head + link->write_queue_len) % MESH_STREAM_LINK_MAX_OUTBOUND;
    struct mesh_stream_link_packet *slot = &link->write_queue[index];
    size_t written = 0U;
    const int encoded =
        mesh_stream_frame_encode(packet, len, slot->data, sizeof slot->data, &written);
    if (encoded < 0) {
        return encoded;
    }

    slot->length = written;
    slot->sent = 0U;
    slot->packet_id = packet_id;
    link->write_queue_len += 1U;
    return 0;
}

/* Keeps EPOLLOUT armed exactly while the queue has a remainder, so a descriptor that filled up
   wakes the loop instead of waiting out the poll timeout. */
static void mesh_stream_link_update_write_interest(struct mesh_stream_link *link) {
    if (!link->fd_registered || link->loop == NULL) {
        return;
    }
    const bool want = link->write_queue_len > 0U;
    if (want == link->want_write) {
        return;
    }
    const uint32_t events = want ? (uint32_t)(EPOLLIN | EPOLLOUT) : (uint32_t)EPOLLIN;
    if (mesh_event_loop_update_fd(link->loop, link->fd, events) == 0) {
        link->want_write = want;
    }
}

int mesh_stream_link_flush(struct mesh_stream_link *link) {
    if (link == NULL) {
        return -EINVAL;
    }
    if (link->fd < 0) {
        return -ENOTCONN;
    }

    while (link->write_queue_len > 0U) {
        struct mesh_stream_link_packet *slot = &link->write_queue[link->write_queue_head];
        const ssize_t written = write(link->fd, slot->data + slot->sent, slot->length - slot->sent);
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; /* the far end is not draining; EPOLLOUT brings us back */
            }
            if (errno == EINTR) {
                continue;
            }
            mesh_log_warn(link->tag, "write failed: %s", strerror(errno));
            return -EIO;
        }

        slot->sent += (size_t)written;
        if (slot->sent < slot->length) {
            break;
        }
        link->write_queue_head = (link->write_queue_head + 1U) % MESH_STREAM_LINK_MAX_OUTBOUND;
        link->write_queue_len -= 1U;
    }

    mesh_stream_link_update_write_interest(link);
    return 0;
}

int mesh_stream_link_send(struct mesh_stream_link *link, const uint8_t *packet, size_t len,
                          uint32_t packet_id) {
    if (link == NULL) {
        return -EINVAL;
    }
    if (link->fd < 0) {
        return -ENOTCONN;
    }

    const int queued = mesh_stream_link_queue_packet(link, packet, len, packet_id);
    if (queued < 0) {
        return queued;
    }
    mesh_stream_link_update_write_interest(link);
    return mesh_stream_link_flush(link);
}

int mesh_stream_link_write_raw(struct mesh_stream_link *link, const uint8_t *data, size_t len) {
    if (link == NULL || data == NULL) {
        return -EINVAL;
    }
    if (link->fd < 0) {
        return -ENOTCONN;
    }
    const ssize_t written = write(link->fd, data, len);
    if (written < 0) {
        return -errno;
    }
    return (int)written;
}

/* ------------------------------------------------------------------ read path */

static void mesh_stream_link_on_frame(const uint8_t *payload, size_t len, void *ctx) {
    struct mesh_stream_link *link = (struct mesh_stream_link *)ctx;
    link->frames_received += 1U;
    mesh_session_handle_from_radio(link->session, payload, len);
}

/* Whatever sits between frames is the radio's own log. Surface it at debug, one line at a time,
   with control bytes stripped so it cannot scribble on the terminal. */
static void mesh_stream_link_on_text(const uint8_t *text, size_t len, void *ctx) {
    struct mesh_stream_link *link = (struct mesh_stream_link *)ctx;
    char line[160];
    size_t out = 0U;
    for (size_t i = 0; i < len; ++i) {
        const uint8_t byte = text[i];
        if (byte == '\n' || byte == '\r') {
            if (out > 0U) {
                line[out] = '\0';
                mesh_log_debug(link->tag, "radio: %s", line);
                out = 0U;
            }
            continue;
        }
        if (out + 1U >= sizeof line) {
            line[out] = '\0';
            mesh_log_debug(link->tag, "radio: %s", line);
            out = 0U;
        }
        line[out++] = (byte >= 0x20U && byte < 0x7FU) ? (char)byte : '.';
    }
    if (out > 0U) {
        line[out] = '\0';
        mesh_log_debug(link->tag, "radio: %s", line);
    }
}

int mesh_stream_link_pump(struct mesh_stream_link *link) {
    if (link == NULL) {
        return -EINVAL;
    }
    if (link->fd < 0) {
        return -ENOTCONN;
    }

    const struct mesh_stream_parser_callbacks callbacks = {
        .on_frame = mesh_stream_link_on_frame,
        .on_text = mesh_stream_link_on_text,
        .ctx = link,
    };

    size_t total = 0U;
    for (unsigned turn = 0U; turn < MESH_STREAM_LINK_READS_PER_TURN; ++turn) {
        uint8_t buffer[MESH_STREAM_LINK_READ_CHUNK];
        const ssize_t got = read(link->fd, buffer, sizeof buffer);
        if (got > 0) {
            total += (size_t)got;
            link->bytes_received += (size_t)got;
            mesh_stream_parser_push(&link->parser, buffer, (size_t)got, &callbacks);
            continue;
        }
        if (got == 0) {
            /* A tty does not normally report EOF, so on serial this is the node unplugged; on a
               socket it is the ordinary way a far end says it has gone. */
            return -ENOTCONN;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        mesh_log_warn(link->tag, "read failed: %s", strerror(errno));
        return -EIO;
    }

    return (int)total;
}

/* ------------------------------------------------------------------ lifecycle */

int mesh_stream_link_open(struct mesh_stream_link *link, int fd, struct mesh_event_loop *loop,
                          mesh_event_callback callback, void *userdata) {
    if (link == NULL || fd < 0) {
        return -EINVAL;
    }
    if (link->fd >= 0) {
        return -EBUSY;
    }

    if (loop != NULL) {
        const int added = mesh_event_loop_add_fd(loop, fd, EPOLLIN, callback, userdata);
        if (added < 0) {
            return added;
        }
        link->fd_registered = true;
    } else {
        link->fd_registered = false;
    }

    link->fd = fd;
    link->loop = loop;
    link->want_write = false;
    mesh_stream_parser_reset(&link->parser);
    link->frames_received = 0U;
    link->bytes_received = 0U;
    link->write_queue_head = 0U;
    link->write_queue_len = 0U;
    return 0;
}

void mesh_stream_link_close(struct mesh_stream_link *link) {
    if (link == NULL) {
        return;
    }
    /* The queue is cleared even on an already-closed link: a transport that failed between
       queueing and opening still owes those packets a verdict. */
    mesh_stream_link_clear_write_queue(link);
    mesh_stream_parser_reset(&link->parser);
    if (link->fd < 0) {
        return;
    }
    if (link->fd_registered && link->loop != NULL) {
        mesh_event_loop_remove_fd(link->loop, link->fd);
    }
    close(link->fd);
    link->fd = -1;
    link->fd_registered = false;
    link->want_write = false;
}

struct mesh_stream_link_stats mesh_stream_link_stats(const struct mesh_stream_link *link) {
    struct mesh_stream_link_stats stats = {0U, 0U, 0U};
    if (link == NULL) {
        return stats;
    }
    stats.frames_received = link->frames_received;
    stats.bytes_received = link->bytes_received;
    stats.junk_bytes = link->parser.dropped_bytes;
    return stats;
}
