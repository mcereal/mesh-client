#define _GNU_SOURCE

#include "support/https_fixture.h"

#ifdef INKWELL_HAVE_TLS

#include "inkwell/net/fetch.h"

#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ticket.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * The server's certificate and key, generated for this file and used nowhere else - published
 * here so the server can be stood up without a secret. Self-signed with CA:TRUE, so the one PEM
 * is both what the server presents and the whole bundle a client trusts; valid to 2126.
 */
static const char k_cert_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICETCCAbegAwIBAgIURQvOsuxOzFT6zEHgcOS9MhUweD4wCgYIKoZIzj0EAwIw\n"
    "ITEfMB0GA1UEAwwWbWVzaGNsaWVudC10ZXN0LXNlcnZlcjAgFw0yNjA5MTgxNTA3\n"
    "MzlaGA8yMTI2MDgyNTE1MDczOVowITEfMB0GA1UEAwwWbWVzaGNsaWVudC10ZXN0\n"
    "LXNlcnZlcjBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABNQIc5dyCWxT9SXE02mE\n"
    "BUt0466/IgZiZCaWooXx+/L8wTkxX/xuiu3jmrJqyIplJi5/cGQHXurDZwC2iPQv\n"
    "KROjgcowgccwHQYDVR0OBBYEFLh0xqV2rsNVHQjWWC3E0oWEYBxUMB8GA1UdIwQY\n"
    "MBaAFLh0xqV2rsNVHQjWWC3E0oWEYBxUMHQGA1UdEQRtMGuCCWxvY2FsaG9zdIIK\n"
    "Z2l0aHViLmNvbYIOYXBpLmdpdGh1Yi5jb22CFyouZ2l0aHVidXNlcmNvbnRlbnQu\n"
    "Y29tghJhcGkubWVzaHRhc3RpYy5vcmeCD2V4YW1wbGUuaW52YWxpZIcEfwAAATAP\n"
    "BgNVHRMBAf8EBTADAQH/MAoGCCqGSM49BAMCA0gAMEUCIHZtqAs5wH7CbacJ4MQD\n"
    "PRpBUp3ZwcSYFankzTKHXKYIAiEA4ySdhLPBRn5ZAwN3nozrJtZDq/eQ2tCyOgPQ\n"
    "d2bJ7UY=\n"
    "-----END CERTIFICATE-----\n";

static const char k_key_pem[] = "-----BEGIN PRIVATE KEY-----\n"
                                "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQg33siDbgp76nmcI4P\n"
                                "crCvmnKOKCEQuUEQH0K4EAap8zOhRANCAATUCHOXcglsU/UlxNNphAVLdOOuvyIG\n"
                                "YmQmlqKF8fvy/ME5MV/8bort45qyasiKZSYuf3BkB17qw2cAtoj0LykT\n"
                                "-----END PRIVATE KEY-----\n";

struct https_fixture_conn {
    mbedtls_ssl_context *ssl;
    const struct https_fixture_request *request;
    bool cut;
};

/* Blocking both ways: the child has nothing else to do while it waits. */
static int fixture_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    const ssize_t written = send((int)(intptr_t)ctx, buf, len, MSG_NOSIGNAL);
    return written >= 0 ? (int)written : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int fixture_bio_recv(void *ctx, unsigned char *buf, size_t len) {
    const ssize_t got = recv((int)(intptr_t)ctx, buf, len, 0);
    if (got > 0) {
        return (int)got;
    }
    return got == 0 ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_NET_RECV_FAILED;
}

void https_fixture_send(struct https_fixture_conn *conn, const void *data, size_t len) {
    const unsigned char *at = (const unsigned char *)data;
    while (len > 0U && !conn->cut) {
        const int wrote = mbedtls_ssl_write(conn->ssl, at, len);
        if (wrote <= 0) {
            /* The client went away; nothing more on this connection will arrive. */
            conn->cut = true;
            return;
        }
        at += wrote;
        len -= (size_t)wrote;
    }
}

void https_fixture_printf(struct https_fixture_conn *conn, const char *format, ...) {
    char line[8192];
    va_list args;
    va_start(args, format);
    const int len = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (len > 0) {
        https_fixture_send(conn, line, (size_t)len < sizeof line ? (size_t)len : sizeof line - 1U);
    }
}

void https_fixture_reply(struct https_fixture_conn *conn, int status, const char *extra,
                         const void *body, size_t len) {
    https_fixture_printf(conn, "HTTP/1.1 %d Fixture\r\nContent-Length: %zu\r\n%s\r\n", status, len,
                         extra != NULL ? extra : "");
    if (strcmp(conn->request->method, "HEAD") != 0 && len > 0U) {
        https_fixture_send(conn, body, len);
    }
}

