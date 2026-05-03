/* sw/socket_server.c — Unix-socket request/response server for tetra_d.
 *
 * Owned by S7 (S7-sw-tetra-d). Implements the daemon side of
 * IF_DAEMON_OPS_v1 + the wire envelope from docs/OPERATIONS.md §6:
 *   - listener at /var/run/tetra_d.sock (mode 0660 root:tetra)
 *   - accept(2) clients, single-shot length-prefixed JSON request →
 *     length-prefixed JSON response, close. Re-use is opt-in but the
 *     CGI corpus (S6) is per-request fork+connect+close — matches.
 *   - dispatch by op-name through DAEMON_OP_LIST (daemon_ops.h).
 *   - default error mapping per OPERATIONS.md §6 catalogue.
 *
 * Async-jobs (§9) are tracked via a small in-memory ring; this file
 * exposes the four jobs.* ops + tools.* enqueue ops as
 * stub-implementations that the operational handler set fills in
 * later. Stubs return ok-envelopes so the test gate exercises
 * envelope routing without dragging in real-HW state.
 */
#include "tetra/daemon_ops.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * Op-name <-> id table.
 *
 * One static array; the DAEMON_OP_LIST X-macro makes the names + enum
 * consistent at compile time so a typo in either column is a build
 * error.
 *
 * Handler bodies are defined further down — see s_handler_<suffix>.
 * Keep them in the same order as DAEMON_OP_LIST for grep-ability.
 * ------------------------------------------------------------------------- */

#define DAEMON_OP_HANDLER_DECL(opname, suffix)                         \
    static int s_handler_##suffix(DaemonState *state,                  \
                                  const json_t *args,                  \
                                  json_t **result);
DAEMON_OP_LIST(DAEMON_OP_HANDLER_DECL)
#undef DAEMON_OP_HANDLER_DECL

static const DaemonOpEntry s_op_table[] = {
#define DAEMON_OP_TABLE_ROW(opname, suffix)                            \
    { .id = DaemonOp_##suffix,                                         \
      .name = opname,                                                  \
      .fn = s_handler_##suffix,                                        \
      .min_args_present = NULL,                                        \
      .is_streaming = false,                                           \
      .is_async_job = false },
DAEMON_OP_LIST(DAEMON_OP_TABLE_ROW)
#undef DAEMON_OP_TABLE_ROW
};

const DaemonOpEntry *daemon_op_table(size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = sizeof(s_op_table) / sizeof(s_op_table[0]);
    }
    return s_op_table;
}

const DaemonOpEntry *daemon_op_table_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_op_table) / sizeof(s_op_table[0]); i++) {
        if (strcmp(s_op_table[i].name, name) == 0) {
            return &s_op_table[i];
        }
    }
    return NULL;
}

const char *daemon_op_name(DaemonOpId op)
{
    if ((int) op < 0 || (int) op >= (int) DaemonOp__Count) {
        return NULL;
    }
    return s_op_table[(size_t) op].name;
}

DaemonOpId daemon_op_lookup(const char *name)
{
    const DaemonOpEntry *e = daemon_op_table_find(name);
    return (e != NULL) ? e->id : DaemonOp_Unknown;
}

static const char *const s_err_names[] = {
#define DAEMON_ERR_NAME_ROW(id, str) [id] = str,
    DAEMON_ERR_LIST(DAEMON_ERR_NAME_ROW)
#undef DAEMON_ERR_NAME_ROW
};

const char *daemon_err_name(DaemonErrCode err)
{
    if ((int) err < 0 || (int) err >= (int) DaemonErr__Count) {
        return "EINTERNAL";
    }
    return s_err_names[err];
}

/* ---------------------------------------------------------------------------
 * Envelope helpers — public via daemon_ops.h.
 * ------------------------------------------------------------------------- */

