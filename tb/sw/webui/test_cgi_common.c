/* tb/sw/webui/test_cgi_common.c — S6 Unity host tests for cgi_common.
 *
 * Owned by S6. Test gate per docs/MIGRATION_PLAN.md §S6:
 *
 *   - env-parse (REQUEST_METHOD / QUERY_STRING / CONTENT_LENGTH)
 *   - stdin-body-read (CONTENT_LENGTH bytes, exact match, EOF guard)
 *   - length-prefix encode/decode (4-byte BE)
 *   - request-envelope build (OPERATIONS.md §6 shape)
 *   - error-code mapping (FORBIDDEN_OP, gateway timeouts, 502)
 *   - full mock-daemon round-trip via socketpair() — no real S7 needed.
 *
 * Mock pattern: socketpair(AF_UNIX, SOCK_STREAM) — one end given to a
 * mock-daemon thread, the other end stuffed into TETRA_D_SOCK via a
 * pre-bound listening AF_UNIX path under /tmp. We use the listener
 * approach so connect_daemon() exercises its real connect() path
 * (matches the dma_io.c mock idiom S1 set up).
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "tetra/cgi_common.h"
#include "unity.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- Common scaffold ---------------------------------------------------- */

void setUp(void) {}
void tearDown(void) {}

/* Replace stdin with a memory buffer (via pipe). Returns the read end
 * fd dup'd onto STDIN_FILENO, plus the write fd for the producer. */
static int stdin_pipe_replace(int *write_fd_out, const char *body, size_t len)
{
    int p[2];
    if (pipe(p) < 0) return -1;
    if (dup2(p[0], STDIN_FILENO) < 0) { close(p[0]); close(p[1]); return -1; }
    close(p[0]);
    if (len > 0) {
        if (write(p[1], body, len) != (ssize_t)len) {
            close(p[1]);
            return -1;
        }
    }
    *write_fd_out = p[1];
    return 0;
}

/* Replace stdout with a pipe; returns read fd to consume what the CGI
 * wrote. */
static int stdout_pipe_replace(int *read_fd_out)
{
    int p[2];
    if (pipe(p) < 0) return -1;
    if (dup2(p[1], STDOUT_FILENO) < 0) { close(p[0]); close(p[1]); return -1; }
    close(p[1]);
    *read_fd_out = p[0];
    return 0;
}

