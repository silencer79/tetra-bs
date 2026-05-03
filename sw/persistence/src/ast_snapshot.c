/* sw/persistence/src/ast_snapshot.c — AST snapshot/reload (IF_AST_PERSIST_v1).
 *
 * Owned by S5 (S5-sw-persistence). Implements Decision #10:
 *   - SIGTERM handler calls ast_snapshot() to write the AST + a
 *     clean_shutdown_flag=true marker atomically (tmp + rename).
 *   - Daemon start calls ast_reload(); the AST is repopulated ONLY if
 *     the file exists, parses, and clean_shutdown_flag == true. Otherwise
 *     the AST is left zeroed and *out_loaded=false. A crash always leaves
 *     either no file (first run) or a flag=false file (overwritten by the
 *     next clean shutdown only).
 *
 * JSON shape:
 *   {
 *     "version": 1,
 *     "clean_shutdown_flag": true,
 *     "slots": [
 *       { "idx": 0, "issi": 2633716, "last_seen_multiframe": 12345,
 *         "shadow_idx": 7, "state": 1, "group_count": 2,
 *         "group_list": [3098465, 3098470], "valid": true },
 *       ...
 *     ]
 *   }
 *
 * Invalid (valid=false) slots are not serialised; ast_reload() leaves
 * unmentioned slot indices zero-initialised. The clean_shutdown_flag
 * lives inside the JSON document — there is no separate marker file —
 * because rename(2) is atomic, so flag + payload land together.
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

/* Local atomic-write helper (kept private so this TU can be built/tested
 * standalone without dragging db.c in via a shared header). Mirrors the
 * one in db.c — tmp + write + fsync + rename. */
static int ast_atomic_write(const char *path, const char *text)
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

static json_t *slot_to_json(uint16_t idx, const AstSlot *s)
{
    json_t *o = json_object();
    if (!o) return NULL;
    if (json_object_set_new(o, "idx", json_integer(idx)) < 0
        || json_object_set_new(o, "issi", json_integer(s->issi)) < 0
        || json_object_set_new(o, "last_seen_multiframe",
                               json_integer(s->last_seen_multiframe)) < 0
        || json_object_set_new(o, "shadow_idx", json_integer(s->shadow_idx)) < 0
        || json_object_set_new(o, "state", json_integer(s->state)) < 0
        || json_object_set_new(o, "group_count",
                               json_integer(s->group_count)) < 0
        || json_object_set_new(o, "valid", json_boolean(s->valid)) < 0) {
        json_decref(o);
        return NULL;
    }
    json_t *gl = json_array();
    if (!gl || json_object_set_new(o, "group_list", gl) < 0) {
        json_decref(o);
        return NULL;
    }
    for (uint8_t g = 0; g < s->group_count && g < TETRA_AST_GROUP_LIST_MAX; g++) {
        if (json_array_append_new(gl, json_integer(s->group_list[g])) < 0) {
            json_decref(o);
            return NULL;
        }
    }
    return o;
}

int ast_snapshot(const Ast *ast, const char *path)
{
    if (!ast || !path) {
        return -EINVAL;
    }
    if (strlen(path) >= TETRA_DB_PATH_MAX) {
        return -EINVAL;
    }

    json_t *root = json_object();
    if (!root) return -ENOMEM;

    if (json_object_set_new(root, "version", json_integer(1)) < 0
        || json_object_set_new(root, "clean_shutdown_flag",
                               json_boolean(true)) < 0) {
        json_decref(root);
        return -ENOMEM;
    }

    json_t *slots = json_array();
    if (!slots || json_object_set_new(root, "slots", slots) < 0) {
        json_decref(root);
        return -ENOMEM;
    }

    for (uint16_t i = 0; i < TETRA_AST_SLOT_COUNT; i++) {
        const AstSlot *s = &ast->slots[i];
        if (!s->valid) {
            continue;
        }
        json_t *o = slot_to_json(i, s);
        if (!o || json_array_append_new(slots, o) < 0) {
            json_decref(root);
            return -ENOMEM;
        }
    }

    char *text = json_dumps(root, JSON_INDENT(2) | JSON_SORT_KEYS);
    json_decref(root);
    if (!text) return -ENOMEM;

    int rc = ast_atomic_write(path, text);
    free(text);
    return rc;
}

