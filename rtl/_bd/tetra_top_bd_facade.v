// rtl/_bd/tetra_top_bd_facade.v
//
// Owned by Phase 3.6 (BD-wiring follow-up to A1's slim AXI-DMA wrapper).
//
// Purpose: a thin façade around `tetra_top` that re-exports its
// interfaces with port-name prefixes that match Vivado IPI's bus-
// inference rules. This way, when Vivado adds the module to a Block
// Design via `create_bd_cell -type module -reference`, the standard
// AXI4-Lite slave + 4× AXI4-MM slim master interfaces appear as proper
// bus interfaces and can be wired with `connect_bd_intf_net`.
//
// Naming map (tetra_top ↔ façade):
//   s_axil_*                     →  s_axi_lite_*       (S_AXI_LITE bus)
//   m_axi_tma_rx_*               →  m_axi_tma_rx_*     (kept; slim-shape
//                                                       inference may
//                                                       still trigger;
//                                                       BD then handles
//                                                       per-pin connect
//                                                       via the slim
//                                                       completer)
//
// All other tetra_top ports pass through 1:1.
//
// Note: the slim AXI-MM master ports keep their `m_axi_*` prefix even
// though Vivado will fail to infer a bus interface for them (signal
// set incomplete). That's intentional — we connect them pin-by-pin
// inside `create_bd.tcl`. Renaming the AXI-Lite slave is the only
// strict requirement because the BD's `axi_ic_ctrl/M00_AXI` is a real
// bus and can only be connected via `connect_bd_intf_net`.
//
// LVDS handling (Phase 3.6 DRC IOSTDTYPE-1 fix):
//   The XDC declares all rx_*_p/n + tx_*_p/n pins as `IOSTANDARD
//   LVDS_25 DIFF_TERM TRUE` (carry-over). DRC IOSTDTYPE-1 requires
//   each LVDS_25 pair to be driven through IBUFDS / OBUFDS so the
//   pair is treated as a single differential signal at the IO buffer.
//   We instantiate the buffers HERE in the façade, present a single-
//   ended view to `tetra_top` (which still declares the pair ports
//   for API compatibility — the inner module's RX consumes the merged
//   single-ended representation; TX outputs a single-ended signal
//   that we drive into OBUFDS).
//
// Verilog-2001 only (CLAUDE.md §Languages).

`timescale 1ns / 1ps
`default_nettype none