/* Drain a pipe until EOF or maxlen. */
static size_t drain_pipe(int fd, char *buf, size_t cap)
{
    size_t got = 0;
    while (got < cap) {
        const ssize_t n = read(fd, buf + got, cap - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    return got;
}

/* ---- Test 1: length-prefix encode/decode -------------------------------- */
static void test_lp_encode_decode_round_trip(void)
{
    uint8_t buf[4];
    cgi_pack_len_be(buf, 0x12345678u);
    TEST_ASSERT_EQUAL_UINT8(0x12, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x34, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x56, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0x78, buf[3]);
    TEST_ASSERT_EQUAL_UINT32(0x12345678u, cgi_unpack_len_be(buf));

    cgi_pack_len_be(buf, 0u);
    TEST_ASSERT_EQUAL_UINT32(0u, cgi_unpack_len_be(buf));
    cgi_pack_len_be(buf, 0xFFFFFFFFu);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, cgi_unpack_len_be(buf));
}

/* ---- Test 2: request-envelope build ------------------------------------ */
static void test_request_envelope_shape(void)
{
    char buf[512];
    int n = cgi_build_request(buf, sizeof(buf),
                              "profile.put",
                              "{\"id\":3,\"priority\":7}",
                              NULL,
                              "cgi:profiles.cgi");
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    /* Spec: must contain op + args + client; req_id optional. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"op\":\"profile.put\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"args\":{\"id\":3,\"priority\":7}"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"client\":\"cgi:profiles.cgi\""));
}

static void test_request_envelope_empty_args(void)
{
    char buf[256];
    int n = cgi_build_request(buf, sizeof(buf),
                              "status.summary", NULL, NULL, "cgi:status.cgi");
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"args\":{}"));
}

static void test_request_envelope_overflow(void)
{
    char buf[40];  /* far too small */
    int n = cgi_build_request(buf, sizeof(buf),
                              "status.summary", "{}", NULL, "cgi:status.cgi");
    TEST_ASSERT_LESS_THAN_INT(0, n);
}

/* ---- Test 3: cgi_op_allowed --------------------------------------------- */
static void test_op_allowed_table(void)
{
    static const char *const allow[] = {
        "profile.list", "profile.get", "profile.put", NULL
    };
    CgiRunOpts o = { .script_name = "x", .allowed_ops = allow };
    TEST_ASSERT_TRUE(cgi_op_allowed(&o, "profile.list"));
    TEST_ASSERT_TRUE(cgi_op_allowed(&o, "profile.put"));
    TEST_ASSERT_FALSE(cgi_op_allowed(&o, "tools.reg_write"));
    TEST_ASSERT_FALSE(cgi_op_allowed(&o, NULL));
    TEST_ASSERT_FALSE(cgi_op_allowed(&o, ""));
}

/* ---- Test 4: read_atleast / write_all on a pipe ------------------------ */
static void test_read_atleast_basic(void)
{
    int p[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(p));
    const char msg[] = "ABCDEFGHIJ";
    TEST_ASSERT_EQUAL_INT((int)sizeof(msg) - 1,
                          (int)write(p[1], msg, sizeof(msg) - 1));
    close(p[1]);
    char buf[16] = {0};
    /* deadline 1s in future. */
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    long deadline = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L + 1000L;
    const ssize_t n = cgi_read_atleast(p[0], buf, 5, sizeof(buf) - 1, deadline);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(5, n);
    TEST_ASSERT_EQUAL_STRING_LEN(msg, buf, 5);
    close(p[0]);
}

static void test_read_atleast_timeout(void)
{
    int p[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(p));
    /* No data — deadline 50 ms in the past. */
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    long deadline = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L + 50L;
    char buf[8];
    const ssize_t n = cgi_read_atleast(p[0], buf, 4, 4, deadline);
    TEST_ASSERT_EQUAL_INT(-1, n);
    TEST_ASSERT_EQUAL_INT(ETIMEDOUT, errno);
    close(p[0]); close(p[1]);
}

static void test_write_all_short(void)
{
    int p[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(p));
    const char msg[] = "hello world";
    TEST_ASSERT_EQUAL_INT(0, cgi_write_all(p[1], msg, sizeof(msg) - 1));
    close(p[1]);
    char buf[32] = {0};
    const ssize_t n = read(p[0], buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT((int)(sizeof(msg) - 1), (int)n);
    TEST_ASSERT_EQUAL_STRING_LEN(msg, buf, sizeof(msg) - 1);
    close(p[0]);
}

/* ---- Test 5: cgi_emit_error / cgi_emit_response ------------------------ */
static void test_emit_error_shape(void)
{
    int p[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(p));
    TEST_ASSERT_EQUAL_INT(0,
        cgi_emit_error(p[1], 403, "FORBIDDEN_OP", "op not in CGI allow-list"));
    close(p[1]);
    char buf[1024] = {0};
    const size_t n = drain_pipe(p[0], buf, sizeof(buf) - 1);
    TEST_ASSERT_NOT_NULL(strstr(buf, "Status: 403 Forbidden\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "Content-Type: application/json\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ok\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"code\":\"FORBIDDEN_OP\""));
    (void)n;
    close(p[0]);
}

static void test_emit_response_shape(void)
{
    int p[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(p));
    const char body[] = "{\"ok\":true,\"data\":{\"x\":1}}";
    TEST_ASSERT_EQUAL_INT(0,
        cgi_emit_response(p[1], body, sizeof(body) - 1));
    close(p[1]);
    char buf[256] = {0};
    drain_pipe(p[0], buf, sizeof(buf) - 1);
    TEST_ASSERT_NOT_NULL(strstr(buf, "Status: 200 OK\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "Content-Type: application/json\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(buf, body));
    close(p[0]);
}

/* ---- Test 6: full round-trip via mocked daemon -------------------------- */

/* Mock daemon thread: bind a listener at @sock_path, accept once, read
 * length-prefixed request, write back a canned response envelope. */
typedef struct {
    char  sock_path[128];
    int   listen_fd;
    char  expect_op[64];
    char  reply[256];
    size_t reply_len;
    int   ok;
} MockArgs;

static void *mock_daemon_thread(void *arg)
{
    MockArgs *m = (MockArgs *)arg;
    const int cli = accept(m->listen_fd, NULL, NULL);
    if (cli < 0) { m->ok = -1; return NULL; }

    uint8_t lp[4];
    if (read(cli, lp, 4) != 4) { m->ok = -2; close(cli); return NULL; }
    const uint32_t rlen = ((uint32_t)lp[0] << 24) | ((uint32_t)lp[1] << 16)
                        | ((uint32_t)lp[2] << 8)  | (uint32_t)lp[3];
    if (rlen == 0 || rlen > 4096) { m->ok = -3; close(cli); return NULL; }
    char reqbuf[4096];
    if (read(cli, reqbuf, rlen) != (ssize_t)rlen) { m->ok = -4; close(cli); return NULL; }
    reqbuf[rlen] = '\0';
    if (strstr(reqbuf, m->expect_op) == NULL) { m->ok = -5; close(cli); return NULL; }

    uint8_t out_lp[4];
    cgi_pack_len_be(out_lp, (uint32_t)m->reply_len);
    if (write(cli, out_lp, 4) != 4) { m->ok = -6; close(cli); return NULL; }
    if (write(cli, m->reply, m->reply_len) != (ssize_t)m->reply_len) {
        m->ok = -7; close(cli); return NULL;
    }
    close(cli);
    m->ok = 1;
    return NULL;
}

static int spawn_mock(MockArgs *m, const char *expect_op,
                      const char *reply_json)
{
    snprintf(m->sock_path, sizeof(m->sock_path),
             "/tmp/tetra_d_test_%d_%ld.sock", getpid(),
             (long)time(NULL) ^ (long)pthread_self());
    unlink(m->sock_path);
    m->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m->listen_fd < 0) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, m->sock_path, sizeof(a.sun_path) - 1);
    if (bind(m->listen_fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(m->listen_fd); return -1;
    }
    if (listen(m->listen_fd, 1) < 0) { close(m->listen_fd); return -1; }
    snprintf(m->expect_op, sizeof(m->expect_op), "\"op\":\"%s\"", expect_op);
    m->reply_len = strlen(reply_json);
    memcpy(m->reply, reply_json, m->reply_len);
    m->ok = 0;
    return 0;
}

static void test_full_round_trip_status_summary(void)
{
    MockArgs m;
    TEST_ASSERT_EQUAL_INT(0, spawn_mock(&m, "status.summary",
        "{\"ok\":true,\"data\":{\"cell_synced\":true,\"uptime_s\":42}}"));
    pthread_t th;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, mock_daemon_thread, &m));

    /* Set env so connect_daemon() picks our socket. */
    setenv("TETRA_D_SOCK", m.sock_path, 1);
    setenv("REQUEST_METHOD", "GET", 1);
    setenv("QUERY_STRING", "op=status.summary", 1);
    unsetenv("CONTENT_LENGTH");

    /* Empty stdin pipe + capture stdout. */
    int wfd = -1, rfd = -1;
    /* Save & restore real fds. */
    int saved_in  = dup(STDIN_FILENO);
    int saved_out = dup(STDOUT_FILENO);
    TEST_ASSERT_EQUAL_INT(0, stdin_pipe_replace(&wfd, "", 0));
    close(wfd);  /* immediate EOF — no body */
    TEST_ASSERT_EQUAL_INT(0, stdout_pipe_replace(&rfd));

    static const char *const allow[] = { "status.summary", NULL };
    CgiRunOpts opts = {
        .script_name = "status.cgi",
        .allowed_ops = allow,
        .sock_path   = m.sock_path,
    };
    const int rc = cgi_run(&opts);
    fflush(stdout);
    /* Restore so Unity can print. */
    dup2(saved_in,  STDIN_FILENO);  close(saved_in);
    dup2(saved_out, STDOUT_FILENO); close(saved_out);

    TEST_ASSERT_EQUAL_INT(0, rc);

    char out[1024] = {0};
    drain_pipe(rfd, out, sizeof(out) - 1);
    close(rfd);

    pthread_join(th, NULL);
    TEST_ASSERT_EQUAL_INT(1, m.ok);
    close(m.listen_fd); unlink(m.sock_path);

    TEST_ASSERT_NOT_NULL(strstr(out, "Status: 200 OK"));
    TEST_ASSERT_NOT_NULL(strstr(out, "Content-Type: application/json"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"ok\":true"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"uptime_s\":42"));
}

