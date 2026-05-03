/* sw/webui/cgi_common.c — shared CGI thin-client implementation.
 *
 * Owned by S6 (S6-sw-webui-cgis). See include/tetra/cgi_common.h for
 * the public contract.
 *
 * Wire format: docs/OPERATIONS.md §6 — 4-byte BE length prefix, then
 * UTF-8 JSON envelope, both directions over /run/tetra_d.sock.
 *
 * No jansson dependency: requests build a small JSON envelope by hand
 * (op-name, args-passthrough, req_id, client tag — all strings we
 * already know to be safe to embed) and responses are forwarded
 * verbatim. The daemon (S7) owns full JSON parse.
 */
#define _POSIX_C_SOURCE 200809L

#include "tetra/cgi_common.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * tiny utility helpers
 * ------------------------------------------------------------------------- */

static long now_ms_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec) * 1000L + (long)(ts.tv_nsec / 1000000L);
}

void cgi_pack_len_be(uint8_t out[4], uint32_t len)
{
    out[0] = (uint8_t)((len >> 24) & 0xFFu);
    out[1] = (uint8_t)((len >> 16) & 0xFFu);
    out[2] = (uint8_t)((len >>  8) & 0xFFu);
    out[3] = (uint8_t)((len >>  0) & 0xFFu);
}

uint32_t cgi_unpack_len_be(const uint8_t in[4])
{
    return ((uint32_t)in[0] << 24)
         | ((uint32_t)in[1] << 16)
         | ((uint32_t)in[2] <<  8)
         | ((uint32_t)in[3] <<  0);
}

