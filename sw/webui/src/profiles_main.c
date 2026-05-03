/* sw/webui/src/profiles_main.c — profiles.cgi entry point.
 *
 * Allow-list = OPERATIONS.md §2 "Profiles" endpoint catalog.
 * Profile schema (per references/reference_subscriber_db_arch.md):
 *   max_call_duration, hangtime, priority, gila_class, gila_lifetime,
 *   reserved3, permit_voice, permit_data, permit_reg, valid.
 */
#include "tetra/cgi_common.h"

static const char *const k_profiles_allowed[] = {
    "profile.list",
    "profile.get",
    "profile.put",
    "profile.delete",
    "profile.reset",
    NULL
};

int main(void)
{
    CgiRunOpts opts = {
        .script_name = "profiles.cgi",
        .allowed_ops = k_profiles_allowed,
        .fallback_op = "profile.list",
    };
    return cgi_run(&opts);
}
