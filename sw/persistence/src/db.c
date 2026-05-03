/* sw/persistence/src/db.c — Subscriber-DB implementation (IF_DB_API_v1).
 *
 * Owned by S5 (S5-sw-persistence). Backing store is a single JSON file
 * (Decision #8) at the path passed to db_open(); writes are atomic via
 * tmp + rename. Profile 0 is enforced read-only with bit-exact value
 * TETRA_DB_PROFILE0_BITS per ARCHITECTURE.md §"Subscriber-DB".
 *
 * JSON shape (compact, sorted keys for stable diffs):
 *   {
 *     "version": 1,
 *     "profiles": [
 *       { "id": 0, "bits": 2191 },        // 0x088F
 *       { "id": 1, "bits": ... },
 *       ...
 *     ],
 *     "entities": [
 *       { "idx": 0, "entity_id": 2633716, "entity_type": 0,
 *         "profile_id": 1, "reserved": 0, "valid": true },
 *       ...
 *     ]
 *   }
 *
 * Profiles are stored as their packed 32-bit integer ("bits") rather than
 * as a struct of named fields, because the on-air semantics are the bit
 * pattern; named expansion is a UI concern (S6 webui CGIs decode for the
 * editor view). Entities use named fields because shadow_idx, profile_id
 * and entity_type are small enums + the reserved34 area is preserved
 * bit-exact via a single uint64.
 */

#define _POSIX_C_SOURCE 200809L

#include "tetra/db.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <jansson.h>

/* ---------------------------------------------------------------------------
 * Profile pack / unpack — bit-exact to reference_subscriber_db_arch.md.
 * ------------------------------------------------------------------------- */

uint32_t profile_pack(const Profile *p)
{
    uint32_t v = 0;
    v |= ((uint32_t) p->max_call_duration & 0xFFu) << 24;
    v |= ((uint32_t) p->hangtime          & 0xFFu) << 16;
    v |= ((uint32_t) p->priority          & 0x0Fu) << 12;
    v |= ((uint32_t) p->gila_class        & 0x07u) <<  9;
    v |= ((uint32_t) p->gila_lifetime     & 0x03u) <<  7;
    v |= ((uint32_t) p->reserved3         & 0x07u) <<  4;
    v |= ((uint32_t) (p->permit_voice ? 1u : 0u))  <<  3;
    v |= ((uint32_t) (p->permit_data  ? 1u : 0u))  <<  2;
    v |= ((uint32_t) (p->permit_reg   ? 1u : 0u))  <<  1;
    v |= ((uint32_t) (p->valid        ? 1u : 0u));
    return v;
}

void profile_unpack(uint32_t bits, Profile *out)
{
    out->max_call_duration = (uint8_t) ((bits >> 24) & 0xFFu);
    out->hangtime          = (uint8_t) ((bits >> 16) & 0xFFu);
    out->priority          = (uint8_t) ((bits >> 12) & 0x0Fu);
    out->gila_class        = (uint8_t) ((bits >>  9) & 0x07u);
    out->gila_lifetime     = (uint8_t) ((bits >>  7) & 0x03u);
    out->reserved3         = (uint8_t) ((bits >>  4) & 0x07u);
    out->permit_voice      = ((bits >> 3) & 1u) != 0;
    out->permit_data       = ((bits >> 2) & 1u) != 0;
    out->permit_reg        = ((bits >> 1) & 1u) != 0;
    out->valid             = (bits        & 1u) != 0;
}

/* ---------------------------------------------------------------------------
 * Internal helpers.
 * ------------------------------------------------------------------------- */

static void db_init_empty(SubscriberDb *db)
{
    memset(db->profiles, 0, sizeof(db->profiles));
    memset(db->entities, 0, sizeof(db->entities));
    profile_unpack(TETRA_DB_PROFILE0_BITS, &db->profiles[0]);
    db->opened = true;
}

static int json_get_uint(json_t *obj, const char *key, uint64_t *out, uint64_t max)
{
    json_t *v = json_object_get(obj, key);
    if (!v || !json_is_integer(v)) {
        return -EINVAL;
    }
    json_int_t i = json_integer_value(v);
    if (i < 0) {
        return -EINVAL;
    }
    if ((uint64_t) i > max) {
        return -EINVAL;
    }
    *out = (uint64_t) i;
    return 0;
}

static int json_get_bool(json_t *obj, const char *key, bool *out)
{
    json_t *v = json_object_get(obj, key);
    if (!v || !json_is_boolean(v)) {
        return -EINVAL;
    }
    *out = json_is_true(v);
    return 0;
}

/* Atomic write: open <path>.tmp, write all, fsync, close, rename onto <path>.
 * On any failure unlinks <path>.tmp and returns -EIO.
 */