json_t *daemon_envelope_ok(const char *req_id, json_t *data)
{
    json_t *root = json_object();
    if (root == NULL) {
        if (data != NULL) {
            json_decref(data);
        }
        return NULL;
    }
    json_object_set_new(root, "ok", json_true());
    if (req_id != NULL) {
        json_object_set_new(root, "req_id", json_string(req_id));
    }
    if (data != NULL) {
        json_object_set_new(root, "data", data);
    } else {
        json_object_set_new(root, "data", json_object());
    }
    return root;
}

json_t *daemon_envelope_err(const char    *req_id,
                            DaemonErrCode  code,
                            const char    *message,
                            const char    *field,
                            json_t        *detail)
{
    json_t *root = json_object();
    json_t *err  = json_object();
    if (root == NULL || err == NULL) {
        if (root != NULL) json_decref(root);
        if (err  != NULL) json_decref(err);
        if (detail != NULL) json_decref(detail);
        return NULL;
    }
    json_object_set_new(root, "ok", json_false());
    if (req_id != NULL) {
        json_object_set_new(root, "req_id", json_string(req_id));
    }
    json_object_set_new(err, "code", json_string(daemon_err_name(code)));
    if (message != NULL) {
        json_object_set_new(err, "message", json_string(message));
    }
    if (field != NULL) {
        json_object_set_new(err, "field", json_string(field));
    }
    if (detail != NULL) {
        json_object_set_new(err, "detail", detail);
    }
    json_object_set_new(root, "error", err);
    return root;
}

int daemon_envelope_parse(const json_t  *root,
                          const char   **out_op_name,
                          const json_t **out_args,
                          const char   **out_req_id,
                          const char   **out_client)
{
    if (root == NULL || !json_is_object(root) || out_op_name == NULL ||
        out_args == NULL) {
        return -DaemonErr_Einval;
    }
    json_t *op = json_object_get(root, "op");
    if (!json_is_string(op)) {
        return -DaemonErr_Einval;
    }
    *out_op_name = json_string_value(op);

    json_t *args = json_object_get(root, "args");
    *out_args = (args != NULL && json_is_object(args)) ? args : NULL;

    json_t *req_id = json_object_get(root, "req_id");
    if (out_req_id != NULL) {
        *out_req_id = json_is_string(req_id) ? json_string_value(req_id) : NULL;
    }
    json_t *client = json_object_get(root, "client");
    if (out_client != NULL) {
        *out_client = json_is_string(client) ? json_string_value(client) : NULL;
    }
    return 0;
}

static void put_be32(uint8_t *buf, uint32_t v)
{
    buf[0] = (uint8_t) ((v >> 24) & 0xFFu);
    buf[1] = (uint8_t) ((v >> 16) & 0xFFu);
    buf[2] = (uint8_t) ((v >>  8) & 0xFFu);
    buf[3] = (uint8_t) ( v        & 0xFFu);
}

static uint32_t get_be32(const uint8_t *buf)
{
    return ((uint32_t) buf[0] << 24) |
           ((uint32_t) buf[1] << 16) |
           ((uint32_t) buf[2] <<  8) |
           ((uint32_t) buf[3]);
}

int daemon_envelope_marshal(const json_t *env,
                            uint8_t     **out_buf,
                            size_t       *out_len)
{
    if (env == NULL || out_buf == NULL || out_len == NULL) {
        return -EINVAL;
    }
    char *body = json_dumps(env, JSON_COMPACT | JSON_PRESERVE_ORDER);
    if (body == NULL) {
        return -ENOMEM;
    }
    size_t body_len = strlen(body);
    if (body_len > DAEMON_OPS_MAX_FRAME_BYTES) {
        free(body);
        return -E2BIG;
    }
    size_t total = DAEMON_OPS_LEN_PREFIX_BYTES + body_len;
    uint8_t *buf = malloc(total);
    if (buf == NULL) {
        free(body);
        return -ENOMEM;
    }
    put_be32(buf, (uint32_t) body_len);
    memcpy(buf + DAEMON_OPS_LEN_PREFIX_BYTES, body, body_len);
    free(body);
    *out_buf = buf;
    *out_len = total;
    return 0;
}

