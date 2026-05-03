/* tb/sw/daemon/test_socket_envelope.c — IF_DAEMON_OPS_v1 wire-format tests.
 *
 * Owned by S7 (S7-sw-tetra-d). Test-gate per agent contract:
 *   - JSON envelope round-trip (length-prefix + parse + serialise).
 *   - Error-code shape from OPERATIONS.md §6 catalogue.
 *   - Op-name table integrity: every entry in DAEMON_OP_LIST is
 *     reachable by name and resolves back to the same id.
 *   - Spawn the socket listener in test mode, connect a client, send
 *     each op-name with `args:{}`, assert the envelope returns
 *     `ok:true` with a JSON object/array under "data".
 *
 * The "spawn" step uses a child process so we do not pin the daemon's
 * full DaemonState shape into the test (the entity stack is exercised
 * by test_main_loop.c separately). Here we only care about envelope
 * routing, so we hand the listener a NULL DaemonState — every stub
 * handler in socket_server.c ignores it.
 */
#define _DEFAULT_SOURCE   /* usleep(3) on glibc strict-c11 */

#include "tetra/daemon_ops.h"
#include "unity.h"

#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* Forward decls — implemented in socket_server.c. */
int socket_server_listen(const char *path, int *out_fd);
int socket_server_close_listener(int fd, const char *path);
int socket_server_handle_client(int fd, void *state /* DaemonState* */);

static char  s_sock_path[256];

void setUp(void)    {}
void tearDown(void) {}

/* ---------------------------------------------------------------------------
 * Envelope unit tests — pure functions; no socket needed.
 * ------------------------------------------------------------------------- */

static void test_envelope_ok_round_trip(void)
{
    json_t *data = json_object();
    json_object_set_new(data, "x", json_integer(42));
    json_t *env = daemon_envelope_ok("req-123", data);
    TEST_ASSERT_NOT_NULL(env);
    TEST_ASSERT_TRUE(json_is_object(env));

    uint8_t *buf = NULL; size_t len = 0;
    int rc = daemon_envelope_marshal(env, &buf, &len);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_TRUE(len > DAEMON_OPS_LEN_PREFIX_BYTES);

    json_t *back = NULL;
    rc = daemon_envelope_unmarshal(buf, len, &back);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_TRUE(json_is_object(back));

    json_t *ok = json_object_get(back, "ok");
    TEST_ASSERT_TRUE(json_is_true(ok));
    json_t *rid = json_object_get(back, "req_id");
    TEST_ASSERT_EQUAL_STRING("req-123", json_string_value(rid));
    json_t *data2 = json_object_get(back, "data");
    TEST_ASSERT_TRUE(json_is_object(data2));
    TEST_ASSERT_EQUAL(42, json_integer_value(json_object_get(data2, "x")));

    free(buf);
    json_decref(env);
    json_decref(back);
}

static void test_envelope_err_shape(void)
{
    json_t *env = daemon_envelope_err("rid-x", DaemonErr_Einval,
                                      "bad arg", "args.x", NULL);
    TEST_ASSERT_NOT_NULL(env);
    json_t *ok = json_object_get(env, "ok");
    TEST_ASSERT_TRUE(json_is_false(ok));
    json_t *err = json_object_get(env, "error");
    TEST_ASSERT_TRUE(json_is_object(err));
    TEST_ASSERT_EQUAL_STRING("EINVAL",
        json_string_value(json_object_get(err, "code")));
    TEST_ASSERT_EQUAL_STRING("bad arg",
        json_string_value(json_object_get(err, "message")));
    TEST_ASSERT_EQUAL_STRING("args.x",
        json_string_value(json_object_get(err, "field")));
    json_decref(env);
}

static void test_envelope_unmarshal_short_frame(void)
{
    uint8_t bad[3] = { 0, 0, 1 };
    json_t *out = NULL;
    int rc = daemon_envelope_unmarshal(bad, sizeof(bad), &out);
    TEST_ASSERT_TRUE(rc < 0);
}

static void test_envelope_unmarshal_oversize(void)
{
    /* LEN field claims 2 GB → reject with E2BIG. */
    uint8_t hdr[8] = { 0x80, 0, 0, 0, '{', '}', 0, 0 };
    json_t *out = NULL;
    int rc = daemon_envelope_unmarshal(hdr, sizeof(hdr), &out);
    TEST_ASSERT_EQUAL(-DaemonErr_E2big, rc);
}

