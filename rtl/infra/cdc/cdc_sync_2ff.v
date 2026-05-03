// -----------------------------------------------------------------------------
// cdc_sync_2ff.v
//
// Generic 2-flop synchronizer for slow-changing single- (or multi-) bit signals
// crossing from `src_clk` domain into `dst_clk` domain.
//
// IMPORTANT:
// - For multi-bit busses where the bits change INDEPENDENTLY (e.g. parallel
//   data words), this primitive is NOT safe — use `cdc_handshake` or
//   `cdc_async_fifo` instead. The WIDTH parameter is provided so that this
//   module can be reused for grouped synchronizers where each bit can be
//   treated as an INDEPENDENT slow-changing single-bit signal (e.g. an array
//   of single-bit IRQ lines, gray-coded pointers, etc).
//
// - Both flops carry the Vivado `(* ASYNC_REG = "TRUE" *)` attribute so the
//   tools place them in the same slice (low MTBF synchronizer chain).
//
// Locked interface: IF_CDC_v1 — see docs/MIGRATION_PLAN.md §A4.
// Reference: Cliff Cummings, "Clock Domain Crossing (CDC) Design & Verification
// Techniques Using SystemVerilog", SNUG 2008.
// -----------------------------------------------------------------------------

`default_nettype none

module cdc_sync_2ff #(
    parameter WIDTH = 1
) (
    // src_clk_unused: kept in port-list per IF_CDC_v1 to make all CDC blocks
    // share a uniform "src/dst" port-naming style; not connected internally
    // because a 2-flop synchronizer samples directly in the dst domain.
    input  wire             src_clk_unused,
    input  wire             dst_clk,
    input  wire [WIDTH-1:0] in,
    output reg  [WIDTH-1:0] out
);

    (* ASYNC_REG = "TRUE" *) reg [WIDTH-1:0] sync_meta;
    (* ASYNC_REG = "TRUE" *) reg [WIDTH-1:0] sync_stable;

    always @(posedge dst_clk) begin
        sync_meta   <= in;
        sync_stable <= sync_meta;
    end

    // Drive `out` from the second flop. Kept as a separate `always` block
    // (rather than aliasing) so synthesis sees a clean named flop on the
    // output without optimizing the chain together.
    always @(posedge dst_clk) begin
        out <= sync_stable;
    end

    // src_clk_unused is intentionally unused; reference it to silence lint.
    wire _unused_src_clk = src_clk_unused;

endmodule

`default_nettype wire
