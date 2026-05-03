/* sw/core/include/tetra/msgbus.h — Message-Bus public API.
 *
 * Owned by S0 (S0-sw-core-msgbus-types). Locked under IF_CORE_API_v1.
 *
 * Design (ARCHITECTURE.md §"Message Bus"):
 *   - 3 priority buckets (High / Normal / Low), FIFO within bucket.
 *   - Single dispatch loop in tetra_d (S7); thread-unsafe by design.
 *   - Callbacks registered per (dest, sap) tuple.
 *   - Bounded queue capacity per priority; on overflow the post is
 *     dropped and a counter increments (drops_per_prio[]).
 *   - Payloads are copied into queue-internal storage on post(); the
 *     handler sees an internal payload pointer, valid only during
 *     dispatch.
 *
 * Memory:
 *   The bus owns a single contiguous buffer for entries plus payload
 *   bytes; both are caller-supplied via MsgBusCfg so the bus does no
 *   malloc itself (keeps SW unit-tests deterministic and arm-cross-
 *   compile-safe). See msgbus_init().
 */
#ifndef TETRA_CORE_MSGBUS_H
#define TETRA_CORE_MSGBUS_H

#include "tetra/sap.h"
#include "tetra/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* msgbus_handler_fn — registered per (dest, sap) tuple.
 *
 * The msg pointer is queue-internal and valid only for the call;
 * payload pointer inside msg is also queue-internal. Handlers MUST NOT
 * retain either past return. ctx is the cookie passed to
 * msgbus_register() (entity instance pointer typically). */
typedef void (*msgbus_handler_fn)(const SapMsg *msg, void *ctx);

/* MSGBUS_REG_CAP — per-bus upper bound on (dest, sap, handler) entries.
 * Conservative compile-time pin; downstream is encouraged not to
 * register more than ~16 keys per layer. */
#define MSGBUS_REG_CAP 32

/* MsgBusCfg — caller-supplied capacity + storage.
 *
 * queue_cap_per_prio: capacity (in entries) of each of the 3 priority
 *                     buckets. Total entries == 3 * queue_cap_per_prio.
 *                     Must be >= 1.
 * max_payload_bytes:  max bytes copied per post(); larger payloads are
 *                     rejected with -E2BIG. Must be >= 1.
 * entry_storage:      caller-owned array of MsgBusEntry; size in bytes
 *                     == 3 * queue_cap_per_prio * sizeof(MsgBusEntry).
 *                     Bus does not free it.
 * payload_storage:    caller-owned bytes; size in bytes ==
 *                     3 * queue_cap_per_prio * max_payload_bytes.
 *                     Bus does not free it.
 *
 * The bus enforces the size relations internally; if storage is too
 * small msgbus_init() returns -EINVAL.
 */
typedef struct MsgBusEntry MsgBusEntry;
typedef struct {
    size_t        queue_cap_per_prio;
    size_t        max_payload_bytes;
    MsgBusEntry  *entry_storage;
    size_t        entry_storage_bytes;
    uint8_t      *payload_storage;
    size_t        payload_storage_bytes;
} MsgBusCfg;

/* MsgBusEntry — opaque to callers; size exposed so MsgBusCfg can size
 * its storage at compile-time. Treat fields as private. */
struct MsgBusEntry {
    SapId    src;
    SapId    dest;
    SapId    sap;
    uint16_t len;
    bool     in_use;
    uint8_t  _pad;
    /* payload is not embedded: it lives in payload_storage[] indexed by
     * the entry slot to keep entry size predictable. */
};

/* MsgBusReg — internal registration entry. Exposed only because MsgBus
 * embeds it; callers must not touch. */
typedef struct {
    SapId             dest;
    SapId             sap;
    msgbus_handler_fn cb;
    void             *ctx;
    bool              active;
} MsgBusReg;

/* MsgBusQueue — single priority bucket, ring-buffered. */
typedef struct {
    MsgBusEntry *entries;       /* pointer into MsgBusCfg.entry_storage   */
    uint8_t     *payloads;      /* pointer into MsgBusCfg.payload_storage */
    size_t       cap;           /* entries per bucket                     */
    size_t       max_payload;   /* per-entry payload cap (bytes)          */
    size_t       head;          /* next pop index                         */
    size_t       tail;          /* next push index                        */
    size_t       count;         /* entries currently held                 */
    size_t       drops;         /* total drops on post-overflow           */
} MsgBusQueue;

typedef struct {
    MsgBusQueue queues[MsgPrio__Count];
    MsgBusReg   regs[MSGBUS_REG_CAP];
    size_t      reg_count;
    bool        initialised;
} MsgBus;

/* ---------------------------------------------------------------------------
 * msgbus_init — set up bus from caller-supplied storage.
 *
 * Returns 0 on success, -EINVAL on bad cfg (NULL, zero capacity,
 * undersized storage). On error, bus is left zeroed and unusable.
 * ------------------------------------------------------------------------- */