void https_fixture_reply_file(struct https_fixture_conn *conn,
                              const struct https_fixture_request *request, const char *path) {
    FILE *file = fopen(path, "rb");
    struct stat info;
    if (file == NULL || fstat(fileno(file), &info) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        https_fixture_reply(conn, 404, NULL, "missing", 7U);
        return;
    }
    const uint64_t size = (uint64_t)info.st_size;
    uint64_t first = 0U;
    uint64_t count = size;
    if (request->ranged) {
        if (request->first >= size || request->last < request->first) {
            fclose(file);
            https_fixture_reply(conn, 416, NULL, NULL, 0U);
            return;
        }
        first = request->first;
        count = (request->last < size ? request->last + 1U : size) - first;
    }
    char *body = malloc(count > 0U ? (size_t)count : 1U);
    if (body == NULL || fseek(file, (long)first, SEEK_SET) != 0 ||
        fread(body, 1U, (size_t)count, file) != (size_t)count) {
        free(body);
        fclose(file);
        https_fixture_reply(conn, 500, NULL, NULL, 0U);
        return;
    }
    fclose(file);
    if (request->ranged) {
        char range[96];
        snprintf(range, sizeof range, "Content-Range: bytes %llu-%llu/%llu\r\n",
                 (unsigned long long)first, (unsigned long long)(first + count - 1U),
                 (unsigned long long)size);
        https_fixture_reply(conn, 206, range, body, (size_t)count);
    } else {
        https_fixture_reply(conn, 200, "Accept-Ranges: bytes\r\n", body, (size_t)count);
    }
    free(body);
}

void https_fixture_cut(struct https_fixture_conn *conn) { conn->cut = true; }

/* ---- the child ------------------------------------------------------------------------- */

/* Case-blind: which header a line is. Returns the value, trimmed at the front, or NULL. */
static const char *fixture_header(const char *line, const char *name) {
    const size_t len = strlen(name);
    if (strncasecmp(line, name, len) != 0 || line[len] != ':') {
        return NULL;
    }
    const char *value = line + len + 1U;
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    return value;
}

static bool fixture_parse(char *head, struct https_fixture_request *request) {
    memset(request, 0, sizeof *request);
    char *line = strtok(head, "\r\n");
    if (line == NULL || sscanf(line, "%7s %4095s", request->method, request->target) != 2) {
        return false;
    }
    while ((line = strtok(NULL, "\r\n")) != NULL) {
        const char *value = fixture_header(line, "host");
        if (value != NULL) {
            snprintf(request->host, sizeof request->host, "%s", value);
            char *const colon = strrchr(request->host, ':');
            if (colon != NULL && request->host[0] != '[') {
                *colon = '\0';
            }
            continue;
        }
        value = fixture_header(line, "range");
        unsigned long long first = 0U;
        unsigned long long last = 0U;
        if (value != NULL && sscanf(value, "bytes=%llu-%llu", &first, &last) == 2) {
            request->ranged = true;
            request->first = first;
            request->last = last;
        }
    }
    return true;
}

static void fixture_log(const char *path, const struct https_fixture_request *request) {
    FILE *log = fopen(path, "a");
    if (log == NULL) {
        return;
    }
    if (request->ranged) {
        fprintf(log, "%s %s %s %llu-%llu\n", request->method, request->host, request->target,
                (unsigned long long)request->first, (unsigned long long)request->last);
    } else {
        fprintf(log, "%s %s %s\n", request->method, request->host, request->target);
    }
    fclose(log);
}

static void fixture_serve(mbedtls_ssl_context *ssl, int fd, const char *log_path,
                          https_fixture_handler handler, void *userdata) {
    mbedtls_ssl_set_bio(ssl, (void *)(intptr_t)fd, fixture_bio_send, fixture_bio_recv, NULL);
    if (mbedtls_ssl_handshake(ssl) != 0) {
        return; /* the client refused us, which a case may have wanted */
    }
    static char head[16384];
    size_t len = 0U;
    while (len + 1U < sizeof head) {
        const int got = mbedtls_ssl_read(ssl, (unsigned char *)head + len, sizeof head - 1U - len);
        if (got <= 0) {
            return;
        }
        len += (size_t)got;
        head[len] = '\0';
        if (strstr(head, "\r\n\r\n") != NULL) {
            break;
        }
    }
    static struct https_fixture_request request;
    if (!fixture_parse(head, &request)) {
        return;
    }
    fixture_log(log_path, &request);
    struct https_fixture_conn conn = {.ssl = ssl, .request = &request, .cut = false};
    handler(userdata, &request, &conn);
    if (!conn.cut) {
        (void)mbedtls_ssl_close_notify(ssl);
    }
}

