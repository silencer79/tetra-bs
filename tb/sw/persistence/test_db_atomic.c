/* tb/sw/persistence/test_db_atomic.c — S5 unit tests.
 *
 * Owned by S5 (S5-sw-persistence). Test gate per
 * docs/MIGRATION_PLAN.md §S5:
 *
 *   - write+rename round-trip
 *   - kill-mid-write simulated by writing partial tmpfile + crash:
 *     next open clean (alte DB intakt, partial tmpfile zurueckgeraeumt)
 *   - Profile-0 read-only enforced
 *   - schema-violation rejected (missing key, wrong type)
 *   - AST snapshot/reload round-trip with clean_shutdown_flag=true reload OK
 *   - AST snapshot/reload with clean_shutdown_flag=false -> reload returns
 *     out_loaded=false
 *
 * Each test case uses a private tmpdir under /tmp/tetra_db_test_XXXXXX and
 * cleans up in tearDown(). Unity main() returns 0 on all-pass.
 */

#define _XOPEN_SOURCE 700  /* mkdtemp + POSIX.1-2008 unistd */

#include "tetra/db.h"
#include "unity.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char g_tmpdir[64];
static char g_dbpath[128];
static char g_astpath[128];

static void rm_if_exists(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        (void) unlink(path);
    }
}

void setUp(void)
{
    strcpy(g_tmpdir, "/tmp/tetra_db_test_XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(g_tmpdir));
    snprintf(g_dbpath, sizeof(g_dbpath), "%s/db.json", g_tmpdir);
    snprintf(g_astpath, sizeof(g_astpath), "%s/ast.json", g_tmpdir);
}

void tearDown(void)
{
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_dbpath);
    rm_if_exists(tmp);
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_astpath);
    rm_if_exists(tmp);
    rm_if_exists(g_dbpath);
    rm_if_exists(g_astpath);
    (void) rmdir(g_tmpdir);
}

/* ---------------------------------------------------------------------------
 * Helper: write a raw text blob to a file (used for schema-violation +
 * partial-tmp-file simulation).
 * ------------------------------------------------------------------------- */
static void write_file(const char *path, const char *body)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT_TRUE(fd >= 0);
    size_t len = strlen(body);
    ssize_t w = write(fd, body, len);
    TEST_ASSERT_EQUAL_INT((ssize_t) len, w);
    TEST_ASSERT_EQUAL_INT(0, close(fd));
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* ---------------------------------------------------------------------------
 * Test: profile_pack/unpack bit-exact + Profile-0 invariant value.
 * ------------------------------------------------------------------------- */
static void test_profile_pack_roundtrip(void)
{
    Profile p0;
    profile_unpack(TETRA_DB_PROFILE0_BITS, &p0);
    TEST_ASSERT_EQUAL_HEX32(TETRA_DB_PROFILE0_BITS, profile_pack(&p0));

    Profile p = {
        .max_call_duration = 0xAB,
        .hangtime          = 0xCD,
        .priority          = 0x0E,
        .gila_class        = 5,    /* 3 bit */
        .gila_lifetime     = 2,    /* 2 bit */
        .reserved3         = 3,    /* 3 bit */
        .permit_voice      = true,
        .permit_data       = false,
        .permit_reg        = true,
        .valid             = true,
    };
    uint32_t bits = profile_pack(&p);
    Profile q;
    profile_unpack(bits, &q);
    TEST_ASSERT_EQUAL_UINT8(p.max_call_duration, q.max_call_duration);
    TEST_ASSERT_EQUAL_UINT8(p.hangtime, q.hangtime);
    TEST_ASSERT_EQUAL_UINT8(p.priority, q.priority);
    TEST_ASSERT_EQUAL_UINT8(p.gila_class, q.gila_class);
    TEST_ASSERT_EQUAL_UINT8(p.gila_lifetime, q.gila_lifetime);
    TEST_ASSERT_EQUAL_UINT8(p.reserved3, q.reserved3);
    TEST_ASSERT_TRUE(q.permit_voice);
    TEST_ASSERT_FALSE(q.permit_data);
    TEST_ASSERT_TRUE(q.permit_reg);
    TEST_ASSERT_TRUE(q.valid);
}

/* ---------------------------------------------------------------------------
 * Test: db_open() on non-existent file -> empty DB, Profile 0 = invariant.
 * ------------------------------------------------------------------------- */