static void test_envelope_unmarshal_garbage(void)
{
    /* Valid len, but body is not JSON. */
    uint8_t buf[5] = { 0, 0, 0, 1, 'x' };
    json_t *out = NULL;
    int rc = daemon_envelope_unmarshal(buf, sizeof(buf), &out);
    TEST_ASSERT_EQUAL(-DaemonErr_Einval, rc);
}

static void test_op_table_integrity(void)
{
    size_t count = 0;
    const DaemonOpEntry *t = daemon_op_table(&count);
    TEST_ASSERT_TRUE(count > 0);
    TEST_ASSERT_EQUAL((size_t) DaemonOp__Count, count);
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_NOT_NULL(t[i].name);
        TEST_ASSERT_NOT_NULL(t[i].fn);
        TEST_ASSERT_EQUAL((int) i, (int) t[i].id);
        const DaemonOpEntry *back = daemon_op_table_find(t[i].name);
        TEST_ASSERT_NOT_NULL(back);
        TEST_ASSERT_EQUAL((int) t[i].id, (int) back->id);
        TEST_ASSERT_EQUAL((int) t[i].id, (int) daemon_op_lookup(t[i].name));
    }
    TEST_ASSERT_EQUAL((int) DaemonOp_Unknown, (int) daemon_op_lookup("no.such.op"));
}

static void test_err_name_round_trip(void)
{
    TEST_ASSERT_EQUAL_STRING("EINVAL",     daemon_err_name(DaemonErr_Einval));
    TEST_ASSERT_EQUAL_STRING("ENOENT",     daemon_err_name(DaemonErr_Enoent));
    TEST_ASSERT_EQUAL_STRING("EFPGA",      daemon_err_name(DaemonErr_Efpga));
    TEST_ASSERT_EQUAL_STRING("EINTERNAL",  daemon_err_name(DaemonErr_Einternal));
    /* Out-of-range falls through to EINTERNAL per contract. */
    TEST_ASSERT_EQUAL_STRING("EINTERNAL",  daemon_err_name((DaemonErrCode) 999));
}

/* ---------------------------------------------------------------------------
 * End-to-end socket round-trip via fork.
 *
 * Child: opens listener, accept-loop until "die" pseudo-op or N seconds.
 * Parent: connects, sends each op-name, asserts envelope shape.
 * ------------------------------------------------------------------------- */

static int connect_to(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
    struct sockaddr_un a; memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    if (connect(fd, (struct sockaddr *) &a, sizeof(a)) < 0) {
        int e = errno; close(fd); return -e;
    }
    return fd;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) (v >> 24); p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >>  8); p[3] = (uint8_t) (v);
}
static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
           ((uint32_t) p[2] <<  8) | ((uint32_t) p[3]);
}

static int write_all(int fd, const uint8_t *b, size_t n)
{
    size_t put = 0;
    while (put < n) {
        ssize_t r = write(fd, b + put, n - put);
        if (r < 0) { if (errno == EINTR) continue; return -errno; }
        put += (size_t) r;
    }
    return 0;
}
static int read_all(int fd, uint8_t *b, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, b + got, n - got);
        if (r < 0) { if (errno == EINTR) continue; return -errno; }
        if (r == 0) return -ECONNRESET;
        got += (size_t) r;
    }
    return 0;
}

static int do_one_request(const char *path, const char *op_name,
                          json_t **out_resp)
{
    int fd = connect_to(path);
    if (fd < 0) return fd;

    json_t *req = json_object();
    json_object_set_new(req, "op", json_string(op_name));
    json_object_set_new(req, "args", json_object());
    json_object_set_new(req, "req_id", json_string("test-1"));

    char *body = json_dumps(req, JSON_COMPACT);
    json_decref(req);
    size_t blen = strlen(body);
    uint8_t hdr[4]; put_be32(hdr, (uint32_t) blen);
    int rc = write_all(fd, hdr, 4);
    if (rc == 0) rc = write_all(fd, (const uint8_t *) body, blen);
    free(body);
    if (rc < 0) { close(fd); return rc; }

    uint8_t lenbuf[4];
    rc = read_all(fd, lenbuf, 4);
    if (rc < 0) { close(fd); return rc; }
    uint32_t rlen = get_be32(lenbuf);
    if (rlen == 0 || rlen > DAEMON_OPS_MAX_FRAME_BYTES) {
        close(fd); return -EINVAL;
    }
    uint8_t *rbuf = malloc(rlen);
    rc = read_all(fd, rbuf, rlen);
    close(fd);
    if (rc < 0) { free(rbuf); return rc; }
    json_error_t je;
    json_t *root = json_loadb((const char *) rbuf, rlen, 0, &je);
    free(rbuf);
    if (root == NULL) return -EINVAL;
    *out_resp = root;
    return 0;
}

