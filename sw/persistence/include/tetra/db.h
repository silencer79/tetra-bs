/* sw/persistence/include/tetra/db.h — Subscriber-DB + AST persistence API.
 *
 * Owned by S5 (S5-sw-persistence). Locked under interface contracts
 * IF_DB_API_v1 + IF_AST_PERSIST_v1 per docs/MIGRATION_PLAN.md §S5.
 *
 * Source-of-truth hierarchy (CLAUDE.md): Gold > Bluestation > ETSI.
 *  - Record layouts (Profile 32-bit, Entity 64-bit, AST 256-bit) come from
 *    docs/references/reference_subscriber_db_arch.md.
 *  - File location + Profile-0 invariant 0x0000_088F come from
 *    docs/ARCHITECTURE.md §"Subscriber-DB".
 *  - Persistence pattern (atomic-rename + clean_shutdown_flag) is
 *    docs/MIGRATION_PLAN.md Decision #8 and #10.
 *
 * Self-contained: pulls in stdbool.h / stddef.h / stdint.h. Consumers also
 * need tetra/types.h for SsiType used inside Entity.
 */
#ifndef TETRA_DB_H
#define TETRA_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tetra/types.h"

/* ---------------------------------------------------------------------------
 * Capacity constants (reference_subscriber_db_arch.md).
 * ------------------------------------------------------------------------- */
#define TETRA_DB_PROFILE_COUNT    6u
#define TETRA_DB_ENTITY_COUNT     256u
#define TETRA_AST_SLOT_COUNT      64u
#define TETRA_AST_GROUP_LIST_MAX  8u

/* Profile-0 read-only invariant per ARCHITECTURE.md §"Subscriber-DB":
 * bit-exact 0x0000_088F (M2 GILA-Guard). Daemon corrects on load + refuses
 * write to id 0 with -EPERM.
 */
#define TETRA_DB_PROFILE0_BITS    0x0000088Fu

/* ---------------------------------------------------------------------------
 * Profile (32 bit on the wire / in BRAM).
 *
 * Layout per reference_subscriber_db_arch.md:
 *   [31:24] max_call_duration  (sec, 0=unlimited)
 *   [23:16] hangtime           (×100ms, max 25.5s)
 *   [15:12] priority           (4 bits)
 *   [11: 4] reserved           (8 bits — preserved bit-exact for round-trip)
 *   [ 3]    permit_voice
 *   [ 2]    permit_data
 *   [ 1]    permit_reg
 *   [ 0]    valid
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t max_call_duration;
    uint8_t hangtime;
    uint8_t priority;       /* 0..15 */
    uint8_t reserved;       /* preserves [11:4] bit-exact */
    bool    permit_voice;
    bool    permit_data;
    bool    permit_reg;
    bool    valid;
} Profile;

/* ---------------------------------------------------------------------------
 * Entity (64 bit on the wire / in BRAM).
 *
 * Layout per reference_subscriber_db_arch.md:
 *   [63:40] entity_id          (24-bit ISSI or GSSI)
 *   [39]    entity_type        (0=ISSI, 1=GSSI)
 *   [38:35] profile_id         (4 bits, indexes into Profile table)
 *   [34: 1] reserved           (34 bits — preserved bit-exact)
 *   [ 0]    valid
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t entity_id;     /* low 24 bits used */
    uint8_t  entity_type;   /* 0=ISSI, 1=GSSI */
    uint8_t  profile_id;    /* 0..(TETRA_DB_PROFILE_COUNT-1) */
    uint64_t reserved34;    /* low 34 bits used; preserves [34:1] bit-exact */
    bool     valid;
} Entity;

/* ---------------------------------------------------------------------------
 * AST slot (256 bit, 64 slots).
 *
 * Layout per reference_subscriber_db_arch.md:
 *   [255:232] ISSI                 (24 bit)
 *   [231:208] last_seen_multiframe (24 bit, rollover ~197 days)
 *   [207:200] shadow_idx           (8 bit, backref into Entity table)
 *   [199:196] state                (4 bit)
 *   [195:192] group_count          (4 bit, 0..8)
 *   [191:  0] group_list[8]        (8 × 24 bit GSSI)
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t issi;                                  /* low 24 bits */
    uint32_t last_seen_multiframe;                  /* 24 bits */
    uint8_t  shadow_idx;
    uint8_t  state;                                 /* 4 bits */
    uint8_t  group_count;                           /* 0..TETRA_AST_GROUP_LIST_MAX */
    uint32_t group_list[TETRA_AST_GROUP_LIST_MAX];  /* low 24 bits each */
    bool     valid;                                 /* slot occupied */
} AstSlot;