static int atomic_write_text(const char *path, const char *text)
{
    char tmp[TETRA_DB_PATH_MAX + 8];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t) n >= sizeof(tmp)) {
        return -EINVAL;
    }

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return -EIO;
    }

    size_t total = strlen(text);
    size_t written = 0;
    while (written < total) {
        ssize_t w = write(fd, text + written, total - written);
        if (w < 0) {
            (void) close(fd);
            (void) unlink(tmp);
            return -EIO;
        }
        written += (size_t) w;
    }

    if (fsync(fd) < 0) {
        (void) close(fd);
        (void) unlink(tmp);
        return -EIO;
    }
    if (close(fd) < 0) {
        (void) unlink(tmp);
        return -EIO;
    }

    if (rename(tmp, path) < 0) {
        (void) unlink(tmp);
        return -EIO;
    }
    return 0;
}

/* Remove a stale <path>.tmp left behind by a crashed prior write. The
 * primary file at `path` is untouched — atomic rename guarantees it is
 * either the old version or the new version, never a torn one.
 */
static void cleanup_stale_tmp(const char *path)
{
    char tmp[TETRA_DB_PATH_MAX + 8];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t) n >= sizeof(tmp)) {
        return;
    }
    struct stat st;
    if (stat(tmp, &st) == 0) {
        (void) unlink(tmp);
    }
}

/* ---------------------------------------------------------------------------
 * JSON deserialization.
 *
 * Returns 0 on success, -EINVAL on schema violation. On schema violation the
 * caller-supplied SubscriberDb is left in the empty-init state.
 * ------------------------------------------------------------------------- */
