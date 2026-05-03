/* sw/webui/src/stop_main.c — stop.cgi entry point.
 * Graceful daemon shutdown per OPERATIONS.md §5 (daemon.stop).
 */
#include "tetra/cgi_common.h"

static const char *const k_stop_allowed[] = {
    "daemon.stop",
    NULL
};

int main(void)
{
    CgiRunOpts opts = {
        .script_name = "stop.cgi",
        .allowed_ops = k_stop_allowed,
        .fallback_op = "daemon.stop",
    };
    return cgi_run(&opts);
}