static void test_open_fresh_initialises_profile0(void)
{
    SubscriberDb db;
    TEST_ASSERT_EQUAL_INT(0, db_open(&db, g_dbpath));

    Profile p0;
    TEST_ASSERT_EQUAL_INT(0, db_get_profile(&db, 0, &p0));
    TEST_ASSERT_EQUAL_HEX32(TETRA_DB_PROFILE0_BITS, profile_pack(&p0));

    /* Other profiles default to all-zero (valid=false). */
    for (uint8_t i = 1; i < TETRA_DB_PROFILE_COUNT; i++) {
        Profile p;
        TEST_ASSERT_EQUAL_INT(0, db_get_profile(&db, i, &p));
        TEST_ASSERT_FALSE(p.valid);
    }
}

/* ---------------------------------------------------------------------------
 * Test: write -> save -> open round-trip; profiles+entities preserved
 * bit-exact. Also asserts atomic-rename (final file present, no leftover
 * .tmp).
 * ------------------------------------------------------------------------- */
static void test_save_open_roundtrip(void)
{
    SubscriberDb db;
    TEST_ASSERT_EQUAL_INT(0, db_open(&db, g_dbpath));

    Profile p1 = {
        .max_call_duration = 60,
        .hangtime = 50,
        .priority = 3,
        .gila_class    = 4,    /* M2 default */
        .gila_lifetime = 1,    /* M2 default */
        .reserved3     = 0,
        .permit_voice = true,
        .permit_data  = true,
        .permit_reg   = true,
        .valid        = true,
    };
    TEST_ASSERT_EQUAL_INT(0, db_put_profile(&db, 1, &p1));

    Entity e = {
        .entity_id   = 0x282FF4u,
        .entity_type = 0,
        .profile_id  = 1,
        .reserved34  = 0x12345u,
        .valid       = true,
    };
    TEST_ASSERT_EQUAL_INT(0, db_put_entity(&db, 5, &e));

    TEST_ASSERT_EQUAL_INT(0, db_atomic_save(&db));
    TEST_ASSERT_TRUE(file_exists(g_dbpath));
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_dbpath);
    TEST_ASSERT_FALSE(file_exists(tmp));

    SubscriberDb db2;
    TEST_ASSERT_EQUAL_INT(0, db_open(&db2, g_dbpath));

    Profile got;
    TEST_ASSERT_EQUAL_INT(0, db_get_profile(&db2, 1, &got));
    TEST_ASSERT_EQUAL_HEX32(profile_pack(&p1), profile_pack(&got));

    Entity got_e;
    TEST_ASSERT_EQUAL_INT(0, db_get_entity(&db2, 5, &got_e));
    TEST_ASSERT_EQUAL_UINT32(e.entity_id, got_e.entity_id);
    TEST_ASSERT_EQUAL_UINT8(e.entity_type, got_e.entity_type);
    TEST_ASSERT_EQUAL_UINT8(e.profile_id, got_e.profile_id);
    TEST_ASSERT_EQUAL_UINT64(e.reserved34, got_e.reserved34);
    TEST_ASSERT_TRUE(got_e.valid);

    /* Profile 0 still bit-exact after round-trip. */
    Profile p0;
    TEST_ASSERT_EQUAL_INT(0, db_get_profile(&db2, 0, &p0));
    TEST_ASSERT_EQUAL_HEX32(TETRA_DB_PROFILE0_BITS, profile_pack(&p0));

    uint16_t idx = 0xFFFF;
    TEST_ASSERT_EQUAL_INT(0, db_lookup_entity(&db2, 0x282FF4u, &idx));
    TEST_ASSERT_EQUAL_UINT16(5, idx);

    TEST_ASSERT_EQUAL_INT(-ENOENT, db_lookup_entity(&db2, 0xDEADBE, &idx));
}

/* ---------------------------------------------------------------------------
 * Test: Profile-0 read-only enforced (-EPERM on db_put_profile(0, ...)).
 * ------------------------------------------------------------------------- */
static void test_profile0_readonly(void)
{
    SubscriberDb db;
    TEST_ASSERT_EQUAL_INT(0, db_open(&db, g_dbpath));

    Profile p = {
        .max_call_duration = 1,
        .hangtime = 1,
        .priority = 1,
        .gila_class = 0,
        .gila_lifetime = 0,
        .reserved3 = 0,
        .permit_voice = false,
        .permit_data  = false,
        .permit_reg   = false,
        .valid        = true,
    };
    TEST_ASSERT_EQUAL_INT(-EPERM, db_put_profile(&db, 0, &p));

    /* Profile 0 still untouched and bit-exact. */
    Profile p0;
    TEST_ASSERT_EQUAL_INT(0, db_get_profile(&db, 0, &p0));
    TEST_ASSERT_EQUAL_HEX32(TETRA_DB_PROFILE0_BITS, profile_pack(&p0));
}