static int ast_load_slot(json_t *o, AstSlot *out, uint16_t *out_idx)
{
    if (!json_is_object(o)) return -EINVAL;

    json_t *jidx   = json_object_get(o, "idx");
    json_t *jissi  = json_object_get(o, "issi");
    json_t *jls    = json_object_get(o, "last_seen_multiframe");
    json_t *jshad  = json_object_get(o, "shadow_idx");
    json_t *jstate = json_object_get(o, "state");
    json_t *jgc    = json_object_get(o, "group_count");
    json_t *jgl    = json_object_get(o, "group_list");
    json_t *jvalid = json_object_get(o, "valid");

    if (!jidx || !jissi || !jls || !jshad || !jstate || !jgc || !jgl || !jvalid) {
        return -EINVAL;
    }
    if (!json_is_integer(jidx) || !json_is_integer(jissi)
        || !json_is_integer(jls) || !json_is_integer(jshad)
        || !json_is_integer(jstate) || !json_is_integer(jgc)
        || !json_is_array(jgl) || !json_is_boolean(jvalid)) {
        return -EINVAL;
    }

    json_int_t idx = json_integer_value(jidx);
    if (idx < 0 || idx >= (json_int_t) TETRA_AST_SLOT_COUNT) {
        return -EINVAL;
    }
    json_int_t gc = json_integer_value(jgc);
    if (gc < 0 || gc > (json_int_t) TETRA_AST_GROUP_LIST_MAX) {
        return -EINVAL;
    }
    if (json_array_size(jgl) != (size_t) gc) {
        return -EINVAL;
    }

    memset(out, 0, sizeof(*out));
    out->issi                 = (uint32_t) json_integer_value(jissi);
    out->last_seen_multiframe = (uint32_t) json_integer_value(jls);
    out->shadow_idx           = (uint8_t)  json_integer_value(jshad);
    out->state                = (uint8_t)  (json_integer_value(jstate) & 0x0F);
    out->group_count          = (uint8_t)  gc;
    out->valid                = json_is_true(jvalid);

    for (size_t g = 0; g < (size_t) gc; g++) {
        json_t *jg = json_array_get(jgl, g);
        if (!json_is_integer(jg)) {
            return -EINVAL;
        }
        json_int_t gv = json_integer_value(jg);
        if (gv < 0 || (uint64_t) gv > 0xFFFFFFu) {
            return -EINVAL;
        }
        out->group_list[g] = (uint32_t) gv;
    }

    *out_idx = (uint16_t) idx;
    return 0;
}

int ast_reload(Ast *ast, const char *path, bool *out_loaded)
{
    if (!ast || !path || !out_loaded) {
        return -EINVAL;
    }
    *out_loaded = false;
    memset(ast, 0, sizeof(*ast));

    struct stat st;
    if (stat(path, &st) != 0) {
        /* No prior snapshot — clean first start. */
        return 0;
    }

    json_error_t err;
    json_t *root = json_load_file(path, 0, &err);
    if (!root) {
        /* File present but unreadable / malformed — fail loud so operator
         * sees something is wrong. AST stays zero. */
        return -EIO;
    }

    if (!json_is_object(root)) {
        json_decref(root);
        return -EIO;
    }

    json_t *flag = json_object_get(root, "clean_shutdown_flag");
    if (!flag || !json_is_boolean(flag) || !json_is_true(flag)) {
        /* Crash recovery: a snapshot exists but the flag was not set
         * (or was explicitly false). Per Decision #10 we DO NOT reload —
         * the AST is left zero and the caller proceeds with a fresh
         * working set. Not an error. */
        json_decref(root);
        return 0;
    }

    json_t *jver = json_object_get(root, "version");
    if (!jver || !json_is_integer(jver) || json_integer_value(jver) != 1) {
        json_decref(root);
        return -EIO;
    }

    json_t *slots = json_object_get(root, "slots");
    if (!slots || !json_is_array(slots)) {
        json_decref(root);
        return -EIO;
    }

    size_t i;
    json_t *item;
    json_array_foreach(slots, i, item) {
        AstSlot tmp;
        uint16_t idx = 0;
        int rc = ast_load_slot(item, &tmp, &idx);
        if (rc != 0) {
            memset(ast, 0, sizeof(*ast));
            json_decref(root);
            return -EIO;
        }
        ast->slots[idx] = tmp;
    }

    json_decref(root);
    *out_loaded = true;
    return 0;
}
