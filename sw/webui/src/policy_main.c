/* sw/webui/src/policy_main.c — policy.cgi entry point.
 * DB-policy struct getter/setter per OPERATIONS.md §2.
 */
#include "tetra/cgi_common.h"

static const char *const k_policy_allowed[] = {
    "policy.get",
    "policy.put",
    NULL
};

int main(void)
{
    CgiRunOpts opts = {
        .script_name = "policy.cgi",
        .allowed_ops = k_policy_allowed,
        .fallback_op = "policy.get",
    };
    return cgi_run(&opts);
}