bool cgi_op_allowed(const CgiRunOpts *opts, const char *op)
{
    if (opts == NULL || op == NULL || opts->allowed_ops == NULL) {
        return false;
    }
    for (size_t i = 0; opts->allowed_ops[i] != NULL; ++i) {
        if (strcmp(opts->allowed_ops[i], op) == 0) {
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * raw I/O helpers
 * ------------------------------------------------------------------------- */

int cgi_write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t left = len;
    while (left > 0) {
        const ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    return 0;
}

ssize_t cgi_read_atleast(int fd,
                         void *buf,
                         size_t min_bytes,
                         size_t max_bytes,
                         long deadline_ms_epoch)
{
    if (max_bytes < min_bytes) {
        errno = EINVAL;
        return -1;
    }
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < min_bytes) {
        const long now = now_ms_monotonic();
        const long left_ms = deadline_ms_epoch - now;
        if (left_ms <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        struct timeval tv;
        tv.tv_sec = left_ms / 1000;
        tv.tv_usec = (left_ms % 1000) * 1000;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        const int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (sel == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        const ssize_t n = read(fd, p + got, max_bytes - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        got += (size_t)n;
    }
    return (ssize_t)got;
}

/* ---------------------------------------------------------------------------
 * tiny JSON-string escape (ASCII only, no UTF-16 surrogate pairs).
 * Sufficient for op-names, req_ids, client tags, and short error
 * messages we generate ourselves. Long-haul JSON goes through the
 * daemon, not this code path.
 * ------------------------------------------------------------------------- */
static int json_escape(char *out, size_t cap, const char *in)
{
    size_t w = 0;
    for (size_t i = 0; in != NULL && in[i] != '\0'; ++i) {
        const unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (w + 2 >= cap) return -1;
            out[w++] = '\\';
            out[w++] = (char)c;
        } else if (c == '\n') {
            if (w + 2 >= cap) return -1;
            out[w++] = '\\'; out[w++] = 'n';
        } else if (c == '\r') {
            if (w + 2 >= cap) return -1;
            out[w++] = '\\'; out[w++] = 'r';
        } else if (c == '\t') {
            if (w + 2 >= cap) return -1;
            out[w++] = '\\'; out[w++] = 't';
        } else if (c < 0x20u) {
            if (w + 6 >= cap) return -1;
            const int n = snprintf(out + w, cap - w, "\\u%04x", (unsigned)c);
            if (n < 0 || (size_t)n >= cap - w) return -1;
            w += (size_t)n;
        } else {
            if (w + 1 >= cap) return -1;
            out[w++] = (char)c;
        }
    }
    if (w >= cap) return -1;
    out[w] = '\0';
    return (int)w;
}

int cgi_build_request(char *out_buf,
                      size_t out_cap,
                      const char *op,
                      const char *args_json,
                      const char *req_id,
                      const char *client_tag)
{
    if (out_buf == NULL || out_cap < 16 || op == NULL) {
        return -1;
    }
    char op_esc[256];
    char rid_esc[64];
    char tag_esc[128];
    if (json_escape(op_esc, sizeof(op_esc), op) < 0) return -1;
    if (json_escape(rid_esc, sizeof(rid_esc),
                    req_id != NULL ? req_id : "") < 0) return -1;
    if (json_escape(tag_esc, sizeof(tag_esc),
                    client_tag != NULL ? client_tag : "cgi") < 0) return -1;

    const char *args = (args_json != NULL && args_json[0] != '\0')
                       ? args_json : "{}";

    int n;
    if (req_id != NULL && req_id[0] != '\0') {
        n = snprintf(out_buf, out_cap,
                     "{\"op\":\"%s\",\"args\":%s,\"req_id\":\"%s\","
                     "\"client\":\"%s\"}",
                     op_esc, args, rid_esc, tag_esc);
    } else {
        n = snprintf(out_buf, out_cap,
                     "{\"op\":\"%s\",\"args\":%s,\"client\":\"%s\"}",
                     op_esc, args, tag_esc);
    }
    if (n < 0 || (size_t)n >= out_cap) return -1;
    return n;
}

/* ---------------------------------------------------------------------------
 * URL-decode in place. Returns the new length.
 * Standard application/x-www-form-urlencoded: '+' → space, %HH → byte.
 * ------------------------------------------------------------------------- */
static size_t url_decode_inplace(char *s)
{
    char *r = s;
    char *w = s;
    while (*r != '\0') {
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%' && isxdigit((unsigned char)r[1])
                             && isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
    return (size_t)(w - s);
}

/* Find key=value in a query-string. Returns a malloc'd, URL-decoded
 * value, or NULL if the key isn't present. Caller frees. */
static char *qs_extract(const char *qs, const char *key)
{
    if (qs == NULL || key == NULL) return NULL;
    const size_t klen = strlen(key);
    const char *p = qs;
    while (*p != '\0') {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        if (eq == NULL) break;
        const size_t name_len = (size_t)(eq - p);
        if (name_len == klen && strncmp(p, key, klen) == 0) {
            const char *vstart = eq + 1;
            const char *vend = (amp != NULL) ? amp : (vstart + strlen(vstart));
            const size_t vlen = (size_t)(vend - vstart);
            char *out = (char *)malloc(vlen + 1);
            if (out == NULL) return NULL;
            memcpy(out, vstart, vlen);
            out[vlen] = '\0';
            url_decode_inplace(out);
            return out;
        }
        if (amp == NULL) break;
        p = amp + 1;
    }
    return NULL;
}

/* Convert a query string into a JSON object literal. This is a
 * coarse-grained shim: every key=value becomes "key":"value" (string
 * type). The daemon's per-op handler is responsible for re-coercing
 * to int/bool/etc. — the wire spec in OPERATIONS.md §6 does not
 * promise types here, only that args is an object.
 *
 * The "op" key is filtered out (it is hoisted to the envelope). */
static int qs_to_args_json(const char *qs, char *out, size_t cap)
{
    if (cap < 3) return -1;
    size_t w = 0;
    out[w++] = '{';
    bool first = true;
    const char *p = (qs != NULL) ? qs : "";
    while (*p != '\0') {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        if (eq == NULL) break;
        const size_t name_len = (size_t)(eq - p);
        const char *vstart = eq + 1;
        const char *vend = (amp != NULL) ? amp : (vstart + strlen(vstart));
        const size_t vlen = (size_t)(vend - vstart);

        if (!(name_len == 2 && strncmp(p, "op", 2) == 0)) {
            char name_buf[64];
            char val_buf[256];
            if (name_len >= sizeof(name_buf) || vlen >= sizeof(val_buf)) {
                return -1;
            }
            memcpy(name_buf, p, name_len);
            name_buf[name_len] = '\0';
            memcpy(val_buf, vstart, vlen);
            val_buf[vlen] = '\0';
            url_decode_inplace(name_buf);
            url_decode_inplace(val_buf);

            char name_esc[128];
            char val_esc[512];
            if (json_escape(name_esc, sizeof(name_esc), name_buf) < 0) return -1;
            if (json_escape(val_esc,  sizeof(val_esc),  val_buf)  < 0) return -1;

            const int n = snprintf(out + w, cap - w,
                                   "%s\"%s\":\"%s\"",
                                   first ? "" : ",",
                                   name_esc, val_esc);
            if (n < 0 || (size_t)n >= cap - w) return -1;
            w += (size_t)n;
            first = false;
        }
        if (amp == NULL) break;
        p = amp + 1;
    }
    if (w + 2 > cap) return -1;
    out[w++] = '}';
    out[w] = '\0';
    return (int)w;
}

/* ---------------------------------------------------------------------------
 * Output helpers
 * ------------------------------------------------------------------------- */

static const char *http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 413: return "Payload Too Large";
    case 502: return "Bad Gateway";
    case 504: return "Gateway Timeout";
    default:  return "OK";
    }
}

int cgi_emit_error(int fd,
                   int status_http,
                   const char *code,
                   const char *message)
{
    if (code == NULL)    code = "EINTERNAL";
    if (message == NULL) message = "";

    char code_esc[64];
    char msg_esc[256];
    if (json_escape(code_esc, sizeof(code_esc), code) < 0) return -1;
    if (json_escape(msg_esc,  sizeof(msg_esc),  message) < 0) return -1;

    char body[512];
    const int blen = snprintf(body, sizeof(body),
        "{\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
        code_esc, msg_esc);
    if (blen < 0 || (size_t)blen >= sizeof(body)) return -1;

    char hdr[256];
    const int hlen = snprintf(hdr, sizeof(hdr),
        "Status: %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "\r\n",
        status_http, http_reason(status_http), blen);
    if (hlen < 0 || (size_t)hlen >= sizeof(hdr)) return -1;

    if (cgi_write_all(fd, hdr, (size_t)hlen) < 0) return -1;
    if (cgi_write_all(fd, body, (size_t)blen) < 0) return -1;
    return 0;
}

int cgi_emit_response(int fd, const void *body, size_t body_len)
{
    char hdr[256];
    const int hlen = snprintf(hdr, sizeof(hdr),
        "Status: 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "\r\n",
        body_len);
    if (hlen < 0 || (size_t)hlen >= sizeof(hdr)) return -1;
    if (cgi_write_all(fd, hdr, (size_t)hlen) < 0) return -1;
    if (cgi_write_all(fd, body, body_len) < 0) return -1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Connect to the daemon Unix socket. Honours $TETRA_D_SOCK override
 * for tests (the harness pre-binds an AF_UNIX socket and exports the
 * env var so the CGI doesn't reach for /run/tetra_d.sock).
 * ------------------------------------------------------------------------- */
static int connect_daemon(const char *path_in, long deadline_ms_epoch)
{
    const char *path = path_in;
    const char *env = getenv("TETRA_D_SOCK");
    if (env != NULL && env[0] != '\0') path = env;
    if (path == NULL || path[0] == '\0') path = CGI_DAEMON_SOCK_DEFAULT;

    const long left_ms = deadline_ms_epoch - now_ms_monotonic();
    if (left_ms <= 0) { errno = ETIMEDOUT; return -1; }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    /* Best-effort connect-timeout via NONBLOCK + select. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (rc < 0) {
        struct timeval tv;
        tv.tv_sec = left_ms / 1000;
        tv.tv_usec = (left_ms % 1000) * 1000;
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        const int sel = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (sel <= 0) {
            close(fd);
            errno = (sel == 0) ? ETIMEDOUT : errno;
            return -1;
        }
        int err = 0;
        socklen_t err_len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) < 0
            || err != 0) {
            close(fd);
            errno = (err != 0) ? err : EIO;
            return -1;
        }
    }
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags);
    return fd;
}

/* ---------------------------------------------------------------------------
 * Read CONTENT_LENGTH bytes of stdin into a malloc'd buffer.
 * Returns NULL with errno on failure (errno=E2BIG if max exceeded,
 * EIO for short read). Caller frees.
 * ------------------------------------------------------------------------- */
static char *slurp_stdin(size_t max_bytes,
                         size_t *out_len,
                         long deadline_ms_epoch)
{
    *out_len = 0;
    const char *cl = getenv("CONTENT_LENGTH");
    if (cl == NULL || cl[0] == '\0') {
        char *empty = (char *)malloc(1);
        if (empty == NULL) return NULL;
        empty[0] = '\0';
        return empty;
    }
    char *endp = NULL;
    const unsigned long want = strtoul(cl, &endp, 10);
    if (endp == NULL || endp == cl) {
        errno = EINVAL;
        return NULL;
    }
    if (want > max_bytes) {
        errno = E2BIG;
        return NULL;
    }
    char *buf = (char *)malloc(want + 1);
    if (buf == NULL) return NULL;
    if (want > 0) {
        const ssize_t n = cgi_read_atleast(STDIN_FILENO, buf,
                                           want, want,
                                           deadline_ms_epoch);
        if (n < 0 || (size_t)n != want) {
            free(buf);
            return NULL;
        }
    }
    buf[want] = '\0';
    *out_len = want;
    return buf;
}

/* ---------------------------------------------------------------------------
 * Pull "op":"NAME" out of a JSON-ish body. Cheap: we look for the
 * literal pattern  "op"  optional-WS  :  optional-WS  "..."  and copy
 * the contents of the string. This is good enough for the well-known
 * CGI POST shape; daemon does the strict parse.
 * ------------------------------------------------------------------------- */
static char *json_extract_op(const char *body, size_t body_len)
{
    if (body == NULL || body_len < 6) return NULL;
    const char *p = body;
    const char *end = body + body_len;
    while (p < end - 4) {
        if ((p == body || !isalnum((unsigned char)p[-1]))
            && p[0] == '"' && p[1] == 'o' && p[2] == 'p' && p[3] == '"') {
            const char *q = p + 4;
            while (q < end && isspace((unsigned char)*q)) q++;
            if (q >= end || *q != ':') { p++; continue; }
            q++;
            while (q < end && isspace((unsigned char)*q)) q++;
            if (q >= end || *q != '"') { p++; continue; }
            q++;
            const char *vstart = q;
            while (q < end && *q != '"') {
                if (*q == '\\' && (q + 1) < end) q++;
                q++;
            }
            if (q >= end) return NULL;
            const size_t vlen = (size_t)(q - vstart);
            char *out = (char *)malloc(vlen + 1);
            if (out == NULL) return NULL;
            memcpy(out, vstart, vlen);
            out[vlen] = '\0';
            return out;
        }
        p++;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Public: cgi_run
 * ------------------------------------------------------------------------- */

int cgi_run(const CgiRunOpts *opts)
{
    if (opts == NULL || opts->script_name == NULL
        || opts->allowed_ops == NULL) {
        cgi_emit_error(STDOUT_FILENO, 500, "EINTERNAL",
                       "cgi_run: bad opts");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    const int deadline_ms = (opts->deadline_ms > 0)
                            ? opts->deadline_ms : CGI_DEADLINE_MS;
    const long deadline_epoch = now_ms_monotonic() + deadline_ms;
    const size_t max_body = (opts->max_body_bytes > 0)
                            ? opts->max_body_bytes : CGI_DEFAULT_MAX_BODY;

    const char *method = getenv("REQUEST_METHOD");
    const char *qs = getenv("QUERY_STRING");
    if (method == NULL) method = "GET";
    if (qs == NULL) qs = "";

    /* 1. Slurp body if POST-like. */
    char *body = NULL;
    size_t body_len = 0;
    const bool has_body = (strcmp(method, "POST") == 0
                          || strcmp(method, "PUT")  == 0);
    if (has_body) {
        body = slurp_stdin(max_body, &body_len, deadline_epoch);
        if (body == NULL) {
            const int code = (errno == E2BIG) ? 413 : 400;
            const char *ec = (errno == E2BIG) ? "E2BIG" : "EINVAL";
            cgi_emit_error(STDOUT_FILENO, code, ec, "stdin slurp failed");
            return 0;
        }
    }

    /* 2. Resolve op-name. Body JSON wins over query, query wins over
     *    fallback. */
    char *op = NULL;
    if (body != NULL && body_len > 0) {
        op = json_extract_op(body, body_len);
    }
    if (op == NULL) {
        op = qs_extract(qs, "op");
    }
    if (op == NULL && opts->fallback_op != NULL) {
        op = strdup(opts->fallback_op);
    }
    if (op == NULL) {
        cgi_emit_error(STDOUT_FILENO, 400, "EINVAL", "missing op");
        free(body);
        return 0;
    }

    /* 3. Allow-list check. */
    if (!cgi_op_allowed(opts, op)) {
        cgi_emit_error(STDOUT_FILENO, 403, "FORBIDDEN_OP",
                       "op not in CGI allow-list");
        free(op);
        free(body);
        return 0;
    }

    /* 4. Build request envelope.
     *    POST → body is already JSON; we fold it into args by passing
     *           body bytes verbatim if it parses as an object.
     *    GET  → query string → args object. */
    char args_json[2048];
    args_json[0] = '\0';
    if (has_body && body_len > 0) {
        /* Trim whitespace; if body[0]=='{' assume JSON object. */
        size_t s = 0;
        while (s < body_len && isspace((unsigned char)body[s])) s++;
        if (s < body_len && body[s] == '{') {
            if (body_len - s + 1 > sizeof(args_json)) {
                cgi_emit_error(STDOUT_FILENO, 413, "E2BIG", "body too large");
                free(op); free(body);
                return 0;
            }
            memcpy(args_json, body + s, body_len - s);
            args_json[body_len - s] = '\0';
        } else {
            /* treat as URL-encoded form fallback */
            if (qs_to_args_json((char *)body, args_json,
                                sizeof(args_json)) < 0) {
                cgi_emit_error(STDOUT_FILENO, 413, "E2BIG",
                               "form too large");
                free(op); free(body);
                return 0;
            }
        }
    } else {
        if (qs_to_args_json(qs, args_json, sizeof(args_json)) < 0) {
            cgi_emit_error(STDOUT_FILENO, 413, "E2BIG", "query too large");
            free(op); free(body);
            return 0;
        }
    }

    char client_tag[64];
    snprintf(client_tag, sizeof(client_tag), "cgi:%s", opts->script_name);

    char req_buf[CGI_DEFAULT_MAX_BODY];
    const int req_len = cgi_build_request(req_buf, sizeof(req_buf),
                                          op, args_json, NULL, client_tag);
    if (req_len < 0) {
        cgi_emit_error(STDOUT_FILENO, 413, "E2BIG", "request too large");
        free(op); free(body);
        return 0;
    }

    /* 5. Connect + length-prefixed exchange. */
    const int sfd = connect_daemon(opts->sock_path, deadline_epoch);
    if (sfd < 0) {
        cgi_emit_error(STDOUT_FILENO, 502, "EINTERNAL",
                       "daemon socket connect failed");
        free(op); free(body);
        return 0;
    }

    uint8_t lp[4];
    cgi_pack_len_be(lp, (uint32_t)req_len);
    if (cgi_write_all(sfd, lp, 4) < 0
        || cgi_write_all(sfd, req_buf, (size_t)req_len) < 0) {
        cgi_emit_error(STDOUT_FILENO, 502, "EINTERNAL",
                       "request write failed");
        close(sfd); free(op); free(body);
        return 0;
    }

    uint8_t rlp[4];
    if (cgi_read_atleast(sfd, rlp, 4, 4, deadline_epoch) < 0) {
        const int http = (errno == ETIMEDOUT) ? 504 : 502;
        const char *code = (errno == ETIMEDOUT) ? "EINTERNAL" : "EINTERNAL";
        cgi_emit_error(STDOUT_FILENO, http, code,
                       "response header read failed");
        close(sfd); free(op); free(body);
        return 0;
    }
    const uint32_t rlen = cgi_unpack_len_be(rlp);
    if (rlen == 0 || rlen > CGI_ENVELOPE_MAX) {
        cgi_emit_error(STDOUT_FILENO, 502, "EINTERNAL",
                       "response length invalid");
        close(sfd); free(op); free(body);
        return 0;
    }
    char *resp = (char *)malloc(rlen);
    if (resp == NULL) {
        cgi_emit_error(STDOUT_FILENO, 502, "EINTERNAL", "oom");
        close(sfd); free(op); free(body);
        return 0;
    }
    if (cgi_read_atleast(sfd, resp, rlen, rlen, deadline_epoch) < 0) {
        const int http = (errno == ETIMEDOUT) ? 504 : 502;
        cgi_emit_error(STDOUT_FILENO, http, "EINTERNAL",
                       "response body read failed");
        close(sfd); free(resp); free(op); free(body);
        return 0;
    }
    close(sfd);

    /* 6. Forward the daemon's JSON envelope verbatim. */
    cgi_emit_response(STDOUT_FILENO, resp, rlen);

    free(resp);
    free(op);
    free(body);
    return 0;
}
