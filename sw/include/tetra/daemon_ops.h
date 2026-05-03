/* sw/include/tetra/daemon_ops.h — IF_DAEMON_OPS_v1 — daemon-side wire API.
 *
 * Owned by S7 (S7-sw-tetra-d). Locked under interface contract
 * IF_DAEMON_OPS_v1 per docs/MIGRATION_PLAN.md §"Agent Topology" §S7
 * + §A "Interface-locking schedule" #6.
 *
 * Defines the daemon-side of the JSON envelope wire format from
 * docs/OPERATIONS.md §6 + the dotted op-name catalogue from §1..§5 +
 * the async-job model from §9. Consumed by S6 CGI clients (read-only
 * — they only need the op-name strings + error-code names) and
 * implemented inside this same agent (`sw/socket_server.c`).
 *
 * Wire envelope (length-prefixed; 4 byte big-endian LEN, then UTF-8
 * JSON of LEN bytes):
 *
 *   request : {"op":"<entity>.<verb>", "args":{...},
 *              "req_id":"<uuid>", "client":"cgi:<scriptname>"}
 *   response: {"ok":true,  "req_id":"<echo>", "data":{...}}
 *           | {"ok":false, "req_id":"<echo>",
 *              "error":{"code":"EXXX","message":"...",
 *                       "field":"<path>", "detail":{...}}}
 *
 * Self-contained: pulls in <jansson.h> for the json_t type and
 * <stdint.h>/<stddef.h>/<stdbool.h>. Daemon-side only — CGIs never
 * include this header (they hit the socket from S6 cgi_common).
 */
#ifndef TETRA_DAEMON_OPS_H
#define TETRA_DAEMON_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <jansson.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Frame size limits — chosen to fit any op envelope comfortably while
 * bounding per-client memory. CGIs are local-only; large payloads are
 * the bulk-DB import (db.import) and capture downloads. The latter is
 * NOT routed through the JSON path (file streamed by CGI directly), so
 * 1 MiB is a loose upper bound that still leaves DoS-headroom on the
 * 512 MiB Zynq RAM.
 * ------------------------------------------------------------------------- */
#define DAEMON_OPS_MAX_FRAME_BYTES   (1u * 1024u * 1024u)
#define DAEMON_OPS_LEN_PREFIX_BYTES  4u
#define DAEMON_OPS_SOCKET_PATH       "/var/run/tetra_d.sock"

/* ---------------------------------------------------------------------------
 * DAEMON_OP_LIST — X-macro over every op-name from OPERATIONS.md §1..§5.
 *
 * Order matches the OPERATIONS.md walk: §1 status, §2 db/profile/entity
 * /session/policy, §3 debug, §4 config/apply, §5 tools/jobs/daemon.
 * Adding a new op = append one X(...) line. Removing one breaks the
 * IF_DAEMON_OPS_v1 lock — bump the contract instead.
 *
 * The second column is the C-identifier-friendly enum suffix
 * (DaemonOp_<suffix>); the first column is the wire op-name verbatim
 * (string matched against envelope.op).
 * ------------------------------------------------------------------------- */
