/* sw/core/include/tetra/sap.h — SAP-level helpers and accessor macros.
 *
 * Owned by S0 (S0-sw-core-msgbus-types). Locked under IF_CORE_API_v1.
 *
 * Defines:
 *   - SAP_ID_LIST() X-macro for stringification + iteration
 *   - sap_id_name() reverse lookup
 *   - safe SapMsg accessor macros (no field access without bounds check)
 *   - SapMsg construction helpers (sapmsg_init / sapmsg_init_bytes)
 *
 * Pure header (no .c). Header-only by design so msgbus / downstream
 * layers do not pay an indirection cost on hot dispatch paths.
 */
#ifndef TETRA_CORE_SAP_H
#define TETRA_CORE_SAP_H

#include "tetra/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * SAP_ID_LIST — X-macro over the canonical SapId values.
 *
 * Used for sap_id_name() below and for tests that want to enumerate
 * every defined SapId without keeping two copies of the list in sync.
 * Order matches the SapId enum in types.h.
 * ------------------------------------------------------------------------- */
#define SAP_ID_LIST(X)        \
    X(SapId_None,    "None")  \
    X(SapId_TpSap,   "Tp")    \
    X(SapId_TmvSap,  "Tmv")   \
    X(SapId_TmaSap,  "Tma")   \
    X(SapId_TmdSap,  "Tmd")   \
    X(SapId_TleSap,  "Tle")   \
    X(SapId_TlmbSap, "Tlmb")  \
    X(SapId_TlmcSap, "Tlmc") \
    X(SapId_TnmmSap, "Tnmm")

/* sap_id_name — lookup short label for log/debug. Returns "?" on
 * out-of-range to avoid forcing callers to bounds-check first. */
static inline const char *sap_id_name(SapId id)
{
    switch (id) {
#define SAP_ID_NAME_CASE(enum_val, label) case enum_val: return label;
    SAP_ID_LIST(SAP_ID_NAME_CASE)
#undef SAP_ID_NAME_CASE
    default: return "?";
    }
}

/* sap_id_is_valid — guards msgbus_register / msgbus_post against
 * out-of-range enum values that downstream code might pass through
 * unchecked casts. Treats SapId_None as invalid for routing (it is the
 * "uninitialised" sentinel only). */
static inline bool sap_id_is_valid(SapId id)
{
    return ((int) id) > (int) SapId_None && ((int) id) < (int) SapId__Max;
}

/* ---------------------------------------------------------------------------
 * SapMsg accessor macros.
 *
 * The msgbus runtime stores SapMsg by value plus a copy of the payload;
 * these macros are used by handlers to pull fields out without forcing
 * struct field knowledge into every call site. They also paper over a
 * future change where SapMsg may grow opaque.
 * ------------------------------------------------------------------------- */
#define SAPMSG_SRC(m)      ((m)->src)
#define SAPMSG_DEST(m)     ((m)->dest)
#define SAPMSG_SAP(m)      ((m)->sap)
#define SAPMSG_LEN(m)      ((m)->len)
#define SAPMSG_PAYLOAD(m)  ((m)->payload)

/* SAPMSG_PAYLOAD_BYTE — safe per-byte read.
 *
 * Out-of-range index returns 0 rather than reading wild memory. Use this
 * in handlers when scanning a payload of unknown precise length. */
#define SAPMSG_PAYLOAD_BYTE(m, i)                                      \
    (((i) < (size_t) (m)->len && (m)->payload != NULL)                  \
        ? (m)->payload[(i)]                                              \
        : (uint8_t) 0)

/* SAPMSG_MATCH — true if message addresses (dest, sap) tuple. Used in
 * dispatch: every registered handler is keyed exactly by this tuple. */
#define SAPMSG_MATCH(m, want_dest, want_sap)                           \
    ((m)->dest == (want_dest) && (m)->sap == (want_sap))

/* ---------------------------------------------------------------------------
 * SapMsg construction helpers.
 * ------------------------------------------------------------------------- */

/* sapmsg_init — populate envelope without allocating payload storage.
 * `payload` is borrowed; caller must keep it alive until msgbus_post()
 * returns (msgbus_post copies into its queue). */
static inline void sapmsg_init(SapMsg *m,
                               SapId src, SapId dest, SapId sap,
                               const uint8_t *payload, uint16_t len)
{
    if (m == NULL) {
        return;
    }
    m->src     = src;
    m->dest    = dest;
    m->sap     = sap;
    m->len     = len;
    m->payload = payload;
}

/* sapmsg_init_zero — zero-length signalling-only message (no payload). */
static inline void sapmsg_init_zero(SapMsg *m,
                                    SapId src, SapId dest, SapId sap)
{
    sapmsg_init(m, src, dest, sap, NULL, 0);
}

/* ---------------------------------------------------------------------------
 * SAP_PRIO_DEFAULT — recommended default priority per SAP.
 *
 * Per ARCHITECTURE.md §"Message Bus":
 *   Immediate = TmaSap RX (AACH-window-bounded), TlmbSap (sync/sysinfo)
 *   Normal    = LLC/MLE/MM/CMCE day-to-day signalling
 *   Low       = TnmmSap (test, supervisory)
 *
 * Callers may override via msgbus_post(prio, ...). This is just a hint
 * for entities that don't care to pick.
 * ------------------------------------------------------------------------- */
static inline MsgPriority sap_prio_default(SapId sap)
{
    switch (sap) {
    case SapId_TmaSap:
    case SapId_TlmbSap:
        return MsgPrio_High;
    case SapId_TnmmSap:
        return MsgPrio_Low;
    default:
        return MsgPrio_Normal;
    }
}

#endif /* TETRA_CORE_SAP_H */
