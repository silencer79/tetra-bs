// -----------------------------------------------------------------------------
// cdc_handshake.v
//
// Closed-loop request/acknowledge handshake for occasional multi-bit transfers
// between two clock domains. Use for:
//   - AXI-Lite config register writes pushed from clk_axi → clk_sys
//   - Periodic status snapshots latched in clk_sys and read in clk_axi
//
// Operation (4-phase):
//   1. SRC asserts `src_valid` with stable `src_data`.
//      Module captures data into `data_hold`, asserts internal `req` toggle.
//   2. The toggle is 2-flop-synchronized into DST, edge-detected, raises
//      `dst_valid` for one dst-clk cycle alongside the held `data_hold`.
//   3. DST consumes the data; on the SAME cycle (combinationally) or any
//      later cycle it pulses `dst_ack` (1 dst-clk cycle).
//   4. The ack toggle is 2-flop-synchronized back into SRC; once seen, the
//      module deasserts `src_ready` low for the duration of the round-trip
//      and raises it again when the loop closes, signalling SRC may issue
//      the next word.
//
// Throughput: 1 word per ~ (3*Tdst + 3*Tsrc) — adequate for config/status,
// not for sustained streams (use cdc_async_fifo for those).
//
// Multi-bit safety: `data_hold` is written ONLY while no transfer is in
// flight (src_ready=1) and read in DST only after the synchronized req
// edge — bus is therefore stable across sampling, satisfying CDC.
//
// Locked interface: IF_CDC_v1.
// Reference: Cummings SNUG 2008, "Closed-loop MCP formulation".
// -----------------------------------------------------------------------------

`default_nettype none

module cdc_handshake #(
    parameter WIDTH = 32
) (
    // ---- src domain ----
    input  wire             src_clk,
    input  wire [WIDTH-1:0] src_data,
    input  wire             src_valid,
    output wire             src_ready,

    // ---- dst domain ----
    input  wire             dst_clk,
    output wire [WIDTH-1:0] dst_data,
    output reg              dst_valid,
    input  wire             dst_ack
);

    // -------------------------------------------------------------------------
    // SRC domain
    // -------------------------------------------------------------------------
    reg              src_req_tog;       // toggles on each accepted word
    reg [WIDTH-1:0]  data_hold;         // latched payload, stable until ack

    initial begin
        src_req_tog = 1'b0;
        data_hold   = {WIDTH{1'b0}};
    end

    // ack toggle synchronized back into src domain
    (* ASYNC_REG = "TRUE" *) reg src_ack_meta;
    (* ASYNC_REG = "TRUE" *) reg src_ack_sync;
    reg                          src_ack_seen;   // last-seen ack-toggle level

    initial begin
        src_ack_meta = 1'b0;
        src_ack_sync = 1'b0;
        src_ack_seen = 1'b0;
    end

    // ready when last issued req has been ack'd (toggles match)
    assign src_ready = (src_req_tog == src_ack_seen);

    always @(posedge src_clk) begin
        src_ack_meta <= dst_ack_tog;
        src_ack_sync <= src_ack_meta;
        src_ack_seen <= src_ack_sync;

        if (src_valid && src_ready) begin
            data_hold   <= src_data;
            src_req_tog <= ~src_req_tog;
        end
    end

    // -------------------------------------------------------------------------
    // DST domain
    // -------------------------------------------------------------------------
    (* ASYNC_REG = "TRUE" *) reg dst_req_meta;
    (* ASYNC_REG = "TRUE" *) reg dst_req_sync;
    reg                          dst_req_q;
    reg                          dst_ack_tog;

    initial begin
        dst_req_meta = 1'b0;
        dst_req_sync = 1'b0;
        dst_req_q    = 1'b0;
        dst_ack_tog  = 1'b0;
        dst_valid    = 1'b0;
    end

    // data_hold is stable while a request is in flight; safe to drive across
    // domains as a CDC-quasi-static bus per the closed-loop MCP pattern.
    assign dst_data = data_hold;

    always @(posedge dst_clk) begin
        dst_req_meta <= src_req_tog;
        dst_req_sync <= dst_req_meta;
        dst_req_q    <= dst_req_sync;

        // 1-cycle valid pulse on every detected req edge
        dst_valid    <= (dst_req_sync ^ dst_req_q);

        // toggle ack on consumer-driven dst_ack pulse
        if (dst_ack)
            dst_ack_tog <= ~dst_ack_tog;
    end

endmodule

`default_nettype wire
