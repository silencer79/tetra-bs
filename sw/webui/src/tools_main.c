/* sw/webui/src/tools_main.c — tools.cgi entry point.
 *
 * Catch-all for §5 Tools operator-action surface. Long-running ops
 * (capture_start, bitstream_switch, decoder_run, restart) are async-
 * job dispatchers — daemon enqueues + returns {job_id} per
 * OPERATIONS.md §9.
 */
#include "tetra/cgi_common.h"

static const char *const k_tools_allowed[] = {
    "tools.pdu_send_form",
    "tools.pdu_send",
    "tools.reset_counters",
    "tools.reset_ast",
    "tools.reg_write",
    "tools.bitstream_list",
    "tools.bitstream_switch",
    "tools.capture_start",
    "tools.capture_stop",
    "tools.capture_list",
    "tools.capture_download",
    "tools.decoder_upload",
    "tools.decoder_list",
    "tools.decoder_run",
    "tools.decoder_result",
    "tools.decoder_artifact",
    "daemon.restart",
    NULL
};

int main(void)
{
    CgiRunOpts opts = {
        .script_name    = "tools.cgi",
        .allowed_ops    = k_tools_allowed,
        .max_body_bytes = 1024u * 1024u,  /* decoder uploads up to 256 KiB + multipart headers */
    };
    return cgi_run(&opts);
}
