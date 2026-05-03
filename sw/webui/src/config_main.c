/* sw/webui/src/config_main.c — config.cgi entry point.
 *
 * Maps the legacy `?op=get|put&group=NAME` URL form (OPERATIONS.md §4)
 * onto the dotted op family `config.<group>.<get|put>`. Allow-list
 * covers every group documented in §4.
 */
#include "tetra/cgi_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_config_allowed[] = {
    "config.cell.get",
    "config.cell.put",
    "config.rf.get",
    "config.rf.put",
    "config.cipher.get",
    "config.cipher.put",
    "config.training.get",
    "config.training.put",
    "config.slot_table.get",
    "config.slot_table.put",
    "config.ad9361.get",
    "config.ad9361.put",
    "config.msgbus.get",
    "config.msgbus.put",
    "config.schema",
    NULL
};

static int extract_qs(const char *qs, const char *key, char *dst, size_t cap)
{
    if (qs == NULL || key == NULL) return 0;
    const size_t klen = strlen(key);
    const char *p = qs;
    while (*p != '\0') {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        if (eq == NULL) break;
        if ((size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            const char *vstart = eq + 1;
            const char *vend = (amp != NULL) ? amp : (vstart + strlen(vstart));
            const size_t n = (size_t)(vend - vstart);
            if (n + 1 > cap) return -1;
            memcpy(dst, vstart, n);
            dst[n] = '\0';
            return 1;
        }
        if (amp == NULL) break;
        p = amp + 1;
    }
    return 0;
}

int main(void)
{
    const char *qs = getenv("QUERY_STRING");
    char op_q[16] = "";
    char grp[32]  = "";
    extract_qs(qs, "op", op_q, sizeof(op_q));
    extract_qs(qs, "group", grp, sizeof(grp));

    char synth[64] = "";
    if (op_q[0] != '\0' && grp[0] != '\0'
        && (strcmp(op_q, "get") == 0 || strcmp(op_q, "put") == 0
         || strcmp(op_q, "schema") == 0)) {
        if (strcmp(op_q, "schema") == 0) {
            snprintf(synth, sizeof(synth), "config.schema");
        } else {
            snprintf(synth, sizeof(synth), "config.%s.%s", grp, op_q);
        }
    }

    CgiRunOpts opts = {
        .script_name = "config.cgi",
        .allowed_ops = k_config_allowed,
        .fallback_op = (synth[0] != '\0') ? synth : NULL,
    };
    return cgi_run(&opts);
}
