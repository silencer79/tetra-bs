/* tb/sw/webui/test_op_allowlist.c — S6 allow-list contract test.
 *
 * For every CGI binary listed in OPERATIONS.md §1..§5, hit it with a
 * forbidden op and assert it returns HTTP 403 + FORBIDDEN_OP without
 * ever touching the daemon socket.
 *
 * Strategy: re-define `main` in each per-binary main.c via macro
 * renaming, then call them as ordinary functions from this file. The
 * harness redirects stdin/stdout per-call, sets CGI env vars to a
 * known-bad op, and asserts the produced HTTP envelope.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "tetra/cgi_common.h"
#include "unity.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* Each per-binary main.c defines `int main(void)`. We rename to a
 * unique symbol per-CGI by re-#define'ing main before #include'ing
 * the source. (The CGI mains have no other symbols, so this is safe
 * and avoids dragging the build of cgi_common.c into 12 separate
 * test binaries.)
 */
#define main apply_main
#include "../../../sw/webui/src/apply_main.c"
#undef main

#define main entities_main
#include "../../../sw/webui/src/entities_main.c"
#undef main

#define main policy_main
#include "../../../sw/webui/src/policy_main.c"
#undef main

#define main profiles_main
#include "../../../sw/webui/src/profiles_main.c"
#undef main

#define main sessions_main
#include "../../../sw/webui/src/sessions_main.c"
#undef main

#define main status_main
#include "../../../sw/webui/src/status_main.c"
#undef main

#define main stop_main
#include "../../../sw/webui/src/stop_main.c"
#undef main

#define main db_main
#include "../../../sw/webui/src/db_main.c"
#undef main

#define main debug_main
#include "../../../sw/webui/src/debug_main.c"
#undef main

#define main config_main
#include "../../../sw/webui/src/config_main.c"
#undef main

#define main tools_main
#include "../../../sw/webui/src/tools_main.c"
#undef main

#define main jobs_main
#include "../../../sw/webui/src/jobs_main.c"
#undef main

void setUp(void) {}
void tearDown(void) {}

/* Run one CGI binary's main() with stdin empty + stdout captured. */
static void run_with_stub_io(int (*entry)(void),
                             const char *query_string,
                             char *out_buf, size_t out_cap)
{
    /* Defeat any actual daemon connect attempt. */
    setenv("TETRA_D_SOCK", "/nonexistent/allowlist_test.sock", 1);
    setenv("REQUEST_METHOD", "GET", 1);
    setenv("QUERY_STRING", query_string, 1);
    unsetenv("CONTENT_LENGTH");

    int saved_in  = dup(STDIN_FILENO);
    int saved_out = dup(STDOUT_FILENO);

    int in_pipe[2], out_pipe[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(in_pipe));
    TEST_ASSERT_EQUAL_INT(0, pipe(out_pipe));
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, dup2(in_pipe[0], STDIN_FILENO));
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, dup2(out_pipe[1], STDOUT_FILENO));
    close(in_pipe[0]); close(in_pipe[1]);
    close(out_pipe[1]);

    (void)entry();
    fflush(stdout);

    dup2(saved_in,  STDIN_FILENO);  close(saved_in);
    dup2(saved_out, STDOUT_FILENO); close(saved_out);

    size_t got = 0;
    while (got < out_cap - 1) {
        const ssize_t n = read(out_pipe[0], out_buf + got, out_cap - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    out_buf[got] = '\0';
    close(out_pipe[0]);
}

static void assert_forbidden(int (*entry)(void), const char *bogus_op)
{
    char qs[128];
    snprintf(qs, sizeof(qs), "op=%s", bogus_op);
    char out[1024];
    run_with_stub_io(entry, qs, out, sizeof(out));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "Status: 403 Forbidden"), bogus_op);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "\"code\":\"FORBIDDEN_OP\""), bogus_op);
    /* Must NOT have leaked through to a daemon-connect error. */
    TEST_ASSERT_NULL_MESSAGE(strstr(out, "Status: 502"), bogus_op);
}

static void test_apply_cgi_rejects(void)    { assert_forbidden(apply_main,    "tools.reg_write"); }
static void test_entities_cgi_rejects(void) { assert_forbidden(entities_main, "profile.put"); }
static void test_policy_cgi_rejects(void)   { assert_forbidden(policy_main,   "tools.reg_write"); }
static void test_profiles_cgi_rejects(void) { assert_forbidden(profiles_main, "entity.put"); }
static void test_sessions_cgi_rejects(void) { assert_forbidden(sessions_main, "profile.put"); }
static void test_status_cgi_rejects(void)   { assert_forbidden(status_main,   "tools.reg_write"); }
static void test_stop_cgi_rejects(void)     { assert_forbidden(stop_main,     "tools.reg_write"); }
static void test_db_cgi_rejects(void)       { assert_forbidden(db_main,       "profile.list"); }
static void test_debug_cgi_rejects(void)    { assert_forbidden(debug_main,    "tools.reg_write"); }
static void test_config_cgi_rejects(void)   { assert_forbidden(config_main,   "tools.reg_write"); }
static void test_tools_cgi_rejects(void)    { assert_forbidden(tools_main,    "profile.put"); }
static void test_jobs_cgi_rejects(void)     { assert_forbidden(jobs_main,     "tools.reg_write"); }

/* Spot-check that an ALLOWED op is NOT short-circuited (it would try
 * to connect → 502 against our nonexistent socket). The contract is:
 * the gate releases it past the 403 path. */
static void test_status_cgi_allowed_op_passes_gate(void)
{
    char out[1024];
    run_with_stub_io(status_main, "op=status.summary", out, sizeof(out));
    /* Allowed → falls through to socket connect → 502 (nonexistent path). */
    TEST_ASSERT_NOT_NULL(strstr(out, "Status: 502"));
    TEST_ASSERT_NULL(strstr(out, "FORBIDDEN_OP"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_apply_cgi_rejects);
    RUN_TEST(test_entities_cgi_rejects);
    RUN_TEST(test_policy_cgi_rejects);
    RUN_TEST(test_profiles_cgi_rejects);
    RUN_TEST(test_sessions_cgi_rejects);
    RUN_TEST(test_status_cgi_rejects);
    RUN_TEST(test_stop_cgi_rejects);
    RUN_TEST(test_db_cgi_rejects);
    RUN_TEST(test_debug_cgi_rejects);
    RUN_TEST(test_config_cgi_rejects);
    RUN_TEST(test_tools_cgi_rejects);
    RUN_TEST(test_jobs_cgi_rejects);
    RUN_TEST(test_status_cgi_allowed_op_passes_gate);
    return UNITY_END();
}
