/* sw/webui/src/status_main.c — status.cgi entry point.
 *
 * Maps the legacy `?scope=NAME` URL form (OPERATIONS.md §1) onto the
 * dotted op family `status.<scope>`. CGIs that take only ?scope= use a
 * fallback_op of "status.summary" when nothing is provided.
 *
 * Allow-list = §1 endpoint catalog (read-only).
 */
#include "tetra/cgi_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_status_allowed[] = {
    "status.summary",
    "status.layers",
    "status.phy_stats",
    "status.dma_stats",
    "status.txq",
    "status.msgbus",
    "status.calls",
    "status.groups",
    "status.fpga_regs",
    "status.aach_current",
    "status.snapshot",
    NULL
};

int main(void)
{
    /* Translate ?scope=NAME → op=status.NAME if op= is not set. */
    const char *qs = getenv("QUERY_STRING");
    char synthesised[64] = "";
    if (qs != NULL && strstr(qs, "op=") == NULL) {
        const char *s = strstr(qs, "scope=");
        if (s != NULL) {
            s += 6;
            const char *e = strchr(s, '&');
            const size_t n = (e != NULL) ? (size_t)(e - s) : strlen(s);
            if (n < sizeof(synthesised) - 8) {
                snprintf(synthesised, sizeof(synthesised),
                         "status.%.*s", (int)n, s);
            }
        }
    }

    CgiRunOpts opts = {
        .script_name = "status.cgi",
        .allowed_ops = k_status_allowed,
        .fallback_op = (synthesised[0] != '\0') ? synthesised : "status.summary",
    };
    return cgi_run(&opts);
}