/* ---- Test 7: forbidden op short-circuits without socket connect -------- */
static void test_forbidden_op_no_connect(void)
{
    /* Point sock to a path that doesn't exist — if cgi_run() tries to
     * connect we'd see EINTERNAL. The 403 must fire BEFORE that. */
    setenv("TETRA_D_SOCK", "/nonexistent/forbidden_test.sock", 1);
    setenv("REQUEST_METHOD", "GET", 1);
    setenv("QUERY_STRING", "op=tools.reg_write&addr=0xDEAD", 1);
    unsetenv("CONTENT_LENGTH");

    int wfd = -1, rfd = -1;
    int saved_in  = dup(STDIN_FILENO);
    int saved_out = dup(STDOUT_FILENO);
    TEST_ASSERT_EQUAL_INT(0, stdin_pipe_replace(&wfd, "", 0));
    close(wfd);
    TEST_ASSERT_EQUAL_INT(0, stdout_pipe_replace(&rfd));

    static const char *const allow[] = { "status.summary", NULL };
    CgiRunOpts opts = {
        .script_name = "status.cgi",
        .allowed_ops = allow,
    };
    const int rc = cgi_run(&opts);
    fflush(stdout);
    dup2(saved_in,  STDIN_FILENO);  close(saved_in);
    dup2(saved_out, STDOUT_FILENO); close(saved_out);

    TEST_ASSERT_EQUAL_INT(0, rc);

    char out[512] = {0};
    drain_pipe(rfd, out, sizeof(out) - 1);
    close(rfd);

    TEST_ASSERT_NOT_NULL(strstr(out, "Status: 403 Forbidden"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"code\":\"FORBIDDEN_OP\""));
    TEST_ASSERT_NULL(strstr(out, "EINTERNAL"));
}

