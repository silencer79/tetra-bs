/* sw/webui/src/debug_main.c — debug.cgi entry point.
 * Allow-list = OPERATIONS.md §3 Debug catalog.
 *
 * SSE streaming ops (`*_stream`) are forwarded too, but the current
 * thin client returns one envelope and exits. A dedicated SSE pump
 * lives outside cgi_common (TBD); the allow-list keeps the op-names
 * reserved.
 */
#include "tetra/cgi_common.h"

static const char *const k_debug_allowed[] = {
    "debug.pdu_recent",
    "debug.pdu_stream",
    "debug.aach_recent",
    "debug.aach_stream",
    "debug.slot_schedule",
    "debug.irq_counters",
    "debug.msgbus_stream",
    "debug.log_tail",
    "debug.reg_read",
    "debug.reg_write",
    "debug.tmasap_recent",
    "debug.tmdsap_stats",
    NULL
};

int main(void)
{
    CgiRunOpts opts = {
        .script_name = "debug.cgi",
        .allowed_ops = k_debug_allowed,
        .fallback_op = "debug.aach_recent",
    };
    return cgi_run(&opts);
}