/* ---------------------------------------------------------------------------
 * Test: kill-mid-write simulation. We:
 *   1. Save a clean DB to disk.
 *   2. Manually write a partial+garbage `<path>.tmp` to mimic a crash mid-
 *      write (rename has not yet happened).
 *   3. db_open() must succeed, return the original DB intact, and clean up
 *      the partial .tmp.
 * ------------------------------------------------------------------------- */
static void test_partial_tmpfile_recovered(void)
{
    SubscriberDb db;
    TEST_ASSERT_EQUAL_INT(0, db_open(&db, g_dbpath));

    Entity e = {
        .entity_id = 0x111111u, .entity_type = 0, .profile_id = 1,
        .reserved34 = 0, .valid = true,
    };
    TEST_ASSERT_EQUAL_INT(0, db_put_entity(&db, 0, &e));
    TEST_ASSERT_EQUAL_INT(0, db_atomic_save(&db));

    /* Drop a deliberately-truncated tmp file as if the daemon was killed
     * after open() but before rename(). The original file remains intact. */
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_dbpath);
    write_file(tmp, "{ \"version\": 1, \"profiles\": [");

    /* Re-open: must succeed; partial .tmp must be cleaned up. */
    SubscriberDb db2;
    TEST_ASSERT_EQUAL_INT(0, db_open(&db2, g_dbpath));
    TEST_ASSERT_FALSE(file_exists(tmp));

    Entity got;
    TEST_ASSERT_EQUAL_INT(0, db_get_entity(&db2, 0, &got));
    TEST_ASSERT_EQUAL_UINT32(e.entity_id, got.entity_id);
    TEST_ASSERT_TRUE(got.valid);
}

/* ---------------------------------------------------------------------------
 * Test: schema-violation rejected — missing top-level key and wrong type.
 * ------------------------------------------------------------------------- */
static void test_schema_violation_missing_key(void)
{
    /* Missing "version" key. */
    write_file(g_dbpath, "{ \"profiles\": [], \"entities\": [] }");
    SubscriberDb db;
    TEST_ASSERT_EQUAL_INT(-EINVAL, db_open(&db, g_dbpath));
}

static void test_schema_violation_wrong_type(void)
{
    /* "profiles" is an object, not an array — schema violation. */
    write_file(g_dbpath,
               "{ \"version\": 1, \"profiles\": {}, \"entities\": [] }");
    SubscriberDb db;
    TEST_ASSERT_EQUAL_INT(-EINVAL, db_open(&db, g_dbpath));
}

static void test_schema_violation_entity_field(void)
{
    /* Entity item missing "valid" key. */
    write_file(g_dbpath,
               "{ \"version\": 1, \"profiles\": [],"
               " \"entities\": [{\"idx\":0,\"entity_id\":1,"
               "\"entity_type\":0,\"profile_id\":0,\"reserved\":0}] }");
    SubscriberDb db;
    TEST_ASSERT_EQUAL_INT(-EINVAL, db_open(&db, g_dbpath));
}

/* ---------------------------------------------------------------------------
 * Test: db_put_entity argument validation.
 * ------------------------------------------------------------------------- */
static void test_put_entity_invalid_args(void)
{
    SubscriberDb db;
    TEST_ASSERT_EQUAL_INT(0, db_open(&db, g_dbpath));

    Entity bad_pid = { .entity_id=1, .profile_id=99,
                       .entity_type=0, .reserved34=0, .valid=true };
    TEST_ASSERT_EQUAL_INT(-EINVAL, db_put_entity(&db, 0, &bad_pid));

    Entity bad_type = { .entity_id=1, .profile_id=0,
                        .entity_type=2, .reserved34=0, .valid=true };
    TEST_ASSERT_EQUAL_INT(-EINVAL, db_put_entity(&db, 0, &bad_type));

    Entity bad_id = { .entity_id=0xFF000000u, .profile_id=0,
                      .entity_type=0, .reserved34=0, .valid=true };
    TEST_ASSERT_EQUAL_INT(-EINVAL, db_put_entity(&db, 0, &bad_id));
}

/* ---------------------------------------------------------------------------
 * Test: AST snapshot+reload round-trip with clean_shutdown_flag=true.
 * ------------------------------------------------------------------------- */