#define DAEMON_OP_LIST(X)                                              \
    /* §1 Live Status */                                               \
    X("status.summary",            status_summary)                     \
    X("status.layers",             status_layers)                      \
    X("status.phy_stats",          status_phy_stats)                   \
    X("status.dma_stats",          status_dma_stats)                   \
    X("status.txq",                status_txq)                         \
    X("status.msgbus",             status_msgbus)                      \
    X("status.calls",              status_calls)                       \
    X("status.groups",             status_groups)                      \
    X("status.fpga_regs",          status_fpga_regs)                   \
    X("status.aach_current",       status_aach_current)                \
    /* §2 Subscriber DB — profiles */                                  \
    X("profile.list",              profile_list)                       \
    X("profile.get",               profile_get)                        \
    X("profile.put",               profile_put)                        \
    X("profile.delete",            profile_delete)                     \
    X("profile.reset",             profile_reset)                      \
    /* §2 entities */                                                  \
    X("entity.list",               entity_list)                        \
    X("entity.get",                entity_get)                         \
    X("entity.lookup",             entity_lookup)                      \
    X("entity.put",                entity_put)                         \
    X("entity.delete",             entity_delete)                      \
    X("entity.clear_all",          entity_clear_all)                   \
    /* §2 sessions (AST) */                                            \
    X("session.list",              session_list)                       \
    X("session.get",               session_get)                        \
    X("session.stats",             session_stats)                      \
    /* §2 bulk DB + policy */                                          \
    X("db.export",                 db_export)                          \
    X("db.import",                 db_import)                          \
    X("db.reset",                  db_reset)                           \
    X("policy.get",                policy_get)                         \
    X("policy.put",                policy_put)                         \
    /* §3 Debug */                                                     \
    X("debug.pdu_recent",          debug_pdu_recent)                   \
    X("debug.pdu_stream",          debug_pdu_stream)                   \
    X("debug.aach_recent",         debug_aach_recent)                  \
    X("debug.aach_stream",         debug_aach_stream)                  \
    X("debug.slot_schedule",       debug_slot_schedule)                \
    X("debug.irq_counters",        debug_irq_counters)                 \
    X("debug.msgbus_stream",       debug_msgbus_stream)                \
    X("debug.log_tail",            debug_log_tail)                     \
    X("debug.reg_read",            debug_reg_read)                     \
    X("debug.reg_write",           debug_reg_write)                    \
    X("debug.tmasap_recent",       debug_tmasap_recent)                \
    X("debug.tmdsap_stats",        debug_tmdsap_stats)                 \
    /* §4 Configuration */                                             \
    X("config.cell.get",           config_cell_get)                    \
    X("config.cell.put",           config_cell_put)                    \
    X("config.rf.get",             config_rf_get)                      \
    X("config.rf.put",             config_rf_put)                      \
    X("config.cipher.get",         config_cipher_get)                  \
    X("config.cipher.put",         config_cipher_put)                  \
    X("config.training.get",       config_training_get)                \
    X("config.training.put",       config_training_put)                \
    X("config.slot_table.get",     config_slot_table_get)              \
    X("config.slot_table.put",     config_slot_table_put)              \
    X("config.ad9361.get",         config_ad9361_get)                  \
    X("config.ad9361.put",         config_ad9361_put)                  \
    X("config.msgbus.get",         config_msgbus_get)                  \
    X("config.msgbus.put",         config_msgbus_put)                  \
    X("config.schema",             config_schema)                      \
    X("apply.status",              apply_status)                       \
    X("apply.apply",               apply_apply)                        \
    X("apply.discard",             apply_discard)                      \
    /* §5 Tools */                                                     \
    X("tools.pdu_send_form",       tools_pdu_send_form)                \
    X("tools.pdu_send",            tools_pdu_send)                     \
    X("tools.reset_counters",      tools_reset_counters)               \
    X("tools.reset_ast",           tools_reset_ast)                    \
    X("tools.reg_write",           tools_reg_write)                    \
    X("tools.bitstream_list",      tools_bitstream_list)               \
    X("tools.bitstream_switch",    tools_bitstream_switch)             \
    X("tools.capture_start",       tools_capture_start)                \
    X("tools.capture_stop",        tools_capture_stop)                 \
    X("tools.capture_list",        tools_capture_list)                 \
    X("tools.capture_download",    tools_capture_download)             \
    X("tools.decoder_upload",      tools_decoder_upload)               \
    X("tools.decoder_list",        tools_decoder_list)                 \
    X("tools.decoder_run",         tools_decoder_run)                  \
    X("tools.decoder_result",      tools_decoder_result)               \
    /* §5 jobs */                                                      \
    X("jobs.list",                 jobs_list)                          \
    X("jobs.get",                  jobs_get)                           \
    X("jobs.cancel",               jobs_cancel)                        \
    /* §5 daemon lifecycle */                                          \
    X("daemon.stop",               daemon_stop)                        \
    X("daemon.restart",            daemon_restart)

/* DaemonOpId — enum id per op. Generated from DAEMON_OP_LIST. */
typedef enum {
#define DAEMON_OP_ENUM(name, suffix) DaemonOp_##suffix,
    DAEMON_OP_LIST(DAEMON_OP_ENUM)
#undef DAEMON_OP_ENUM
    DaemonOp__Count,
    DaemonOp_Unknown = -1
} DaemonOpId;

/* ---------------------------------------------------------------------------
 * Error code catalogue — mirrors OPERATIONS.md §6 verbatim. The strings
 * are emitted on the wire under {"error":{"code":...}}; daemon code
 * uses the symbolic enum to keep call sites typo-free.
 * ------------------------------------------------------------------------- */
#define DAEMON_ERR_LIST(X)                                             \
    X(DaemonErr_None,        "OK")                                     \
    X(DaemonErr_Einval,      "EINVAL")                                 \
    X(DaemonErr_Enoent,      "ENOENT")                                 \
    X(DaemonErr_Eexist,      "EEXIST")                                 \
    X(DaemonErr_Erofs,       "EROFS")                                  \
    X(DaemonErr_Eperm,       "EPERM")                                  \
    X(DaemonErr_Ebusy,       "EBUSY")                                  \
    X(DaemonErr_Enospc,      "ENOSPC")                                 \
    X(DaemonErr_Erange,      "ERANGE")                                 \
    X(DaemonErr_Eref,        "EREF")                                   \
    X(DaemonErr_Esched,      "ESCHED")                                 \
    X(DaemonErr_Efpga,       "EFPGA")                                  \
    X(DaemonErr_Eio,         "EIO")                                    \
    X(DaemonErr_E2big,       "E2BIG")                                  \
    X(DaemonErr_Einternal,   "EINTERNAL")

typedef enum {
#define DAEMON_ERR_ENUM(id, str) id,
    DAEMON_ERR_LIST(DAEMON_ERR_ENUM)
#undef DAEMON_ERR_ENUM
    DaemonErr__Count
} DaemonErrCode;

