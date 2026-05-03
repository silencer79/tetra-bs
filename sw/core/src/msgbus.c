/* sw/core/src/msgbus.c — Message-Bus implementation.
 *
 * Owned by S0 (S0-sw-core-msgbus-types). Locked under IF_CORE_API_v1.
 *
 * Single-threaded, three priority buckets, ring-buffered. No malloc:
 * caller supplies entry + payload storage via MsgBusCfg. Per-bucket
 * cap and per-message max-payload are pinned at init.
 */
#include "tetra/msgbus.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal helpers.
 * ------------------------------------------------------------------------- */

static bool prio_is_valid(MsgPriority p)
{
    return ((int) p) >= 0 && ((int) p) < (int) MsgPrio__Count;
}

static uint8_t *queue_payload_slot(const MsgBusQueue *q, size_t idx)
{
    return q->payloads + (idx * q->max_payload);
}

/* Walk all buckets in priority order, return the first non-empty one.
 * Returns NULL if all are empty. */
static MsgBusQueue *next_nonempty_queue(MsgBus *bus)
{
    for (int p = 0; p < (int) MsgPrio__Count; ++p) {
        if (bus->queues[p].count > 0) {
            return &bus->queues[p];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Public API.
 * ------------------------------------------------------------------------- */

int msgbus_init(MsgBus *bus, const MsgBusCfg *cfg)
{
    if (bus == NULL || cfg == NULL) {
        return -EINVAL;
    }
    if (cfg->queue_cap_per_prio == 0 || cfg->max_payload_bytes == 0) {
        return -EINVAL;
    }
    if (cfg->entry_storage == NULL || cfg->payload_storage == NULL) {
        return -EINVAL;
    }

    const size_t total_entries  = cfg->queue_cap_per_prio * (size_t) MsgPrio__Count;
    const size_t need_entry_b   = total_entries * sizeof(MsgBusEntry);
    const size_t need_payload_b = total_entries * cfg->max_payload_bytes;

    if (cfg->entry_storage_bytes   < need_entry_b ||
        cfg->payload_storage_bytes < need_payload_b) {
        return -EINVAL;
    }

    memset(bus, 0, sizeof(*bus));
    memset(cfg->entry_storage,   0, need_entry_b);
    memset(cfg->payload_storage, 0, need_payload_b);

    for (int p = 0; p < (int) MsgPrio__Count; ++p) {
        MsgBusQueue *q = &bus->queues[p];
        q->entries     = cfg->entry_storage   + ((size_t) p * cfg->queue_cap_per_prio);
        q->payloads    = cfg->payload_storage +
                         ((size_t) p * cfg->queue_cap_per_prio * cfg->max_payload_bytes);
        q->cap         = cfg->queue_cap_per_prio;
        q->max_payload = cfg->max_payload_bytes;
        q->head        = 0;
        q->tail        = 0;
        q->count       = 0;
        q->drops       = 0;
    }

    bus->reg_count   = 0;
    bus->initialised = true;
    return 0;
}

int msgbus_register(MsgBus            *bus,
                    SapId              dest,
                    SapId              sap,
                    msgbus_handler_fn  cb,
                    void              *ctx)
{
    if (bus == NULL || !bus->initialised || cb == NULL) {
        return -EINVAL;
    }
    if (!sap_id_is_valid(dest) || !sap_id_is_valid(sap)) {
        return -EINVAL;
    }

    if (bus->reg_count >= MSGBUS_REG_CAP) {
        return -ENOSPC;
    }

    MsgBusReg *r = &bus->regs[bus->reg_count];
    r->dest   = dest;
    r->sap    = sap;
    r->cb     = cb;
    r->ctx    = ctx;
    r->active = true;
    bus->reg_count += 1;
    return 0;
}

int msgbus_post(MsgBus       *bus,
                MsgPriority   prio,
                const SapMsg *msg)
{
    if (bus == NULL || !bus->initialised || msg == NULL) {
        return -EINVAL;
    }
    if (!prio_is_valid(prio)) {
        return -EINVAL;
    }
    /* dest + sap must be sane; src is allowed to be SapId_None for
     * messages sourced from a non-SAP origin (e.g. timer ticks). */
    if (!sap_id_is_valid(msg->dest) || !sap_id_is_valid(msg->sap)) {
        return -EINVAL;
    }

    MsgBusQueue *q = &bus->queues[prio];

    if ((size_t) msg->len > q->max_payload) {
        return -E2BIG;
    }

    if (q->count == q->cap) {
        q->drops += 1;
        return -ENOSPC;
    }

    MsgBusEntry *e = &q->entries[q->tail];
    e->src    = msg->src;
    e->dest   = msg->dest;
    e->sap    = msg->sap;
    e->len    = msg->len;
    e->in_use = true;

    if (msg->len > 0 && msg->payload != NULL) {
        memcpy(queue_payload_slot(q, q->tail), msg->payload, msg->len);
    }

    q->tail   = (q->tail + 1) % q->cap;
    q->count += 1;
    return 0;
}

int msgbus_dispatch_one(MsgBus *bus)
{
    if (bus == NULL || !bus->initialised) {
        return -EINVAL;
    }

    MsgBusQueue *q = next_nonempty_queue(bus);
    if (q == NULL) {
        return 0;
    }

    MsgBusEntry *e   = &q->entries[q->head];
    SapMsg       msg = {
        .src     = e->src,
        .dest    = e->dest,
        .sap     = e->sap,
        .len     = e->len,
        .payload = (e->len > 0) ? queue_payload_slot(q, q->head) : NULL,
    };

    /* Fan-out to every registered (dest, sap) match. Multi-subscribe is
     * supported intentionally — see msgbus.h note. */
    for (size_t i = 0; i < bus->reg_count; ++i) {
        const MsgBusReg *r = &bus->regs[i];
        if (!r->active) {
            continue;
        }
        if (r->dest == msg.dest && r->sap == msg.sap) {
            r->cb(&msg, r->ctx);
        }
    }

    /* Always advance head, even if no listener was registered. The
     * caller still gets "1 dispatched" so it can drive the loop until
     * empty. */
    e->in_use = false;
    q->head   = (q->head + 1) % q->cap;
    q->count -= 1;
    return 1;
}

size_t msgbus_drops(const MsgBus *bus, MsgPriority prio)
{
    if (bus == NULL || !bus->initialised || !prio_is_valid(prio)) {
        return SIZE_MAX;
    }
    return bus->queues[prio].drops;
}

size_t msgbus_pending(const MsgBus *bus)
{
    if (bus == NULL || !bus->initialised) {
        return 0;
    }
    size_t total = 0;
    for (int p = 0; p < (int) MsgPrio__Count; ++p) {
        total += bus->queues[p].count;
    }
    return total;
}
