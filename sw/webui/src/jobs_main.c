/* sw/webui/src/jobs_main.c — jobs.cgi entry point.
 * Async-job query surface per OPERATIONS.md §5 + §9.
 */
#include "tetra/cgi_common.h"

static const char *const k_jobs_allowed[] = {
    "jobs.list",
    "jobs.get",
    "jobs.cancel",
    NULL
};

int main(void)
{
    CgiRunOpts opts = {
        .script_name = "jobs.cgi",
        .allowed_ops = k_jobs_allowed,
        .fallback_op = "jobs.list",
    };
    return cgi_run(&opts);
}