/* daemon_op_name — wire string for an op id, NULL if out-of-range. */
const char *daemon_op_name(DaemonOpId op);

/* daemon_op_lookup — reverse lookup; DaemonOp_Unknown if unknown. */
DaemonOpId daemon_op_lookup(const char *name);

/* daemon_err_name — wire string for an error code. */
const char *daemon_err_name(DaemonErrCode err);

/* ---------------------------------------------------------------------------
 * daemon_op_fn — handler signature.
 *
 * `args`       : the "args" sub-object from the request envelope. May be
 *                NULL (caller is supposed to default-treat) but most ops
 *                will -EINVAL on NULL.
 * `result`     : on success the handler stores a json_t * here that the
 *                envelope layer wraps into {"ok":true,"data":...}. The
 *                caller takes ownership (json_decref) on the success
 *                path. On error, *result is left NULL and the handler
 *                returns a DaemonErrCode * 0..DaemonErr__Count cast as
 *                a negative int (i.e. return -DaemonErr_Einval).
 * `state`      : opaque daemon-state pointer; handlers re-cast to their
 *                expected struct (single-thread, single-instance daemon).
 *
 * Returns 0 on success, or -<DaemonErrCode> on a structured error.
 * Internal-error catch-all: -DaemonErr_Einternal.
 * ------------------------------------------------------------------------- */
typedef struct DaemonState DaemonState;

typedef int (*daemon_op_fn)(DaemonState   *state,
                            const json_t  *args,
                            json_t       **result);

/* ---------------------------------------------------------------------------
 * DaemonOpEntry — one row in the dispatch table.
 *
 * `min_args_present` : optional hint. If non-NULL, envelope-layer rejects
 *                      requests whose args object is missing any of the
 *                      named keys with -EINVAL before calling the
 *                      handler. NULL means handler does its own
 *                      validation. Use only for ops with single-shape
 *                      mandatory args; leave NULL for everything where
 *                      the args are optional or polymorphic.
 * ------------------------------------------------------------------------- */
typedef struct {
    DaemonOpId          id;
    const char         *name;
    daemon_op_fn        fn;
    const char *const  *min_args_present;
    bool                is_streaming;
    bool                is_async_job;
} DaemonOpEntry;

/* ---------------------------------------------------------------------------
 * Envelope helpers (defined in socket_server.c — public so tests can
 * round-trip without the socket layer).
 * ------------------------------------------------------------------------- */

/* Build a success-envelope json_t. Steals `data` (json_decref called on
 * `data` after embedding). Returns NULL on alloc failure. `req_id` may
 * be NULL → omitted. */
json_t *daemon_envelope_ok(const char *req_id, json_t *data);

/* Build an error-envelope json_t. Mirrors §6 error shape. `field` and
 * `detail` may be NULL. `detail` is stolen on success. */
json_t *daemon_envelope_err(const char    *req_id,
                            DaemonErrCode  code,
                            const char    *message,
                            const char    *field,
                            json_t        *detail);

/* Parse an inbound envelope: extract `op`, `args`, `req_id`, `client`.
 * On success returns 0 + sets all out-pointers (req_id/client may be
 * NULL pointers if the envelope omits them). On schema failure returns
 * -DaemonErr_Einval. The returned json_t* outputs (op_args / req_id /
 * client) borrow from `root`; caller must keep `root` alive. */
int daemon_envelope_parse(const json_t  *root,
                          const char   **out_op_name,
                          const json_t **out_args,
                          const char   **out_req_id,
                          const char   **out_client);

/* Serialise envelope into a length-prefixed wire frame. Allocates a
 * buffer with malloc(); caller frees. Returns 0 on success, -errno on
 * allocation/serialisation failure. *out_buf and *out_len are written
 * only on success. */
int daemon_envelope_marshal(const json_t *env,
                            uint8_t     **out_buf,
                            size_t       *out_len);

/* Parse a length-prefixed wire frame in `buf` of `len` bytes. Returns
 * 0 + writes a new json_t* (caller json_decrefs) on success, or
 * -DaemonErr_Einval on bad LEN / malformed JSON, -DaemonErr_E2big on
 * LEN > DAEMON_OPS_MAX_FRAME_BYTES. */
int daemon_envelope_unmarshal(const uint8_t  *buf,
                              size_t          len,
                              json_t        **out_root);

/* ---------------------------------------------------------------------------
 * Op-table accessor — returns the (static, locked) DaemonOpEntry array.
 *
 * Length is *out_count entries. Indexed by DaemonOpId. The same entry
 * is also reachable via the `name` string through daemon_op_table_find.
 * ------------------------------------------------------------------------- */
const DaemonOpEntry *daemon_op_table(size_t *out_count);

/* daemon_op_table_find — O(N) name lookup (N≈75; sub-µs on Zynq). */
const DaemonOpEntry *daemon_op_table_find(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_DAEMON_OPS_H */
