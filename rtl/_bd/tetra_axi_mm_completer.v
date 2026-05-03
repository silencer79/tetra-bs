// rtl/_bd/tetra_axi_mm_completer.v
//
// Owned by Phase 3.6 (BD-wiring follow-up to A1's slim AXI-DMA wrapper).
//
// Purpose: bridge the *slim* AXI4-MM master shape exposed at `tetra_top`
// (inherited from the SG-shim port-list IF_AXIDMA_v1) to the *full*
// AXI4-MM signal set required by `axi_interconnect:2.1` /
// `smartconnect:1.0` so the masters can be wired into PS S_AXI_HP0.
//
// The slim master only carries:
//   write side: awaddr/awvalid/awready/wdata/wvalid/wready/wlast/
//               bresp/bvalid/bready
//   read  side: araddr/arvalid/arready/rdata/rvalid/rready/rlast/rresp
//
// The interconnect needs the full AXI4 sideband:
//   awid/awlen/awsize/awburst/awcache/awprot/awlock/awqos/awregion
//   wstrb
//   bid
//   arid/arlen/arsize/arburst/arcache/arprot/arlock/arqos/arregion
//   rid
//
// Completion strategy (locked, matches the IP's runtime behaviour for
// 32-bit data + INCR singles via S_AXIS_*_TDATA = 32 bit):
//   - AWID/ARID = 0 (single master ID, S_AXI_HP0 is shared but the
//     interconnect inserts a routing tag automatically)
//   - AWLEN/ARLEN = 0 (one beat per transaction — the SG-shim's outer
//     burst-collapse logic already issues 1-beat transactions to the
//     slim port; only the IP-internal MM2S/S2MM drives the actual long
//     bursts onto the local descriptor-ring AXI fabric inside the shim)
//
//     NOTE — this assumption is the conservative bring-up choice. If
//     post-silicon profiling shows the slim port can carry multi-beat
//     bursts, AWLEN/ARLEN can be promoted to a parameter port and the
//     shim will be updated to drive it. For Phase 3.6 the goal is
//     "real datapath survives opt_design", not "max throughput".
//
//   - AWSIZE/ARSIZE = 3'b010 (4 bytes — DATA_WIDTH=32)
//   - AWBURST/ARBURST = 2'b01 (INCR; harmless for 1-beat txns)
//   - AWCACHE/ARCACHE = 4'b0011 (Bufferable + Modifiable — ZynqMP-friendly)
//   - AWPROT/ARPROT = 3'b000 (data, secure, unprivileged)
//   - AWLOCK/ARLOCK = 1'b0 (no exclusive access)
//   - AWQOS/ARQOS = 4'b0000
//   - AWREGION/ARREGION = 4'b0000
//   - WSTRB = 4'b1111 (all bytes valid — 32-bit aligned data)
//
// Direction-handling:
//   `DIR_IS_S2MM=1` → write-only path used (read side tied off).
//   `DIR_IS_S2MM=0` → read-only path used (write side tied off).
//
// Per-channel instantiation: 4 instances at the BD level (tma_rx,
// tma_tx, tmd_rx, tmd_tx). Each connects on the slim side to the
// corresponding `tetra_top.m_axi_*` port (pin-by-pin, no bus inference)
// and on the full side to the `axi_interconnect`'s S_AXI slave (via
// Vivado's standard `m_axi_*` port-name inference).
//
// Verilog-2001 only (CLAUDE.md §Languages).

`timescale 1ns / 1ps
`default_nettype none