typedef struct {
    AstSlot slots[TETRA_AST_SLOT_COUNT];
} Ast;

/* ---------------------------------------------------------------------------
 * SubscriberDb — opaque-ish handle. Fields are exposed so callers can
 * stack-allocate, but direct mutation outside the API is undefined.
 *
 * `path` stores the path passed to db_open() so db_atomic_save() writes back
 * to the same file. NUL-terminated, capped at TETRA_DB_PATH_MAX-1.
 * ------------------------------------------------------------------------- */
#define TETRA_DB_PATH_MAX  256u

typedef struct {
    Profile  profiles[TETRA_DB_PROFILE_COUNT];
    Entity   entities[TETRA_DB_ENTITY_COUNT];
    char     path[TETRA_DB_PATH_MAX];
    bool     opened;
} SubscriberDb;

/* ---------------------------------------------------------------------------
 * IF_DB_API_v1
 *
 * Return convention: 0 on success, negative errno-style on failure.
 *  -ENOENT : record missing / lookup miss
 *  -EINVAL : argument out of range / schema violation
 *  -EPERM  : write attempted on Profile 0 (read-only invariant)
 *  -EIO    : underlying I/O / JSON error
 *  -ENOMEM : allocation failure
 *
 * db_open() loads the file at `path` if it exists. If the file does not
 * exist, the DB is initialised in-memory (Profile 0 set to invariant, all
 * other slots invalid) and `path` is recorded for later save. The file is
 * NOT created until db_atomic_save() runs.
 *
 * On successful load, Profile 0 is unconditionally re-asserted to the
 * invariant — daemon refuses to fail-start on a corrupted file (matches
 * ARCHITECTURE.md "Daemon ... corrects on load if missing").
 *
 * db_atomic_save() writes to `<path>.tmp` then rename(2)'s onto `path`.
 * Crash mid-write leaves `<path>.tmp` partial; the next db_open() detects
 * and unlinks it (the original `path` is intact because rename is atomic).
 *
 * db_lookup_entity() scans the entity table linearly for valid slots whose
 * entity_id matches; returns -ENOENT if not found.
 * ------------------------------------------------------------------------- */
int db_open(SubscriberDb *db, const char *path);
int db_get_profile(SubscriberDb *db, uint8_t id, Profile *out);
int db_put_profile(SubscriberDb *db, uint8_t id, const Profile *p);
int db_get_entity(SubscriberDb *db, uint16_t idx, Entity *out);
int db_put_entity(SubscriberDb *db, uint16_t idx, const Entity *e);
int db_lookup_entity(SubscriberDb *db, uint32_t entity_id, uint16_t *out_idx);
int db_atomic_save(SubscriberDb *db);

/* ---------------------------------------------------------------------------
 * Profile pack/unpack helpers — bit-exact to the 32-bit layout above.
 * Public so encoders/decoders can round-trip without going through JSON.
 * ------------------------------------------------------------------------- */
uint32_t profile_pack(const Profile *p);
void     profile_unpack(uint32_t bits, Profile *out);

/* ---------------------------------------------------------------------------
 * IF_AST_PERSIST_v1
 *
 * ast_snapshot() writes the AST + a clean_shutdown_flag=true marker to
 * `path` atomically (tmp + rename). Called from the daemon's SIGTERM
 * handler (Decision #10).
 *
 * ast_reload() loads the file and ONLY populates `*ast` if the file
 * contains clean_shutdown_flag=true. On any of:
 *   - file does not exist
 *   - file is malformed
 *   - clean_shutdown_flag is missing or false
 * the AST is left zeroed and *out_loaded is set to false (the function
 * itself returns 0 — "no reload" is not an error).
 *
 * On hard failure (e.g. JSON parse error from a present-and-flagged file)
 * ast_reload() returns -EIO and leaves *out_loaded = false.
 * ------------------------------------------------------------------------- */
int ast_snapshot(const Ast *ast, const char *path);
int ast_reload(Ast *ast, const char *path, bool *out_loaded);

#endif /* TETRA_DB_H */