/* Child: serve N accept-cycles then exit.
 *
 * socket_server_listen opens the FD with SOCK_NONBLOCK so the
 * production main_loop epolls it without blocking. The standalone
 * child loop here wants blocking accept(2) so it parks on the fd
 * until the parent connects — clear O_NONBLOCK before the loop. */
static void child_serve(const char *path, int n_cycles)
{
    int lfd = -1;
    int rc = socket_server_listen(path, &lfd);
    if (rc != 0) {
        _exit(2);
    }
    int flags = fcntl(lfd, F_GETFL, 0);
    if (flags >= 0) {
        (void) fcntl(lfd, F_SETFL, flags & ~O_NONBLOCK);
    }
    for (int i = 0; i < n_cycles; i++) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) { i--; continue; }
            break;
        }
        socket_server_handle_client(cfd, NULL);
        close(cfd);
    }
    socket_server_close_listener(lfd, path);
    _exit(0);
}

static void test_round_trip_known_op(void)
{
    pid_t pid = fork();
    TEST_ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        child_serve(s_sock_path, 1);
    }
    /* Wait for socket to bind. */
    for (int t = 0; t < 50; t++) {
        if (access(s_sock_path, F_OK) == 0) break;
        usleep(20 * 1000);
    }
    json_t *resp = NULL;
    int rc = do_one_request(s_sock_path, "status.summary", &resp);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_NOT_NULL(resp);
    TEST_ASSERT_TRUE(json_is_true(json_object_get(resp, "ok")));
    json_decref(resp);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
}

static void test_round_trip_unknown_op(void)
{
    pid_t pid = fork();
    TEST_ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        child_serve(s_sock_path, 1);
    }
    for (int t = 0; t < 50; t++) {
        if (access(s_sock_path, F_OK) == 0) break;
        usleep(20 * 1000);
    }
    json_t *resp = NULL;
    int rc = do_one_request(s_sock_path, "no.such.op", &resp);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_TRUE(json_is_false(json_object_get(resp, "ok")));
    json_t *err = json_object_get(resp, "error");
    TEST_ASSERT_TRUE(json_is_object(err));
    TEST_ASSERT_EQUAL_STRING("EINVAL",
        json_string_value(json_object_get(err, "code")));
    json_decref(resp);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
}

static void test_round_trip_all_ops(void)
{
    size_t count = 0;
    const DaemonOpEntry *t = daemon_op_table(&count);

    pid_t pid = fork();
    TEST_ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        child_serve(s_sock_path, (int) count);
    }
    for (int wt = 0; wt < 50; wt++) {
        if (access(s_sock_path, F_OK) == 0) break;
        usleep(20 * 1000);
    }
    for (size_t i = 0; i < count; i++) {
        json_t *resp = NULL;
        int rc = do_one_request(s_sock_path, t[i].name, &resp);
        if (rc != 0) {
            char m[256];
            snprintf(m, sizeof(m), "op=%s rc=%d", t[i].name, rc);
            TEST_FAIL_MESSAGE(m);
        }
        TEST_ASSERT_NOT_NULL(resp);
        json_t *ok = json_object_get(resp, "ok");
        TEST_ASSERT_NOT_NULL(ok);
        /* daemon.stop will return ok=true with NULL state — handler
         * tolerates NULL by always succeeding (snapped=false). */
        TEST_ASSERT_TRUE(json_is_true(ok));
        json_decref(resp);
    }
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
}

int main(void)
{
    /* Per-pid socket path so parallel test invocations do not collide. */
    snprintf(s_sock_path, sizeof(s_sock_path),
             "/tmp/tetra_d_test_%d.sock", (int) getpid());
    /* Ignore SIGPIPE so a dropped child does not abort the parent. */
    signal(SIGPIPE, SIG_IGN);

    UNITY_BEGIN();
    RUN_TEST(test_envelope_ok_round_trip);
    RUN_TEST(test_envelope_err_shape);
    RUN_TEST(test_envelope_unmarshal_short_frame);
    RUN_TEST(test_envelope_unmarshal_oversize);
    RUN_TEST(test_envelope_unmarshal_garbage);
    RUN_TEST(test_op_table_integrity);
    RUN_TEST(test_err_name_round_trip);
    RUN_TEST(test_round_trip_known_op);
    RUN_TEST(test_round_trip_unknown_op);
    RUN_TEST(test_round_trip_all_ops);
    return UNITY_END();
}