static void fixture_child(int listen_fd, const char *log_path, https_fixture_handler handler,
                          void *userdata) {
    /* Gone with the suite, whatever happens to it. */
    (void)prctl(PR_SET_PDEATHSIG, SIGKILL);

    mbedtls_ssl_config conf;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
    mbedtls_ssl_ticket_context ticket;
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&cert);
    mbedtls_pk_init(&key);
    mbedtls_ssl_ticket_init(&ticket);
    if (psa_crypto_init() != PSA_SUCCESS ||
        mbedtls_x509_crt_parse(&cert, (const unsigned char *)k_cert_pem, sizeof k_cert_pem) != 0 ||
        mbedtls_pk_parse_key(&key, (const unsigned char *)k_key_pem, sizeof k_key_pem, NULL, 0U) !=
            0 ||
        mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
        mbedtls_ssl_conf_own_cert(&conf, &cert, &key) != 0) {
        _exit(2);
    }
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    /* A session ticket after the handshake, because GitHub sends one and the client has to read
       past it on the same stream as the reply. */
    if (mbedtls_ssl_ticket_setup(&ticket, PSA_ALG_GCM, PSA_KEY_TYPE_AES, 256U, 86400U) == 0) {
        mbedtls_ssl_conf_session_tickets_cb(&conf, mbedtls_ssl_ticket_write,
                                            mbedtls_ssl_ticket_parse, &ticket);
    }

    for (;;) {
        const int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            _exit(3);
        }
        mbedtls_ssl_context ssl;
        mbedtls_ssl_init(&ssl);
        if (mbedtls_ssl_setup(&ssl, &conf) == 0) {
            fixture_serve(&ssl, fd, log_path, handler, userdata);
        }
        mbedtls_ssl_free(&ssl);
        close(fd);
    }
}

/* ---- the suite's side ------------------------------------------------------------------ */

bool https_fixture_start(struct https_fixture *fixture, https_fixture_handler handler,
                         void *userdata) {
    memset(fixture, 0, sizeof *fixture);
    fixture->child = -1;
    fixture->listen_fd = -1;
    snprintf(fixture->ca_path, sizeof fixture->ca_path, "/tmp/meshclient-https-%d.pem",
             (int)getpid());
    snprintf(fixture->log_path, sizeof fixture->log_path, "/tmp/meshclient-https-%d.log",
             (int)getpid());
    (void)unlink(fixture->log_path);

    FILE *pem = fopen(fixture->ca_path, "w");
    if (pem == NULL) {
        return false;
    }
    const bool written = fputs(k_cert_pem, pem) >= 0;
    if (fclose(pem) != 0 || !written) {
        return false;
    }

    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    fixture->listen_fd = fd;
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t address_len = (socklen_t)sizeof address;
    if (bind(fd, (const struct sockaddr *)&address, sizeof address) != 0 || listen(fd, 8) != 0 ||
        getsockname(fd, (struct sockaddr *)&address, &address_len) != 0) {
        return false;
    }
    fixture->port = ntohs(address.sin_port);

    fflush(NULL);
    const pid_t child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        fixture_child(fd, fixture->log_path, handler, userdata);
        _exit(0);
    }
    fixture->child = child;
    setenv("SSL_CERT_FILE", fixture->ca_path, 1);
    return true;
}

void https_fixture_stop(struct https_fixture *fixture) {
    unsetenv("SSL_CERT_FILE");
    /* A zeroed fixture that was never started names no file, and owns no descriptor 0. */
    if (fixture->ca_path[0] == '\0') {
        return;
    }
    if (fixture->child > 0) {
        kill(fixture->child, SIGKILL);
        (void)waitpid(fixture->child, NULL, 0);
        fixture->child = -1;
    }
    if (fixture->listen_fd >= 0) {
        close(fixture->listen_fd);
        fixture->listen_fd = -1;
    }
    if (fixture->ca_path[0] != '\0') {
        (void)unlink(fixture->ca_path);
    }
    if (fixture->log_path[0] != '\0') {
        (void)unlink(fixture->log_path);
    }
}

const char *https_fixture_cert_pem(void) { return k_cert_pem; }

void https_fixture_attach(const struct https_fixture *fixture, struct inkwell_fetch *fetch) {
    inkwell_fetch_connect_to(fetch, "127.0.0.1", fixture->port);
}

size_t https_fixture_requests(const struct https_fixture *fixture, char *out, size_t cap) {
    if (cap == 0U) {
        return 0U;
    }
    out[0] = '\0';
    FILE *log = fopen(fixture->log_path, "r");
    if (log == NULL) {
        return 0U;
    }
    const size_t len = fread(out, 1U, cap - 1U, log);
    fclose(log);
    out[len] = '\0';
    return len;
}

#endif /* INKWELL_HAVE_TLS */