static void test_ast_snapshot_reload_roundtrip(void)
{
    Ast a = (Ast){0};
    a.slots[3].issi                 = 0x282FF4u;
    a.slots[3].last_seen_multiframe = 0x123456u;
    a.slots[3].shadow_idx           = 7;
    a.slots[3].state                = 2;
    a.slots[3].group_count          = 2;
    a.slots[3].group_list[0]        = 0x2F4D61u;
    a.slots[3].group_list[1]        = 0x2F4D62u;
    a.slots[3].valid                = true;

    a.slots[10].issi  = 0xABCDEFu;
    a.slots[10].state = 1;
    a.slots[10].valid = true;

    TEST_ASSERT_EQUAL_INT(0, ast_snapshot(&a, g_astpath));
    TEST_ASSERT_TRUE(file_exists(g_astpath));

    Ast b;
    bool loaded = false;
    TEST_ASSERT_EQUAL_INT(0, ast_reload(&b, g_astpath, &loaded));
    TEST_ASSERT_TRUE(loaded);

    TEST_ASSERT_TRUE(b.slots[3].valid);
    TEST_ASSERT_EQUAL_UINT32(0x282FF4u, b.slots[3].issi);
    TEST_ASSERT_EQUAL_UINT32(0x123456u, b.slots[3].last_seen_multiframe);
    TEST_ASSERT_EQUAL_UINT8(7, b.slots[3].shadow_idx);
    TEST_ASSERT_EQUAL_UINT8(2, b.slots[3].state);
    TEST_ASSERT_EQUAL_UINT8(2, b.slots[3].group_count);
    TEST_ASSERT_EQUAL_UINT32(0x2F4D61u, b.slots[3].group_list[0]);
    TEST_ASSERT_EQUAL_UINT32(0x2F4D62u, b.slots[3].group_list[1]);

    TEST_ASSERT_TRUE(b.slots[10].valid);
    TEST_ASSERT_EQUAL_UINT32(0xABCDEFu, b.slots[10].issi);

    /* Untouched slots remain zeroed. */
    TEST_ASSERT_FALSE(b.slots[0].valid);
    TEST_ASSERT_FALSE(b.slots[63].valid);
}

/* ---------------------------------------------------------------------------
 * Test: AST reload sees clean_shutdown_flag=false -> out_loaded=false,
 * AST left zeroed, return code 0 (not an error).
 * ------------------------------------------------------------------------- */
static void test_ast_reload_skips_dirty_flag(void)
{
    write_file(g_astpath,
               "{ \"version\": 1, \"clean_shutdown_flag\": false,"
               " \"slots\": [] }");
    Ast a;
    /* Pre-pollute caller buffer to confirm reload zeroes it. */
    memset(&a, 0xAA, sizeof(a));
    bool loaded = true;
    TEST_ASSERT_EQUAL_INT(0, ast_reload(&a, g_astpath, &loaded));
    TEST_ASSERT_FALSE(loaded);
    for (size_t i = 0; i < TETRA_AST_SLOT_COUNT; i++) {
        TEST_ASSERT_FALSE(a.slots[i].valid);
        TEST_ASSERT_EQUAL_UINT32(0, a.slots[i].issi);
    }
}

/* ---------------------------------------------------------------------------
 * Test: AST reload on missing file -> out_loaded=false, rc=0.
 * ------------------------------------------------------------------------- */
static void test_ast_reload_missing_file(void)
{
    Ast a;
    bool loaded = true;
    TEST_ASSERT_EQUAL_INT(0, ast_reload(&a, g_astpath, &loaded));
    TEST_ASSERT_FALSE(loaded);
}

/* ---------------------------------------------------------------------------
 * Test: AST reload on malformed file (flag present + true but bad structure)
 * returns -EIO.
 * ------------------------------------------------------------------------- */
static void test_ast_reload_malformed_file(void)
{
    write_file(g_astpath, "{ this is not json");
    Ast a;
    bool loaded = true;
    TEST_ASSERT_EQUAL_INT(-EIO, ast_reload(&a, g_astpath, &loaded));
    TEST_ASSERT_FALSE(loaded);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_profile_pack_roundtrip);
    RUN_TEST(test_open_fresh_initialises_profile0);
    RUN_TEST(test_save_open_roundtrip);
    RUN_TEST(test_profile0_readonly);
    RUN_TEST(test_partial_tmpfile_recovered);
    RUN_TEST(test_schema_violation_missing_key);
    RUN_TEST(test_schema_violation_wrong_type);
    RUN_TEST(test_schema_violation_entity_field);
    RUN_TEST(test_put_entity_invalid_args);
    RUN_TEST(test_ast_snapshot_reload_roundtrip);
    RUN_TEST(test_ast_reload_skips_dirty_flag);
    RUN_TEST(test_ast_reload_missing_file);
    RUN_TEST(test_ast_reload_malformed_file);
    return UNITY_END();
}