module tetra_axi_mm_completer #(
    parameter integer DIR_IS_S2MM = 1,   // 1 = write-only S2MM, 0 = read-only MM2S
    parameter integer ADDR_WIDTH  = 32,
    parameter integer DATA_WIDTH  = 32,
    parameter integer ID_WIDTH    = 1
) (
    // Clock + reset (used only for bus-interface association in the BD;
    // the slim→full mapping is purely combinational).
    input  wire                      aclk,
    input  wire                      aresetn,

    // ---- Slim master side (driven by tetra_top.m_axi_*) -------------
    // These do NOT use the standard `s_axi_*` prefix — that prevents
    // Vivado from auto-inferring a bus interface and lets the BD
    // connect them pin-by-pin to tetra_top's slim master ports.
    input  wire [ADDR_WIDTH-1:0]     slim_awaddr_in,
    input  wire                      slim_awvalid_in,
    output wire                      slim_awready_out,
    input  wire [DATA_WIDTH-1:0]     slim_wdata_in,
    input  wire                      slim_wvalid_in,
    output wire                      slim_wready_out,
    input  wire                      slim_wlast_in,
    output wire [1:0]                slim_bresp_out,
    output wire                      slim_bvalid_out,
    input  wire                      slim_bready_in,

    input  wire [ADDR_WIDTH-1:0]     slim_araddr_in,
    input  wire                      slim_arvalid_in,
    output wire                      slim_arready_out,
    output wire [DATA_WIDTH-1:0]     slim_rdata_out,
    output wire                      slim_rvalid_out,
    input  wire                      slim_rready_in,
    output wire                      slim_rlast_out,
    output wire [1:0]                slim_rresp_out,

    // ---- Full master side (m_axi_* prefix → Vivado infers M_AXI bus) -
    output wire [ID_WIDTH-1:0]       m_axi_awid,
    output wire [ADDR_WIDTH-1:0]     m_axi_awaddr,
    output wire [7:0]                m_axi_awlen,
    output wire [2:0]                m_axi_awsize,
    output wire [1:0]                m_axi_awburst,
    output wire                      m_axi_awlock,
    output wire [3:0]                m_axi_awcache,
    output wire [2:0]                m_axi_awprot,
    output wire [3:0]                m_axi_awqos,
    output wire [3:0]                m_axi_awregion,
    output wire                      m_axi_awvalid,
    input  wire                      m_axi_awready,

    output wire [DATA_WIDTH-1:0]     m_axi_wdata,
    output wire [(DATA_WIDTH/8)-1:0] m_axi_wstrb,
    output wire                      m_axi_wlast,
    output wire                      m_axi_wvalid,
    input  wire                      m_axi_wready,

    input  wire [ID_WIDTH-1:0]       m_axi_bid,
    input  wire [1:0]                m_axi_bresp,
    input  wire                      m_axi_bvalid,
    output wire                      m_axi_bready,

    output wire [ID_WIDTH-1:0]       m_axi_arid,
    output wire [ADDR_WIDTH-1:0]     m_axi_araddr,
    output wire [7:0]                m_axi_arlen,
    output wire [2:0]                m_axi_arsize,
    output wire [1:0]                m_axi_arburst,
    output wire                      m_axi_arlock,
    output wire [3:0]                m_axi_arcache,
    output wire [2:0]                m_axi_arprot,
    output wire [3:0]                m_axi_arqos,
    output wire [3:0]                m_axi_arregion,
    output wire                      m_axi_arvalid,
    input  wire                      m_axi_arready,

    input  wire [ID_WIDTH-1:0]       m_axi_rid,
    input  wire [DATA_WIDTH-1:0]     m_axi_rdata,
    input  wire [1:0]                m_axi_rresp,
    input  wire                      m_axi_rlast,
    input  wire                      m_axi_rvalid,
    output wire                      m_axi_rready
);

    // -----------------------------------------------------------------
    // Static sideband (same on every transaction)
    // -----------------------------------------------------------------
    localparam [2:0] SIZE_4B    = 3'b010;       // DATA_WIDTH=32 -> 4 bytes
    localparam [1:0] BURST_INCR = 2'b01;
    localparam [3:0] CACHE_BM   = 4'b0011;      // Bufferable + Modifiable
    localparam [2:0] PROT_DATA  = 3'b000;
    localparam       LOCK_NORM  = 1'b0;
    localparam [3:0] QOS_ZERO   = 4'b0000;
    localparam [3:0] REGN_ZERO  = 4'b0000;

    // -----------------------------------------------------------------
    // Write side
    // -----------------------------------------------------------------
    generate
    if (DIR_IS_S2MM) begin : g_w_active
        assign m_axi_awid     = {ID_WIDTH{1'b0}};
        assign m_axi_awaddr   = slim_awaddr_in;
        assign m_axi_awlen    = 8'd0;            // single-beat
        assign m_axi_awsize   = SIZE_4B;
        assign m_axi_awburst  = BURST_INCR;
        assign m_axi_awlock   = LOCK_NORM;
        assign m_axi_awcache  = CACHE_BM;
        assign m_axi_awprot   = PROT_DATA;
        assign m_axi_awqos    = QOS_ZERO;
        assign m_axi_awregion = REGN_ZERO;
        assign m_axi_awvalid  = slim_awvalid_in;
        assign slim_awready_out = m_axi_awready;

        assign m_axi_wdata    = slim_wdata_in;
        assign m_axi_wstrb    = {(DATA_WIDTH/8){1'b1}};
        assign m_axi_wlast    = slim_wlast_in;
        assign m_axi_wvalid   = slim_wvalid_in;
        assign slim_wready_out = m_axi_wready;

        assign m_axi_bready   = slim_bready_in;
        assign slim_bvalid_out = m_axi_bvalid;
        assign slim_bresp_out  = m_axi_bresp;
    end else begin : g_w_off
        // Read-only direction — write channel completely tied off.
        assign m_axi_awid     = {ID_WIDTH{1'b0}};
        assign m_axi_awaddr   = {ADDR_WIDTH{1'b0}};
        assign m_axi_awlen    = 8'd0;
        assign m_axi_awsize   = SIZE_4B;
        assign m_axi_awburst  = BURST_INCR;
        assign m_axi_awlock   = LOCK_NORM;
        assign m_axi_awcache  = CACHE_BM;
        assign m_axi_awprot   = PROT_DATA;
        assign m_axi_awqos    = QOS_ZERO;
        assign m_axi_awregion = REGN_ZERO;
        assign m_axi_awvalid  = 1'b0;
        assign slim_awready_out = 1'b1;          // accept any phantom AW

        assign m_axi_wdata    = {DATA_WIDTH{1'b0}};
        assign m_axi_wstrb    = {(DATA_WIDTH/8){1'b0}};
        assign m_axi_wlast    = 1'b0;
        assign m_axi_wvalid   = 1'b0;
        assign slim_wready_out = 1'b1;

        assign m_axi_bready   = 1'b1;
        assign slim_bvalid_out = 1'b0;
        assign slim_bresp_out  = 2'b00;
    end
    endgenerate

    // -----------------------------------------------------------------
    // Read side
    // -----------------------------------------------------------------
    generate
    if (!DIR_IS_S2MM) begin : g_r_active
        assign m_axi_arid     = {ID_WIDTH{1'b0}};
        assign m_axi_araddr   = slim_araddr_in;
        assign m_axi_arlen    = 8'd0;
        assign m_axi_arsize   = SIZE_4B;
        assign m_axi_arburst  = BURST_INCR;
        assign m_axi_arlock   = LOCK_NORM;
        assign m_axi_arcache  = CACHE_BM;
        assign m_axi_arprot   = PROT_DATA;
        assign m_axi_arqos    = QOS_ZERO;
        assign m_axi_arregion = REGN_ZERO;
        assign m_axi_arvalid  = slim_arvalid_in;
        assign slim_arready_out = m_axi_arready;

        assign m_axi_rready   = slim_rready_in;
        assign slim_rvalid_out = m_axi_rvalid;
        assign slim_rdata_out  = m_axi_rdata;
        assign slim_rresp_out  = m_axi_rresp;
        assign slim_rlast_out  = m_axi_rlast;
    end else begin : g_r_off
        assign m_axi_arid     = {ID_WIDTH{1'b0}};
        assign m_axi_araddr   = {ADDR_WIDTH{1'b0}};
        assign m_axi_arlen    = 8'd0;
        assign m_axi_arsize   = SIZE_4B;
        assign m_axi_arburst  = BURST_INCR;
        assign m_axi_arlock   = LOCK_NORM;
        assign m_axi_arcache  = CACHE_BM;
        assign m_axi_arprot   = PROT_DATA;
        assign m_axi_arqos    = QOS_ZERO;
        assign m_axi_arregion = REGN_ZERO;
        assign m_axi_arvalid  = 1'b0;
        assign slim_arready_out = 1'b1;

        assign m_axi_rready   = 1'b1;
        assign slim_rvalid_out = 1'b0;
        assign slim_rdata_out  = {DATA_WIDTH{1'b0}};
        assign slim_rresp_out  = 2'b00;
        assign slim_rlast_out  = 1'b0;
    end
    endgenerate

    // Suppress unused warnings — aclk/aresetn are present on the port
    // list because every AXI master in a BD must be associated with a
    // clock + reset for ASSOCIATED_BUSIF resolution. The slim→full
    // mapping itself is purely combinational.
    /* verilator lint_off UNUSED */
    wire _unused_clk_rstn = aclk | aresetn;
    /* verilator lint_on UNUSED */

endmodule

`default_nettype wire
