/* tb/sw/daemon/test_shutdown.c — clean-shutdown lifecycle (Decision #10).
 *
 * Owned by S7 (S7-sw-tetra-d). Test gate per agent contract:
 *   - simulate kill-mid-loop → ast_snapshot NOT taken; on next start
 *     ast_reload should refuse to populate the AST (clean_shutdown_flag
 *     is missing or false).
 *   - simulate clean SIGTERM → ast_snapshot taken with
 *     clean_shutdown_flag=true; on next start ast_reload populates
 *     the AST.
 *
 * The test exercises the AST path directly via S5's
 * ast_snapshot/ast_reload — these are the only library calls the
 * shutdown path makes for AST. The DaemonState struct duplicate that
 * lives in test_main_loop.c is NOT needed here because the test gate
 * is about the persistence boundary, not the loop wiring.
 */
#include "tetra/db.h"
#include "unity.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char s_ast_path[256];

void setUp(void)
{
    snprintf(s_ast_path, sizeof(s_ast_path),
             "/tmp/tetra_d_shutdown_test_%d.json", (int) getpid());
    (void) unlink(s_ast_path);
}

void tearDown(void)
{
    (void) unlink(s_ast_path);
}

/* ---------------------------------------------------------------------------
 * Build a non-trivial AST so the snapshot has visible content. */
static void fill_sample_ast(Ast *ast)
{
    memset(ast, 0, sizeof(*ast));
    ast->slots[0].valid = true;
    ast->slots[0].issi  = 0x282FF4u;     /* M2 ITSI */
    ast->slots[0].last_seen_multiframe = 1234u;
    ast->slots[0].shadow_idx = 7u;
    ast->slots[0].state = 2u;
    ast->slots[0].group_count = 1u;
    ast->slots[0].group_list[0] = 0x2F4D61u;  /* M2 GSSI */
}

/* ---------------------------------------------------------------------------
 * Test 1: clean shutdown → snapshot exists → reload succeeds. */
static void test_clean_shutdown_then_reload(void)
{
    Ast snapshot;
    fill_sample_ast(&snapshot);
    int rc = ast_snapshot(&snapshot, s_ast_path);
    TEST_ASSERT_EQUAL(0, rc);
    /* Verify the file is present. */
    struct stat sb;
    TEST_ASSERT_EQUAL(0, stat(s_ast_path, &sb));
    TEST_ASSERT_TRUE(sb.st_size > 0);

    Ast reloaded; bool loaded = false;
    memset(&reloaded, 0, sizeof(reloaded));
    rc = ast_reload(&reloaded, s_ast_path, &loaded);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(reloaded.slots[0].valid);
    TEST_ASSERT_EQUAL(0x282FF4u, reloaded.slots[0].issi);
    TEST_ASSERT_EQUAL(1234u, reloaded.slots[0].last_seen_multiframe);
    TEST_ASSERT_EQUAL(7u, reloaded.slots[0].shadow_idx);
    TEST_ASSERT_EQUAL(1u, reloaded.slots[0].group_count);
    TEST_ASSERT_EQUAL(0x2F4D61u, reloaded.slots[0].group_list[0]);
}

/* ---------------------------------------------------------------------------
 * Test 2: kill mid-loop (no snapshot file ever written) → reload
 * leaves AST untouched + reports loaded=false. */
static void test_kill_mid_loop_leaves_ast_inert(void)
{
    /* Verify file is NOT present (setUp unlinked it). */
    struct stat sb;
    TEST_ASSERT_EQUAL(-1, stat(s_ast_path, &sb));

    Ast loaded_ast;
    /* Pre-fill so we can detect "ast_reload left it alone". */
    fill_sample_ast(&loaded_ast);
    bool loaded = true;  /* clear-flag input */
    int rc = ast_reload(&loaded_ast, s_ast_path, &loaded);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_FALSE(loaded);
    /* Per contract, ast_reload zeroes the struct on no-load. */
    TEST_ASSERT_FALSE(loaded_ast.slots[0].valid);
}

