/* sw/webui/src/db_main.c — db.cgi entry point.
 * Bulk import / export / factory-reset per OPERATIONS.md §2 "Bulk DB".
 *
 * NOTE: db.import accepts a multipart/form-data upload. For the thin
 * CGI layer we forward the raw body — the daemon's import handler
 * does the multipart parse. Body cap is raised to fit a typical
 * db.json (1 MiB) per OPERATIONS.md §8 quota neighbours.
 */
#include "tetra/cgi_common.h"

static const char *const k_db_allowed[] = {
    "db.export",
    "db.import",
    "db.reset",
    NULL
};

int main(void)
{
    CgiRunOpts opts = {
        .script_name    = "db.cgi",
        .allowed_ops    = k_db_allowed,
        .max_body_bytes = 1024u * 1024u,
        .fallback_op    = "db.export",
    };
    return cgi_run(&opts);
}