/* ---- Test 8: POST body with op=... extraction --------------------------- */
static void test_post_body_op_extraction(void)
{
    MockArgs m;
    TEST_ASSERT_EQUAL_INT(0, spawn_mock(&m, "profile.put",
        "{\"ok\":true,\"data\":{\"id\":3,\"packed_word_hex\":\"0x12345678\"}}"));
    pthread_t th;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&th, NULL, mock_daemon_thread, &m));

    setenv("TETRA_D_SOCK", m.sock_path, 1);
    setenv("REQUEST_METHOD", "POST", 1);
    unsetenv("QUERY_STRING");
    const char body[] = "{\"op\":\"profile.put\",\"id\":3,\"priority\":7}";
    char clbuf[16];
    snprintf(clbuf, sizeof(clbuf), "%zu", sizeof(body) - 1);
    setenv("CONTENT_LENGTH", clbuf, 1);

    int wfd = -1, rfd = -1;
    int saved_in  = dup(STDIN_FILENO);
    int saved_out = dup(STDOUT_FILENO);
    TEST_ASSERT_EQUAL_INT(0, stdin_pipe_replace(&wfd, body, sizeof(body) - 1));
    close(wfd);
    TEST_ASSERT_EQUAL_INT(0, stdout_pipe_replace(&rfd));

    static const char *const allow[] = {
        "profile.list", "profile.get", "profile.put", NULL
    };
    CgiRunOpts opts = {
        .script_name = "profiles.cgi",
        .allowed_ops = allow,
    };
    const int rc = cgi_run(&opts);
    fflush(stdout);
    dup2(saved_in,  STDIN_FILENO);  close(saved_in);
    dup2(saved_out, STDOUT_FILENO); close(saved_out);

    TEST_ASSERT_EQUAL_INT(0, rc);

    char out[1024] = {0};
    drain_pipe(rfd, out, sizeof(out) - 1);
    close(rfd);

    pthread_join(th, NULL);
    TEST_ASSERT_EQUAL_INT(1, m.ok);
    close(m.listen_fd); unlink(m.sock_path);

    TEST_ASSERT_NOT_NULL(strstr(out, "\"ok\":true"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"packed_word_hex\":\"0x12345678\""));
}

/* ---- Test 9: socket-connect failure → 502 ------------------------------- */
static void test_socket_connect_fail_502(void)
{
    setenv("TETRA_D_SOCK", "/nonexistent/no_daemon_here.sock", 1);
    setenv("REQUEST_METHOD", "GET", 1);
    setenv("QUERY_STRING", "op=status.summary", 1);
    unsetenv("CONTENT_LENGTH");

    int wfd = -1, rfd = -1;
    int saved_in  = dup(STDIN_FILENO);
    int saved_out = dup(STDOUT_FILENO);
    TEST_ASSERT_EQUAL_INT(0, stdin_pipe_replace(&wfd, "", 0));
    close(wfd);
    TEST_ASSERT_EQUAL_INT(0, stdout_pipe_replace(&rfd));

    static const char *const allow[] = { "status.summary", NULL };
    CgiRunOpts opts = {
        .script_name = "status.cgi",
        .allowed_ops = allow,
    };
    (void)cgi_run(&opts);
    fflush(stdout);
    dup2(saved_in,  STDIN_FILENO);  close(saved_in);
    dup2(saved_out, STDOUT_FILENO); close(saved_out);

    char out[512] = {0};
    drain_pipe(rfd, out, sizeof(out) - 1);
    close(rfd);

    TEST_ASSERT_NOT_NULL(strstr(out, "Status: 502"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"ok\":false"));
}

/* ---- runner ------------------------------------------------------------- */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lp_encode_decode_round_trip);
    RUN_TEST(test_request_envelope_shape);
    RUN_TEST(test_request_envelope_empty_args);
    RUN_TEST(test_request_envelope_overflow);
    RUN_TEST(test_op_allowed_table);
    RUN_TEST(test_read_atleast_basic);
    RUN_TEST(test_read_atleast_timeout);
    RUN_TEST(test_write_all_short);
    RUN_TEST(test_emit_error_shape);
    RUN_TEST(test_emit_response_shape);
    RUN_TEST(test_full_round_trip_status_summary);
    RUN_TEST(test_forbidden_op_no_connect);
    RUN_TEST(test_post_body_op_extraction);
    RUN_TEST(test_socket_connect_fail_502);
    return UNITY_END();
}