int msgbus_init(MsgBus *bus, const MsgBusCfg *cfg);

/* ---------------------------------------------------------------------------
 * msgbus_register — install handler for (dest, sap) tuple.
 *
 * dest must be a valid SAP (not SapId_None). sap likewise. cb must be
 * non-NULL. ctx may be NULL. Returns 0 on success, -ENOSPC if the bus
 * already holds MSGBUS_REG_CAP registrations, -EINVAL on bad args.
 *
 * Multiple handlers MAY be registered for the same (dest, sap) tuple;
 * dispatch calls them in registration order. (Bluestation has 1:1 only,
 * but multi-subscribe is useful for taps/loggers and costs nothing.)
 * ------------------------------------------------------------------------- */
int msgbus_register(MsgBus            *bus,
                    SapId              dest,
                    SapId              sap,
                    msgbus_handler_fn  cb,
                    void              *ctx);

/* ---------------------------------------------------------------------------
 * msgbus_post — enqueue a copy of msg at given priority.
 *
 * Copies payload bytes into queue-internal storage. The caller's
 * SapMsg + payload may be freed/reused after this returns. Returns:
 *   0       on success
 *   -EINVAL on bad args (NULL bus, NULL msg, invalid prio, bad SapId)
 *   -E2BIG  if msg.len > cfg.max_payload_bytes
 *   -ENOSPC on bucket overflow (drops counter incremented before
 *           returning; same return code so the caller can distinguish
 *           full-bus from invalid-arg by checking inputs first)
 * ------------------------------------------------------------------------- */
int msgbus_post(MsgBus       *bus,
                MsgPriority   prio,
                const SapMsg *msg);

/* ---------------------------------------------------------------------------
 * msgbus_dispatch_one — pop highest-priority message and invoke matching
 * handlers. Non-blocking.
 *
 * Returns:
 *   0 if all queues empty (no work)
 *   1 if one message was dispatched (regardless of how many handlers
 *     fired or whether anyone matched — a message with no registered
 *     listener is silently dropped after counting)
 *  <0 on error (-EINVAL if bus is NULL or uninitialised)
 *
 * Priority order: High first; within same priority, FIFO. After
 * dispatch the entry is freed and the slot reused.
 * ------------------------------------------------------------------------- */
int msgbus_dispatch_one(MsgBus *bus);

/* ---------------------------------------------------------------------------
 * msgbus_drops — read drop counter for a priority bucket. Returns
 * SIZE_MAX on bad args (acts as "obviously broken" sentinel; valid
 * counter values are well below that).
 * ------------------------------------------------------------------------- */
size_t msgbus_drops(const MsgBus *bus, MsgPriority prio);

/* ---------------------------------------------------------------------------
 * msgbus_pending — total messages queued across all priorities. Useful
 * for tests + WebUI observers. Returns 0 if bus is NULL.
 * ------------------------------------------------------------------------- */
size_t msgbus_pending(const MsgBus *bus);

/* ---------------------------------------------------------------------------
 * BitBuffer API.
 * ------------------------------------------------------------------------- */

/* bb_init — wrap caller buffer as a BitBuffer with len_bits readable
 * window. autoexpand is OFF by default; call bb_set_autoexpand to
 * toggle. Returns the initialised BitBuffer by value (zero-cost). */
BitBuffer bb_init(uint8_t *buf, size_t len_bits);

/* bb_init_autoexpand — buffer with an initial capacity in bits but
 * `end == 0`; writes advance `end` up to `cap_bits`. Mirrors bluestation
 * BitBuffer::new_autoexpand. The `cap_bits` MUST equal 8 * (size of the
 * underlying byte array) — the implementation does NOT realloc. */
BitBuffer bb_init_autoexpand(uint8_t *buf, size_t cap_bits);

/* bb_put_bits — write the low n bits of v at the cursor. n in 1..32.
 * Asserts on overflow when autoexpand is OFF. */
void bb_put_bits(BitBuffer *bb, uint32_t v, uint8_t n);

/* bb_get_bits — read n bits starting at the cursor and advance.
 * n in 1..32. Returns 0 on underrun (caller should bb_remaining()
 * check first if it cares). */
uint32_t bb_get_bits(BitBuffer *bb, uint8_t n);

/* bb_pos_bits — current cursor position in bits, relative to window
 * start. */
size_t bb_pos_bits(const BitBuffer *bb);

/* bb_seek_bits — move cursor to absolute window-relative position.
 * Asserts if pos > end-start. */
void bb_seek_bits(BitBuffer *bb, size_t pos);

/* bb_remaining — number of bits left between cursor and window end. */
size_t bb_remaining(const BitBuffer *bb);

/* bb_len_bits — total length of window (end - start). */
size_t bb_len_bits(const BitBuffer *bb);

/* bb_set_autoexpand — flip the autoexpand flag at runtime. */
void bb_set_autoexpand(BitBuffer *bb, bool enable);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_CORE_MSGBUS_H */
