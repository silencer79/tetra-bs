/* sw/webui/src/entities_main.c — entities.cgi entry point.
 * Allow-list = OPERATIONS.md §2 "Entities" endpoint catalog.
 */
#include "tetra/cgi_common.h"

static const char *const k_entities_allowed[] = {
    "entity.list",
    "entity.get",
    "entity.lookup",
    "entity.put",
    "entity.delete",
    "entity.clear_all",
    NULL
};

int main(void)
{
    CgiRunOpts opts = {
        .script_name = "entities.cgi",
        .allowed_ops = k_entities_allowed,
        .fallback_op = "entity.list",
    };
    return cgi_run(&opts);
}