/* ---------------------------------------------------------------------------
 * Test 3: explicit unclean snapshot (clean_shutdown_flag=false) →
 * reload refuses to populate. We simulate by writing a snapshot then
 * tampering with the JSON to set clean_shutdown_flag=false. */
static void test_unclean_snapshot_refused(void)
{
    Ast a; fill_sample_ast(&a);
    int rc = ast_snapshot(&a, s_ast_path);
    TEST_ASSERT_EQUAL(0, rc);

    /* Slurp + tamper. ast_snapshot writes a JSON blob; we replace
     * `"clean_shutdown_flag":true` with `"clean_shutdown_flag":false`.
     * The file is small (kB-range). */
    FILE *f = fopen(s_ast_path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *body = malloc((size_t) sz + 1);
    TEST_ASSERT_NOT_NULL(body);
    size_t got = fread(body, 1, (size_t) sz, f);
    body[got] = '\0';
    fclose(f);

    /* jansson by default emits one-space-after-colon ("k": v); accept
     * both forms by locating the key first and the value separately. */
    char *p = strstr(body, "\"clean_shutdown_flag\"");
    TEST_ASSERT_NOT_NULL_MESSAGE(p, "ast_snapshot must emit clean_shutdown_flag key");
    char *t = strstr(p, "true");
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "ast_snapshot must emit clean_shutdown_flag=true");
    /* Sanity: the next non-whitespace after the key is `:`, then optional
     * spaces, then `true` — guard against accidentally matching a later
     * "true" elsewhere by ensuring no `,` or `}` between p and t. */
    for (char *q = p; q < t; q++) {
        TEST_ASSERT_TRUE(*q != ',' && *q != '}');
    }
    /* In-place rewrite of `true` → `fals` (4 bytes, same width as `true`
     * minus the 'e'). Length contract: we replace the 4-byte token with
     * exactly 4 bytes (`fals`) plus shift the trailing 'e' boundary by
     * 1. Simpler: replace 4 bytes "true" with 4 bytes "fals" and prepend
     * a 1-byte mismatch by shifting tail down 1 byte to make room for
     * "false". The resulting string is 1 byte longer. */
    size_t total = (size_t) sz + 1u;
    char *out = malloc(total + 1);
    size_t prefix_len = (size_t) (t - body);
    memcpy(out, body, prefix_len);
    memcpy(out + prefix_len, "false", 5);
    const char *tail = t + 4;     /* skip "true" */
    memcpy(out + prefix_len + 5, tail, strlen(tail));
    out[total] = '\0';

    f = fopen(s_ast_path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite(out, 1, total, f);
    fclose(f);
    free(body);
    free(out);

    Ast reloaded; bool loaded = true;
    memset(&reloaded, 0, sizeof(reloaded));
    rc = ast_reload(&reloaded, s_ast_path, &loaded);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_FALSE(loaded);
    TEST_ASSERT_FALSE(reloaded.slots[0].valid);
}

/* ---------------------------------------------------------------------------
 * Test 4: snapshot+reload cycle is idempotent (round-trip). */
static void test_snapshot_reload_round_trip_is_stable(void)
{
    Ast a; fill_sample_ast(&a);
    TEST_ASSERT_EQUAL(0, ast_snapshot(&a, s_ast_path));
    Ast b; bool loaded = false;
    TEST_ASSERT_EQUAL(0, ast_reload(&b, s_ast_path, &loaded));
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_EQUAL(0, ast_snapshot(&b, s_ast_path));
    Ast c; loaded = false;
    TEST_ASSERT_EQUAL(0, ast_reload(&c, s_ast_path, &loaded));
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_EQUAL(b.slots[0].issi, c.slots[0].issi);
    TEST_ASSERT_EQUAL(b.slots[0].last_seen_multiframe,
                      c.slots[0].last_seen_multiframe);
    TEST_ASSERT_EQUAL(b.slots[0].group_count, c.slots[0].group_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_clean_shutdown_then_reload);
    RUN_TEST(test_kill_mid_loop_leaves_ast_inert);
    RUN_TEST(test_unclean_snapshot_refused);
    RUN_TEST(test_snapshot_reload_round_trip_is_stable);
    return UNITY_END();
}
