// -----------------------------------------------------------------------------
// cdc_pulse.v
//
// One-cycle pulse crossing from `src_clk` to `dst_clk` using the
// canonical TOGGLE-FLAG + 2-flop synchronizer + edge-detect pattern.
//
// Behavior:
//   - Each rising edge of `src_pulse` (1 src-clk cycle) toggles an internal
//     `tog` flop in the src domain.
//   - The toggle is synchronized into the dst domain through 3 flops
//     (extra 1st flop helps when src clock is faster than dst clock so the
//     toggle is registered before sampling).
//   - An edge-detect XOR generates a 1-dst-clk-cycle pulse on `dst_pulse`.
//
// Constraints:
//   - SRC must keep `src_pulse` LOW for at least 1 src-clk cycle between two
//     pulses, AND the inter-pulse separation in *time* must be ≥ 2 dst-clk
//     periods, otherwise pulses can merge in the dst domain. The TB asserts
//     this by spacing pulses with a margin matching the worst-case ratio.
//   - For very fast back-to-back pulses across slow → fast crossings, the
//     dst will still see one dst-cycle pulse per toggle (no merging).
//
// Reference: Cliff Cummings, SNUG 2008, "Pulse synchronizer".
// Locked interface: IF_CDC_v1.
// -----------------------------------------------------------------------------

`default_nettype none

module cdc_pulse (
    input  wire src_clk,
    input  wire dst_clk,
    input  wire src_pulse,
    output reg  dst_pulse
);

    // ---- src domain: convert pulse to toggle ---------------------------------
    reg src_tog;
    initial src_tog = 1'b0;

    always @(posedge src_clk) begin
        if (src_pulse)
            src_tog <= ~src_tog;
    end

    // ---- dst domain: 3-flop sync + edge-detect -------------------------------
    (* ASYNC_REG = "TRUE" *) reg dst_tog_meta;
    (* ASYNC_REG = "TRUE" *) reg dst_tog_sync;
    reg dst_tog_q;

    initial begin
        dst_tog_meta = 1'b0;
        dst_tog_sync = 1'b0;
        dst_tog_q    = 1'b0;
        dst_pulse    = 1'b0;
    end

    always @(posedge dst_clk) begin
        dst_tog_meta <= src_tog;
        dst_tog_sync <= dst_tog_meta;
        dst_tog_q    <= dst_tog_sync;
        dst_pulse    <= dst_tog_sync ^ dst_tog_q;
    end

endmodule

`default_nettype wire
