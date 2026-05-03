/* sw/webui/src/apply_main.c — apply.cgi entry point.
 * Staged-config commit/discard per OPERATIONS.md §4.
 */
#include "tetra/cgi_common.h"

static const char *const k_apply_allowed[] = {
    "apply.status",
    "apply.apply",
    "apply.discard",
    NULL
};

int main(void)
{
    CgiRunOpts opts = {
        .script_name = "apply.cgi",
        .allowed_ops = k_apply_allowed,
        .fallback_op = "apply.status",
    };
    return cgi_run(&opts);
}
