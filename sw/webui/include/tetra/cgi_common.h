/* sw/webui/include/tetra/cgi_common.h — shared CGI thin-client API.
 *
 * Owned by S6 (S6-sw-webui-cgis). Locked under interface contract
 * IF_WEBUI_CGI_v1 per docs/MIGRATION_PLAN.md §S6 + docs/OPERATIONS.md §6.
 *
 * Each <noun>.cgi binary in build/cgi-bin/ is a thin socket client. It
 * compiles down to a single main() that calls cgi_run() with a
 * binary-specific allow-list of dotted op-names. cgi_common.c does:
 *
 *   1. Read CGI env vars (REQUEST_METHOD, CONTENT_LENGTH, QUERY_STRING,
 *      REMOTE_ADDR, SCRIPT_NAME).
 *   2. Slurp stdin body up to CONTENT_LENGTH (POSTs only).
 *   3. Resolve op-name: priority is body JSON {"op":"..."} (POST) > query
 *      "op=..." (GET). For status.cgi the legacy "scope=..." query
 *      becomes args.scope and op is fixed by binary mapping.
 *   4. Allow-list check — ops outside the binary's compile-time list get
 *      a 403 + FORBIDDEN_OP envelope WITHOUT touching the daemon socket.
 *   5. connect(/run/tetra_d.sock), write 4-byte BE length + JSON request
 *      envelope (per OPERATIONS.md §6), read 4-byte BE length + JSON
 *      response, forward the response body to stdout with the right
 *      Content-Type header.
 *   6. SIGPIPE is ignored process-wide; all socket I/O caps total wall
 *      time at CGI_DEADLINE_MS so we stay under BusyBox httpd's 3-s
 *      exec timeout.
 *
 * The header pulls in only stdint/stddef/stdbool — implementation hides
 * jansson and the socket layer.
 */
#ifndef TETRA_CGI_COMMON_H
#define TETRA_CGI_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>  /* ssize_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Default daemon socket path. Override via $TETRA_D_SOCK for tests. */
#define CGI_DAEMON_SOCK_DEFAULT "/run/tetra_d.sock"

/* Total wall-time budget for the CGI run (env→socket→stdout). BusyBox
 * httpd 1.30.1 kills the CGI at ~3 s; we cap below that. */
#define CGI_DEADLINE_MS 2500

/* Maximum stdin body we accept. Larger → 413. Decoder upload uses
 * multipart and is handled by tools.cgi which raises this via
 * cgi_run_opts.max_body_bytes. */
#define CGI_DEFAULT_MAX_BODY (256u * 1024u)

/* Max single-side JSON envelope on the wire. Anything larger from the
 * daemon → 502. Mirrors tetra_d's own ENV_MAX. */
#define CGI_ENVELOPE_MAX (1u * 1024u * 1024u)

/* Per-binary configuration passed into cgi_run(). All fields except
 * `script_name` and `allowed_ops` may be left as zero/NULL to use the
 * defaults documented above. */
typedef struct CgiRunOpts {
    const char         *script_name;     /* e.g. "status.cgi" — for client tag */
    const char *const  *allowed_ops;     /* NULL-terminated allow-list */
    size_t              allowed_ops_n;   /* convenience; 0 → strlen-style scan */
    const char         *sock_path;       /* NULL → CGI_DAEMON_SOCK_DEFAULT */
    size_t              max_body_bytes;  /* 0 → CGI_DEFAULT_MAX_BODY */
    int                 deadline_ms;     /* 0 → CGI_DEADLINE_MS */

    /* Optional fixed op for binaries whose URL doesn't carry op=
     * (e.g. db.cgi?op=export but also db.cgi POST {op:...}). When the
     * caller passes a non-NULL fallback_op AND query/body don't supply
     * one, this is used. */
    const char         *fallback_op;
} CgiRunOpts;

/* The single entry point. Returns process exit code (0 = clean run,
 * non-zero only for catastrophic env failures — every protocol-level
 * error still returns 0 with an error envelope on stdout, per
 * OPERATIONS.md §6). */
int cgi_run(const CgiRunOpts *opts);

/* ---- Public helpers, used by tests and by special CGIs ---- */

/* Return true iff op is in opts->allowed_ops. NULL op → false. */
bool cgi_op_allowed(const CgiRunOpts *opts, const char *op);

/* Encode a 4-byte big-endian length prefix into out[0..3]. */
void cgi_pack_len_be(uint8_t out[4], uint32_t len);

/* Decode a 4-byte big-endian length prefix. */
uint32_t cgi_unpack_len_be(const uint8_t in[4]);

/* Read up to max bytes from fd into buf, blocking until min bytes have
 * arrived OR the absolute deadline_ms_epoch (CLOCK_MONOTONIC) passes.
 * Returns total bytes read on success, -1 on error/timeout (errno set:
 * ETIMEDOUT, EIO). EINTR is auto-resumed. */
ssize_t cgi_read_atleast(int fd,
                         void *buf,
                         size_t min_bytes,
                         size_t max_bytes,
                         long deadline_ms_epoch);

/* Write all `len` bytes to fd, retrying short writes. Returns 0 on
 * success, -1 on error (errno set). EPIPE is propagated. */
int cgi_write_all(int fd, const void *buf, size_t len);

/* Build a request envelope JSON string into out_buf (NUL-terminated).
 * Returns the number of bytes written (excluding NUL), or -1 on
 * overflow. `args_json` is embedded verbatim — caller must pass either
 * a valid JSON object literal ("{}", "{\"k\":1}") or NULL (treated as
 * "{}"). req_id may be NULL to skip the field. */
int cgi_build_request(char *out_buf,
                      size_t out_cap,
                      const char *op,
                      const char *args_json,
                      const char *req_id,
                      const char *client_tag);

/* Format-and-write an error envelope (ok:false, code, message) to fd
 * with HTTP status 200 + JSON Content-Type. Used by the FORBIDDEN_OP
 * gate and by daemon-connect failures. status_http picks the HTTP
 * status line. */
int cgi_emit_error(int fd,
                   int status_http,
                   const char *code,
                   const char *message);

/* Emit an HTTP success envelope wrapping a daemon response. The body
 * is forwarded verbatim. */
int cgi_emit_response(int fd, const void *body, size_t body_len);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_CGI_COMMON_H */