int daemon_envelope_unmarshal(const uint8_t  *buf,
                              size_t          len,
                              json_t        **out_root)
{
    if (buf == NULL || out_root == NULL) {
        return -DaemonErr_Einval;
    }
    if (len < DAEMON_OPS_LEN_PREFIX_BYTES) {
        return -DaemonErr_Einval;
    }
    uint32_t body_len = get_be32(buf);
    if (body_len > DAEMON_OPS_MAX_FRAME_BYTES) {
        return -DaemonErr_E2big;
    }
    if ((size_t) body_len + DAEMON_OPS_LEN_PREFIX_BYTES > len) {
        return -DaemonErr_Einval;
    }
    json_error_t jerr;
    json_t *root = json_loadb((const char *) buf + DAEMON_OPS_LEN_PREFIX_BYTES,
                              (size_t) body_len, 0, &jerr);
    if (root == NULL || !json_is_object(root)) {
        if (root != NULL) {
            json_decref(root);
        }
        return -DaemonErr_Einval;
    }
    *out_root = root;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Single-request dispatch — used by main_loop.c on each ready client fd.
 *
 * Read N bytes (up to MAX_FRAME), parse, dispatch handler, marshal
 * response, write, return. Errors at every step generate a structured
 * error envelope; only catastrophic write-failures cause us to drop the
 * client without a response. Length-prefixed framing means we read
 * exactly one envelope per call.
 *
 * The function is single-shot per fd: caller is expected to close the
 * fd after this returns. (If/when we want pipelined CGIs, this fn flips
 * to a loop; today the BusyBox pattern is one-shot.)
 * ------------------------------------------------------------------------- */
static ssize_t read_full(int fd, uint8_t *buf, size_t want)
{
    size_t got = 0;
    while (got < want) {
        ssize_t n = read(fd, buf + got, want - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (n == 0) {
            return -ECONNRESET;
        }
        got += (size_t) n;
    }
    return (ssize_t) got;
}

static ssize_t write_full(int fd, const uint8_t *buf, size_t want)
{
    size_t put = 0;
    while (put < want) {
        ssize_t n = write(fd, buf + put, want - put);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        put += (size_t) n;
    }
    return (ssize_t) put;
}

static int send_envelope(int fd, json_t *env)
{
    if (env == NULL) {
        return -ENOMEM;
    }
    uint8_t *buf = NULL;
    size_t   len = 0;
    int rc = daemon_envelope_marshal(env, &buf, &len);
    json_decref(env);
    if (rc != 0) {
        return rc;
    }
    ssize_t w = write_full(fd, buf, len);
    free(buf);
    return (w < 0) ? (int) w : 0;
}

/* socket_server_handle_client — public to main_loop.c via the prototype
 * in main_loop.c (not exposed in daemon_ops.h since it is implementation
 * glue, not part of IF_DAEMON_OPS_v1). */
int socket_server_handle_client(int fd, DaemonState *state);

int socket_server_handle_client(int fd, DaemonState *state)
{
    uint8_t lenbuf[DAEMON_OPS_LEN_PREFIX_BYTES];
    ssize_t rc = read_full(fd, lenbuf, sizeof(lenbuf));
    if (rc < 0) {
        return (int) rc;
    }
    uint32_t body_len = get_be32(lenbuf);
    if (body_len == 0u || body_len > DAEMON_OPS_MAX_FRAME_BYTES) {
        json_t *err = daemon_envelope_err(NULL, DaemonErr_E2big,
                                          "frame length out of range",
                                          "len", NULL);
        send_envelope(fd, err);
        return -DaemonErr_E2big;
    }
    uint8_t *body = malloc(body_len);
    if (body == NULL) {
        return -ENOMEM;
    }
    rc = read_full(fd, body, body_len);
    if (rc < 0) {
        free(body);
        return (int) rc;
    }
    json_error_t jerr;
    json_t *root = json_loadb((const char *) body, body_len, 0, &jerr);
    free(body);
    if (root == NULL || !json_is_object(root)) {
        if (root != NULL) json_decref(root);
        json_t *err = daemon_envelope_err(NULL, DaemonErr_Einval,
                                          "envelope is not a JSON object",
                                          "body", NULL);
        return send_envelope(fd, err);
    }

    const char   *op_name = NULL;
    const json_t *args    = NULL;
    const char   *req_id  = NULL;
    const char   *client  = NULL;
    int prc = daemon_envelope_parse(root, &op_name, &args, &req_id, &client);
    if (prc != 0) {
        json_t *err = daemon_envelope_err(req_id, DaemonErr_Einval,
                                          "missing or invalid op",
                                          "op", NULL);
        json_decref(root);
        return send_envelope(fd, err);
    }
    (void) client;  /* reserved for future audit-log hook (§7) */

    const DaemonOpEntry *entry = daemon_op_table_find(op_name);
    if (entry == NULL) {
        json_t *err = daemon_envelope_err(req_id, DaemonErr_Einval,
                                          "unknown op", "op", NULL);
        json_decref(root);
        return send_envelope(fd, err);
    }

    /* Optional pre-validation hook: ensure required arg keys present. */
    if (entry->min_args_present != NULL) {
        if (args == NULL) {
            json_t *err = daemon_envelope_err(req_id, DaemonErr_Einval,
                                              "args missing", "args", NULL);
            json_decref(root);
            return send_envelope(fd, err);
        }
        for (size_t i = 0; entry->min_args_present[i] != NULL; i++) {
            if (json_object_get(args, entry->min_args_present[i]) == NULL) {
                json_t *err = daemon_envelope_err(req_id, DaemonErr_Einval,
                                                  "required arg missing",
                                                  entry->min_args_present[i],
                                                  NULL);
                json_decref(root);
                return send_envelope(fd, err);
            }
        }
    }

    json_t *data = NULL;
    int hrc = entry->fn(state, args, &data);
    json_t *resp = NULL;
    if (hrc == 0) {
        resp = daemon_envelope_ok(req_id, data);
    } else {
        DaemonErrCode ec = (DaemonErrCode) (-hrc);
        if ((int) ec < 0 || (int) ec >= (int) DaemonErr__Count) {
            ec = DaemonErr_Einternal;
        }
        if (data != NULL) {
            json_decref(data);
            data = NULL;
        }
        resp = daemon_envelope_err(req_id, ec, NULL, NULL, NULL);
    }
    json_decref(root);
    return send_envelope(fd, resp);
}

/* ---------------------------------------------------------------------------
 * Listener setup — main_loop.c calls socket_server_listen() once at
 * startup and adds the returned fd to its epoll set; then on each
 * accept-ready event it spawns socket_server_handle_client() above.
 *
 * Path is configurable for tests (so test_socket_envelope can put the
 * socket under a tempdir). Mode is hard-set to 0660 per OPERATIONS.md
 * §"Runtime contract".
 * ------------------------------------------------------------------------- */
int socket_server_listen(const char *path, int *out_fd);

int socket_server_listen(const char *path, int *out_fd)
{
    if (path == NULL || out_fd == NULL) {
        return -EINVAL;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return -errno;
    }
    /* Best-effort unlink — stale socket from previous run is normal. */
    (void) unlink(path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(fd);
        return -ENAMETOOLONG;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    if (chmod(path, 0660) < 0) {
        /* Non-fatal in test mode; production deploy script chowns the
         * group separately. */
    }
    if (listen(fd, 16) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    *out_fd = fd;
    return 0;
}

int socket_server_close_listener(int fd, const char *path)
{
    if (fd >= 0) {
        close(fd);
    }
    if (path != NULL) {
        (void) unlink(path);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Stub op-handlers.
 *
 * These return ok-envelopes with empty data objects (or shaped fixtures)
 * so the test gate exercises envelope round-trip without depending on
 * real entity state. The Phase-3 cosim agent fills in the real bodies;
 * each handler is one C function with a single, traceable git history.
 *
 * The shape of `result` for each op matches the §6 "Response data"
 * column from OPERATIONS.md so S6 can shape its CGI output today.
 * Where a body is ambiguous (large, depends on live state), an empty
 * object is returned and a TODO marker recorded in source.
 * ------------------------------------------------------------------------- */

#define UNUSED_HANDLER_ARGS                                            \
    (void) state; (void) args

static int s_ok_empty(json_t **result)
{
    *result = json_object();
    return (*result != NULL) ? 0 : -DaemonErr_Einternal;
}

static int s_ok_json(json_t **result, json_t *data)
{
    if (data == NULL) {
        return -DaemonErr_Einternal;
    }
    *result = data;
    return 0;
}

/* §1 status.* */
static int s_handler_status_summary(DaemonState *state, const json_t *args, json_t **result)
{
    UNUSED_HANDLER_ARGS;
    json_t *o = json_object();
    json_object_set_new(o, "cell_synced",        json_false());
    json_object_set_new(o, "uptime_s",           json_integer(0));
    json_object_set_new(o, "fpga_bitstream_id",  json_string(""));
    json_object_set_new(o, "sw_version",         json_string("tetra_d/0"));
    return s_ok_json(result, o);
}
static int s_handler_status_layers(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_status_phy_stats(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_status_dma_stats(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_status_txq(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_status_msgbus(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_status_calls(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; *r = json_array(); return (*r != NULL) ? 0 : -DaemonErr_Einternal; }
static int s_handler_status_groups(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; *r = json_array(); return (*r != NULL) ? 0 : -DaemonErr_Einternal; }
static int s_handler_status_fpga_regs(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_status_aach_current(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }

/* §2 profile.* / entity.* / session.* / db.* / policy.* */
static int s_handler_profile_list(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; *r = json_array(); return (*r != NULL) ? 0 : -DaemonErr_Einternal; }
static int s_handler_profile_get(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_profile_put(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_profile_delete(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_profile_reset(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_entity_list(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a;
  json_t *o = json_object();
  json_object_set_new(o, "total", json_integer(0));
  json_object_set_new(o, "items", json_array());
  return s_ok_json(r, o); }
static int s_handler_entity_get(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_entity_lookup(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_entity_put(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_entity_delete(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_entity_clear_all(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_session_list(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; *r = json_array(); return (*r != NULL) ? 0 : -DaemonErr_Einternal; }
static int s_handler_session_get(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_session_stats(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a;
  json_t *o = json_object();
  json_object_set_new(o, "capacity", json_integer(64));
  json_object_set_new(o, "used", json_integer(0));
  json_object_set_new(o, "free", json_integer(64));
  return s_ok_json(r, o); }
static int s_handler_db_export(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_db_import(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_db_reset(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_policy_get(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_policy_put(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }

/* §3 debug.* */
static int s_handler_debug_pdu_recent(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; *r = json_array(); return (*r != NULL) ? 0 : -DaemonErr_Einternal; }
static int s_handler_debug_pdu_stream(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_debug_aach_recent(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; *r = json_array(); return (*r != NULL) ? 0 : -DaemonErr_Einternal; }
static int s_handler_debug_aach_stream(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_debug_slot_schedule(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; *r = json_array(); return (*r != NULL) ? 0 : -DaemonErr_Einternal; }
static int s_handler_debug_irq_counters(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_debug_msgbus_stream(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_debug_log_tail(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; *r = json_array(); return (*r != NULL) ? 0 : -DaemonErr_Einternal; }
static int s_handler_debug_reg_read(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_debug_reg_write(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_debug_tmasap_recent(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; *r = json_array(); return (*r != NULL) ? 0 : -DaemonErr_Einternal; }
static int s_handler_debug_tmdsap_stats(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }

/* §4 config.* / apply.* */
static int s_handler_config_cell_get(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_config_cell_put(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_config_rf_get(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_config_rf_put(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_config_cipher_get(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_config_cipher_put(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_config_training_get(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_config_training_put(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_config_slot_table_get(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_config_slot_table_put(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_config_ad9361_get(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_config_ad9361_put(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_config_msgbus_get(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_config_msgbus_put(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_config_schema(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_apply_status(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a;
  json_t *o = json_object();
  json_object_set_new(o, "has_pending", json_false());
  json_object_set_new(o, "pending_groups", json_array());
  return s_ok_json(r, o); }
static int s_handler_apply_apply(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_apply_discard(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }

/* §5 tools.* */
static int s_handler_tools_pdu_send_form(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_tools_pdu_send(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a;
  json_t *o = json_object();
  json_object_set_new(o, "would_send", json_false());
  json_object_set_new(o, "tma_frame_hex", json_string(""));
  return s_ok_json(r, o); }
static int s_handler_tools_reset_counters(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a;
  json_t *o = json_object();
  json_object_set_new(o, "cleared", json_array());
  return s_ok_json(r, o); }
static int s_handler_tools_reset_ast(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a;
  json_t *o = json_object();
  json_object_set_new(o, "flushed_slots", json_integer(0));
  return s_ok_json(r, o); }
static int s_handler_tools_reg_write(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_tools_bitstream_list(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; *r = json_array(); return (*r != NULL) ? 0 : -DaemonErr_Einternal; }
static int s_handler_tools_bitstream_switch(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a;
  json_t *o = json_object();
  json_object_set_new(o, "job_id", json_string("0"));
  return s_ok_json(r, o); }
static int s_handler_tools_capture_start(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a;
  json_t *o = json_object();
  json_object_set_new(o, "job_id", json_string("0"));
  return s_ok_json(r, o); }
static int s_handler_tools_capture_stop(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_tools_capture_list(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; *r = json_array(); return (*r != NULL) ? 0 : -DaemonErr_Einternal; }
static int s_handler_tools_capture_download(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_tools_decoder_upload(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_tools_decoder_list(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; *r = json_array(); return (*r != NULL) ? 0 : -DaemonErr_Einternal; }
static int s_handler_tools_decoder_run(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a;
  json_t *o = json_object();
  json_object_set_new(o, "job_id", json_string("0"));
  return s_ok_json(r, o); }
static int s_handler_tools_decoder_result(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }

/* §5 jobs.* */
static int s_handler_jobs_list(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; *r = json_array(); return (*r != NULL) ? 0 : -DaemonErr_Einternal; }
static int s_handler_jobs_get(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }
static int s_handler_jobs_cancel(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a; return s_ok_empty(r); }

/* §5 daemon.* — daemon.stop is special: see daemon_request_shutdown().
 *
 * Implemented in main_loop.c; declared here as a weak symbol so the
 * standalone envelope test (tb/sw/daemon/test_socket_envelope.c) can
 * link socket_server.c without dragging in the full main-loop +
 * entity stack. When the weak symbol is absent the handler skips the
 * shutdown call and returns ast_snapshotted=false. */
__attribute__((weak))
int daemon_request_shutdown(DaemonState *state, bool *out_ast_snapshotted);

static int s_handler_daemon_stop(DaemonState *state, const json_t *args, json_t **result)
{
    (void) args;
    bool snapped = false;
    if (state != NULL && daemon_request_shutdown != NULL) {
        int rc = daemon_request_shutdown(state, &snapped);
        if (rc != 0) {
            return -DaemonErr_Einternal;
        }
    }
    json_t *o = json_object();
    json_object_set_new(o, "ok", json_true());
    json_object_set_new(o, "ast_snapshotted", snapped ? json_true() : json_false());
    return s_ok_json(result, o);
}

static int s_handler_daemon_restart(DaemonState *s, const json_t *a, json_t **r)
{ (void) s; (void) a;
  json_t *o = json_object();
  json_object_set_new(o, "job_id", json_string("0"));
  return s_ok_json(r, o); }
