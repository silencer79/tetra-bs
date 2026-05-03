/* sw/webui/src/sessions_main.c — sessions.cgi entry point.
 * Read-only mirror of the in-memory AST. Allow-list per
 * OPERATIONS.md §2 "Sessions".
 */
#include "tetra/cgi_common.h"

static const char *const k_sessions_allowed[] = {
    "session.list",
    "session.get",
    "session.stats",
    NULL
};

int main(void)
{
    CgiRunOpts opts = {
        .script_name = "sessions.cgi",
        .allowed_ops = k_sessions_allowed,
        .fallback_op = "session.list",
    };
    return cgi_run(&opts);
}