module tetra_top_bd_facade (
    input  wire        clk_axi,
    input  wire        rstn_axi,
    input  wire        clk_sys,
    input  wire        rstn_sys,

    // AD9361 LVDS
    input  wire        rx_clk_in_p,
    input  wire        rx_clk_in_n,
    input  wire        rx_frame_in_p,
    input  wire        rx_frame_in_n,
    input  wire [5:0]  rx_data_in_p,
    input  wire [5:0]  rx_data_in_n,
    output wire        tx_clk_out_p,
    output wire        tx_clk_out_n,
    output wire        tx_frame_out_p,
    output wire        tx_frame_out_n,
    output wire [5:0]  tx_data_out_p,
    output wire [5:0]  tx_data_out_n,

    // AD9361 control + GPIO
    output wire        enable,
    output wire        txnrx,
    output wire        spi_csn,
    output wire        spi_clk,
    output wire        spi_mosi,
    input  wire        spi_miso,
    input  wire [7:0]  gpio_status,
    output wire [3:0]  gpio_ctl,
    output wire        gpio_en_agc,
    output wire        gpio_sync,
    output wire        gpio_resetb,

    // Board misc IO
    inout  wire        iic_scl,
    inout  wire        iic_sda,
    output wire        pl_led0,
    output wire        pl_led1,
    output wire        dac_sync,
    output wire        dac_sclk,
    output wire        dac_din,

    // AXI4-Lite slave — renamed prefix so Vivado IPI infers S_AXI_LITE.
    input  wire [11:0] s_axi_lite_awaddr,
    input  wire [2:0]  s_axi_lite_awprot,
    input  wire        s_axi_lite_awvalid,
    output wire        s_axi_lite_awready,
    input  wire [31:0] s_axi_lite_wdata,
    input  wire [3:0]  s_axi_lite_wstrb,
    input  wire        s_axi_lite_wvalid,
    output wire        s_axi_lite_wready,
    output wire [1:0]  s_axi_lite_bresp,
    output wire        s_axi_lite_bvalid,
    input  wire        s_axi_lite_bready,
    input  wire [11:0] s_axi_lite_araddr,
    input  wire [2:0]  s_axi_lite_arprot,
    input  wire        s_axi_lite_arvalid,
    output wire        s_axi_lite_arready,
    output wire [31:0] s_axi_lite_rdata,
    output wire [1:0]  s_axi_lite_rresp,
    output wire        s_axi_lite_rvalid,
    input  wire        s_axi_lite_rready,

    // 4× slim AXI4-MM master — pass-through port-by-port. The BD's
    // `create_bd.tcl` connects these pin-by-pin via the completer.
    output wire [31:0] m_axi_tma_rx_awaddr,
    output wire        m_axi_tma_rx_awvalid,
    input  wire        m_axi_tma_rx_awready,
    output wire [31:0] m_axi_tma_rx_wdata,
    output wire        m_axi_tma_rx_wvalid,
    input  wire        m_axi_tma_rx_wready,
    output wire        m_axi_tma_rx_wlast,
    input  wire [1:0]  m_axi_tma_rx_bresp,
    input  wire        m_axi_tma_rx_bvalid,
    output wire        m_axi_tma_rx_bready,

    output wire [31:0] m_axi_tma_tx_araddr,
    output wire        m_axi_tma_tx_arvalid,
    input  wire        m_axi_tma_tx_arready,
    input  wire [31:0] m_axi_tma_tx_rdata,
    input  wire        m_axi_tma_tx_rvalid,
    output wire        m_axi_tma_tx_rready,
    input  wire        m_axi_tma_tx_rlast,
    input  wire [1:0]  m_axi_tma_tx_rresp,

    output wire [31:0] m_axi_tmd_rx_awaddr,
    output wire        m_axi_tmd_rx_awvalid,
    input  wire        m_axi_tmd_rx_awready,
    output wire [31:0] m_axi_tmd_rx_wdata,
    output wire        m_axi_tmd_rx_wvalid,
    input  wire        m_axi_tmd_rx_wready,
    output wire        m_axi_tmd_rx_wlast,
    input  wire [1:0]  m_axi_tmd_rx_bresp,
    input  wire        m_axi_tmd_rx_bvalid,
    output wire        m_axi_tmd_rx_bready,

    output wire [31:0] m_axi_tmd_tx_araddr,
    output wire        m_axi_tmd_tx_arvalid,
    input  wire        m_axi_tmd_tx_arready,
    input  wire [31:0] m_axi_tmd_tx_rdata,
    input  wire        m_axi_tmd_tx_rvalid,
    output wire        m_axi_tmd_tx_rready,
    input  wire        m_axi_tmd_tx_rlast,
    input  wire [1:0]  m_axi_tmd_tx_rresp,

    // IRQs
    output wire        irq_tma_rx_o,
    output wire        irq_tma_tx_o,
    output wire        irq_tmd_rx_o,
    output wire        irq_tmd_tx_o
);

    // -----------------------------------------------------------------
    // LVDS pin buffers (DRC IOSTDTYPE-1 mandate).
    //
    // The XDC declares each rx_*_p/n + tx_*_p/n as a LVDS_25
    // differential pair. Without IBUFDS/OBUFDS Vivado fails DRC at
    // write_bitstream because it cannot model the IO standard for the
    // raw _p/_n pin pair as anything other than two unrelated SE pins.
    //
    // RX side: IBUFDS merges the pair into a single-ended logical net
    //          (`*_se`). Inner tetra_top still has _p/_n ports for API
    //          stability — we drive _p with the merged net and pin _n
    //          to 0 (the inner module's RX-stub consumes the _p side).
    //
    // TX side: tetra_top's TX is currently driven to constants (its
    //          `_p` outputs to 0 and `_n` outputs to 1). We collect the
    //          _p net and drive OBUFDS, which generates the proper
    //          LVDS pair on the device pins.
    // -----------------------------------------------------------------
    wire        rx_clk_in_se;
    wire        rx_frame_in_se;
    wire [5:0]  rx_data_in_se;

    IBUFDS u_ibufds_rx_clk   (.O(rx_clk_in_se),   .I(rx_clk_in_p),   .IB(rx_clk_in_n));
    IBUFDS u_ibufds_rx_frame (.O(rx_frame_in_se), .I(rx_frame_in_p), .IB(rx_frame_in_n));
    IBUFDS u_ibufds_rx_d0 (.O(rx_data_in_se[0]), .I(rx_data_in_p[0]), .IB(rx_data_in_n[0]));
    IBUFDS u_ibufds_rx_d1 (.O(rx_data_in_se[1]), .I(rx_data_in_p[1]), .IB(rx_data_in_n[1]));
    IBUFDS u_ibufds_rx_d2 (.O(rx_data_in_se[2]), .I(rx_data_in_p[2]), .IB(rx_data_in_n[2]));
    IBUFDS u_ibufds_rx_d3 (.O(rx_data_in_se[3]), .I(rx_data_in_p[3]), .IB(rx_data_in_n[3]));
    IBUFDS u_ibufds_rx_d4 (.O(rx_data_in_se[4]), .I(rx_data_in_p[4]), .IB(rx_data_in_n[4]));
    IBUFDS u_ibufds_rx_d5 (.O(rx_data_in_se[5]), .I(rx_data_in_p[5]), .IB(rx_data_in_n[5]));

    wire        tt_tx_clk_p,   tt_tx_clk_n_unused;
    wire        tt_tx_frame_p, tt_tx_frame_n_unused;
    wire [5:0]  tt_tx_data_p,  tt_tx_data_n_unused;

    OBUFDS u_obufds_tx_clk   (.I(tt_tx_clk_p),   .O(tx_clk_out_p),   .OB(tx_clk_out_n));
    OBUFDS u_obufds_tx_frame (.I(tt_tx_frame_p), .O(tx_frame_out_p), .OB(tx_frame_out_n));
    OBUFDS u_obufds_tx_d0 (.I(tt_tx_data_p[0]), .O(tx_data_out_p[0]), .OB(tx_data_out_n[0]));
    OBUFDS u_obufds_tx_d1 (.I(tt_tx_data_p[1]), .O(tx_data_out_p[1]), .OB(tx_data_out_n[1]));
    OBUFDS u_obufds_tx_d2 (.I(tt_tx_data_p[2]), .O(tx_data_out_p[2]), .OB(tx_data_out_n[2]));
    OBUFDS u_obufds_tx_d3 (.I(tt_tx_data_p[3]), .O(tx_data_out_p[3]), .OB(tx_data_out_n[3]));
    OBUFDS u_obufds_tx_d4 (.I(tt_tx_data_p[4]), .O(tx_data_out_p[4]), .OB(tx_data_out_n[4]));
    OBUFDS u_obufds_tx_d5 (.I(tt_tx_data_p[5]), .O(tx_data_out_p[5]), .OB(tx_data_out_n[5]));

    tetra_top u_tetra (
        .clk_axi              (clk_axi),
        .rstn_axi             (rstn_axi),
        .clk_sys              (clk_sys),
        .rstn_sys             (rstn_sys),

        // RX LVDS — drive _p from the merged single-ended IBUFDS
        // output; pin _n to 0 (inner module ignores _n for its RX-stub).
        .rx_clk_in_p          (rx_clk_in_se),
        .rx_clk_in_n          (1'b0),
        .rx_frame_in_p        (rx_frame_in_se),
        .rx_frame_in_n        (1'b0),
        .rx_data_in_p         (rx_data_in_se),
        .rx_data_in_n         (6'b0),
        // TX — collect _p net into OBUFDS; _n is unused (driven by
        // inner stub but ignored — it stays internal).
        .tx_clk_out_p         (tt_tx_clk_p),
        .tx_clk_out_n         (tt_tx_clk_n_unused),
        .tx_frame_out_p       (tt_tx_frame_p),
        .tx_frame_out_n       (tt_tx_frame_n_unused),
        .tx_data_out_p        (tt_tx_data_p),
        .tx_data_out_n        (tt_tx_data_n_unused),

        .enable               (enable),
        .txnrx                (txnrx),
        .spi_csn              (spi_csn),
        .spi_clk              (spi_clk),
        .spi_mosi             (spi_mosi),
        .spi_miso             (spi_miso),
        .gpio_status          (gpio_status),
        .gpio_ctl             (gpio_ctl),
        .gpio_en_agc          (gpio_en_agc),
        .gpio_sync            (gpio_sync),
        .gpio_resetb          (gpio_resetb),

        .iic_scl              (iic_scl),
        .iic_sda              (iic_sda),
        .pl_led0              (pl_led0),
        .pl_led1              (pl_led1),
        .dac_sync             (dac_sync),
        .dac_sclk             (dac_sclk),
        .dac_din              (dac_din),

        .s_axil_awaddr        (s_axi_lite_awaddr),
        .s_axil_awprot        (s_axi_lite_awprot),
        .s_axil_awvalid       (s_axi_lite_awvalid),
        .s_axil_awready       (s_axi_lite_awready),
        .s_axil_wdata         (s_axi_lite_wdata),
        .s_axil_wstrb         (s_axi_lite_wstrb),
        .s_axil_wvalid        (s_axi_lite_wvalid),
        .s_axil_wready        (s_axi_lite_wready),
        .s_axil_bresp         (s_axi_lite_bresp),
        .s_axil_bvalid        (s_axi_lite_bvalid),
        .s_axil_bready        (s_axi_lite_bready),
        .s_axil_araddr        (s_axi_lite_araddr),
        .s_axil_arprot        (s_axi_lite_arprot),
        .s_axil_arvalid       (s_axi_lite_arvalid),
        .s_axil_arready       (s_axi_lite_arready),
        .s_axil_rdata         (s_axi_lite_rdata),
        .s_axil_rresp         (s_axi_lite_rresp),
        .s_axil_rvalid        (s_axi_lite_rvalid),
        .s_axil_rready        (s_axi_lite_rready),

        .m_axi_tma_rx_awaddr  (m_axi_tma_rx_awaddr),
        .m_axi_tma_rx_awvalid (m_axi_tma_rx_awvalid),
        .m_axi_tma_rx_awready (m_axi_tma_rx_awready),
        .m_axi_tma_rx_wdata   (m_axi_tma_rx_wdata),
        .m_axi_tma_rx_wvalid  (m_axi_tma_rx_wvalid),
        .m_axi_tma_rx_wready  (m_axi_tma_rx_wready),
        .m_axi_tma_rx_wlast   (m_axi_tma_rx_wlast),
        .m_axi_tma_rx_bresp   (m_axi_tma_rx_bresp),
        .m_axi_tma_rx_bvalid  (m_axi_tma_rx_bvalid),
        .m_axi_tma_rx_bready  (m_axi_tma_rx_bready),

        .m_axi_tma_tx_araddr  (m_axi_tma_tx_araddr),
        .m_axi_tma_tx_arvalid (m_axi_tma_tx_arvalid),
        .m_axi_tma_tx_arready (m_axi_tma_tx_arready),
        .m_axi_tma_tx_rdata   (m_axi_tma_tx_rdata),
        .m_axi_tma_tx_rvalid  (m_axi_tma_tx_rvalid),
        .m_axi_tma_tx_rready  (m_axi_tma_tx_rready),
        .m_axi_tma_tx_rlast   (m_axi_tma_tx_rlast),
        .m_axi_tma_tx_rresp   (m_axi_tma_tx_rresp),

        .m_axi_tmd_rx_awaddr  (m_axi_tmd_rx_awaddr),
        .m_axi_tmd_rx_awvalid (m_axi_tmd_rx_awvalid),
        .m_axi_tmd_rx_awready (m_axi_tmd_rx_awready),
        .m_axi_tmd_rx_wdata   (m_axi_tmd_rx_wdata),
        .m_axi_tmd_rx_wvalid  (m_axi_tmd_rx_wvalid),
        .m_axi_tmd_rx_wready  (m_axi_tmd_rx_wready),
        .m_axi_tmd_rx_wlast   (m_axi_tmd_rx_wlast),
        .m_axi_tmd_rx_bresp   (m_axi_tmd_rx_bresp),
        .m_axi_tmd_rx_bvalid  (m_axi_tmd_rx_bvalid),
        .m_axi_tmd_rx_bready  (m_axi_tmd_rx_bready),

        .m_axi_tmd_tx_araddr  (m_axi_tmd_tx_araddr),
        .m_axi_tmd_tx_arvalid (m_axi_tmd_tx_arvalid),
        .m_axi_tmd_tx_arready (m_axi_tmd_tx_arready),
        .m_axi_tmd_tx_rdata   (m_axi_tmd_tx_rdata),
        .m_axi_tmd_tx_rvalid  (m_axi_tmd_tx_rvalid),
        .m_axi_tmd_tx_rready  (m_axi_tmd_tx_rready),
        .m_axi_tmd_tx_rlast   (m_axi_tmd_tx_rlast),
        .m_axi_tmd_tx_rresp   (m_axi_tmd_tx_rresp),

        .irq_tma_rx_o (irq_tma_rx_o),
        .irq_tma_tx_o (irq_tma_tx_o),
        .irq_tmd_rx_o (irq_tmd_rx_o),
        .irq_tmd_tx_o (irq_tmd_tx_o)
    );

endmodule

`default_nettype wire