static int db_load_json(SubscriberDb *db, json_t *root)
{
    if (!json_is_object(root)) {
        return -EINVAL;
    }

    uint64_t version = 0;
    if (json_get_uint(root, "version", &version, 1) != 0 || version != 1) {
        return -EINVAL;
    }

    json_t *profiles = json_object_get(root, "profiles");
    json_t *entities = json_object_get(root, "entities");
    if (!json_is_array(profiles) || !json_is_array(entities)) {
        return -EINVAL;
    }

    db_init_empty(db);

    size_t i;
    json_t *item;
    json_array_foreach(profiles, i, item) {
        if (!json_is_object(item)) {
            return -EINVAL;
        }
        uint64_t id = 0;
        uint64_t bits = 0;
        if (json_get_uint(item, "id", &id, TETRA_DB_PROFILE_COUNT - 1) != 0) {
            return -EINVAL;
        }
        if (json_get_uint(item, "bits", &bits, 0xFFFFFFFFu) != 0) {
            return -EINVAL;
        }
        if (id == 0) {
            /* Profile 0 invariant — silently corrected. */
            continue;
        }
        profile_unpack((uint32_t) bits, &db->profiles[id]);
    }

    json_array_foreach(entities, i, item) {
        if (!json_is_object(item)) {
            return -EINVAL;
        }
        uint64_t idx = 0;
        uint64_t entity_id = 0;
        uint64_t entity_type = 0;
        uint64_t profile_id = 0;
        uint64_t reserved34 = 0;
        bool valid = false;
        if (json_get_uint(item, "idx", &idx, TETRA_DB_ENTITY_COUNT - 1) != 0) {
            return -EINVAL;
        }
        if (json_get_uint(item, "entity_id", &entity_id, 0xFFFFFFu) != 0) {
            return -EINVAL;
        }
        if (json_get_uint(item, "entity_type", &entity_type, 1) != 0) {
            return -EINVAL;
        }
        if (json_get_uint(item, "profile_id", &profile_id,
                          TETRA_DB_PROFILE_COUNT - 1) != 0) {
            return -EINVAL;
        }
        if (json_get_uint(item, "reserved", &reserved34,
                          (1ull << 34) - 1ull) != 0) {
            return -EINVAL;
        }
        if (json_get_bool(item, "valid", &valid) != 0) {
            return -EINVAL;
        }
        Entity *e = &db->entities[idx];
        e->entity_id   = (uint32_t) entity_id;
        e->entity_type = (uint8_t) entity_type;
        e->profile_id  = (uint8_t) profile_id;
        e->reserved34  = reserved34;
        e->valid       = valid;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * JSON serialization. Returns malloc'd C-string, caller frees. NULL on OOM.
 * ------------------------------------------------------------------------- */
static char *db_dump_json(const SubscriberDb *db)
{
    json_t *root = json_object();
    if (!root) return NULL;

    if (json_object_set_new(root, "version", json_integer(1)) < 0) {
        json_decref(root);
        return NULL;
    }

    json_t *profiles = json_array();
    if (!profiles || json_object_set_new(root, "profiles", profiles) < 0) {
        json_decref(root);
        return NULL;
    }
    for (uint8_t i = 0; i < TETRA_DB_PROFILE_COUNT; i++) {
        uint32_t bits = (i == 0)
                            ? TETRA_DB_PROFILE0_BITS
                            : profile_pack(&db->profiles[i]);
        json_t *o = json_pack("{s:i,s:i}", "id", (int) i, "bits", (int) bits);
        if (!o || json_array_append_new(profiles, o) < 0) {
            json_decref(root);
            return NULL;
        }
    }

    json_t *entities = json_array();
    if (!entities || json_object_set_new(root, "entities", entities) < 0) {
        json_decref(root);
        return NULL;
    }
    for (uint16_t i = 0; i < TETRA_DB_ENTITY_COUNT; i++) {
        const Entity *e = &db->entities[i];
        if (!e->valid && e->entity_id == 0 && e->reserved34 == 0
                && e->profile_id == 0 && e->entity_type == 0) {
            /* Skip empty slots — keep the JSON small. */
            continue;
        }
        json_t *o = json_pack(
            "{s:i,s:I,s:i,s:i,s:I,s:b}",
            "idx", (int) i,
            "entity_id", (json_int_t) e->entity_id,
            "entity_type", (int) e->entity_type,
            "profile_id", (int) e->profile_id,
            "reserved", (json_int_t) e->reserved34,
            "valid", e->valid ? 1 : 0);
        if (!o || json_array_append_new(entities, o) < 0) {
            json_decref(root);
            return NULL;
        }
    }

    char *text = json_dumps(root, JSON_INDENT(2) | JSON_SORT_KEYS);
    json_decref(root);
    return text;
}

/* ---------------------------------------------------------------------------
 * Public API.
 * ------------------------------------------------------------------------- */

int db_open(SubscriberDb *db, const char *path)
{
    if (!db || !path) {
        return -EINVAL;
    }
    size_t plen = strlen(path);
    if (plen == 0 || plen >= TETRA_DB_PATH_MAX) {
        return -EINVAL;
    }

    memset(db, 0, sizeof(*db));
    memcpy(db->path, path, plen);
    db->path[plen] = '\0';

    cleanup_stale_tmp(path);

    struct stat st;
    if (stat(path, &st) != 0) {
        /* Fresh start — no file yet. */
        db_init_empty(db);
        return 0;
    }

    json_error_t err;
    json_t *root = json_load_file(path, 0, &err);
    if (!root) {
        return -EIO;
    }

    int rc = db_load_json(db, root);
    json_decref(root);

    if (rc != 0) {
        /* Schema violation: leave caller's struct in a defined state and
         * report the error so the operator sees it. We do NOT auto-heal
         * a malformed file — the SIGTERM-clean case is recoverable; a
         * malformed file is operator-action territory. */
        memset(db, 0, sizeof(*db));
        return rc;
    }

    /* Re-assert Profile 0 invariant unconditionally on load. */
    profile_unpack(TETRA_DB_PROFILE0_BITS, &db->profiles[0]);
    return 0;
}

int db_get_profile(SubscriberDb *db, uint8_t id, Profile *out)
{
    if (!db || !out || !db->opened) {
        return -EINVAL;
    }
    if (id >= TETRA_DB_PROFILE_COUNT) {
        return -EINVAL;
    }
    *out = db->profiles[id];
    return 0;
}

int db_put_profile(SubscriberDb *db, uint8_t id, const Profile *p)
{
    if (!db || !p || !db->opened) {
        return -EINVAL;
    }
    if (id >= TETRA_DB_PROFILE_COUNT) {
        return -EINVAL;
    }
    if (id == 0) {
        return -EPERM;  /* Profile 0 is the M2 GILA-Guard invariant. */
    }
    db->profiles[id] = *p;
    return 0;
}

int db_get_entity(SubscriberDb *db, uint16_t idx, Entity *out)
{
    if (!db || !out || !db->opened) {
        return -EINVAL;
    }
    if (idx >= TETRA_DB_ENTITY_COUNT) {
        return -EINVAL;
    }
    *out = db->entities[idx];
    return 0;
}

int db_put_entity(SubscriberDb *db, uint16_t idx, const Entity *e)
{
    if (!db || !e || !db->opened) {
        return -EINVAL;
    }
    if (idx >= TETRA_DB_ENTITY_COUNT) {
        return -EINVAL;
    }
    if (e->profile_id >= TETRA_DB_PROFILE_COUNT) {
        return -EINVAL;
    }
    if (e->entity_type > 1) {
        return -EINVAL;
    }
    if ((e->entity_id & ~0x00FFFFFFu) != 0) {
        return -EINVAL;
    }
    db->entities[idx] = *e;
    return 0;
}

int db_lookup_entity(SubscriberDb *db, uint32_t entity_id, uint16_t *out_idx)
{
    if (!db || !out_idx || !db->opened) {
        return -EINVAL;
    }
    for (uint16_t i = 0; i < TETRA_DB_ENTITY_COUNT; i++) {
        const Entity *e = &db->entities[i];
        if (e->valid && e->entity_id == entity_id) {
            *out_idx = i;
            return 0;
        }
    }
    return -ENOENT;
}

int db_atomic_save(SubscriberDb *db)
{
    if (!db || !db->opened || db->path[0] == '\0') {
        return -EINVAL;
    }
    char *text = db_dump_json(db);
    if (!text) {
        return -ENOMEM;
    }
    int rc = atomic_write_text(db->path, text);
    free(text);
    return rc;
}
