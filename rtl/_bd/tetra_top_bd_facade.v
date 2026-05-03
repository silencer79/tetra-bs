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

    tetra_top u_tetra (
        .clk_axi              (clk_axi),
        .rstn_axi             (rstn_axi),
        .clk_sys              (clk_sys),
        .rstn_sys             (rstn_sys),

        .rx_clk_in_p          (rx_clk_in_p),
        .rx_clk_in_n          (rx_clk_in_n),
        .rx_frame_in_p        (rx_frame_in_p),
        .rx_frame_in_n        (rx_frame_in_n),
        .rx_data_in_p         (rx_data_in_p),
        .rx_data_in_n         (rx_data_in_n),
        .tx_clk_out_p         (tx_clk_out_p),
        .tx_clk_out_n         (tx_clk_out_n),
        .tx_frame_out_p       (tx_frame_out_p),
        .tx_frame_out_n       (tx_frame_out_n),
        .tx_data_out_p        (tx_data_out_p),
        .tx_data_out_n        (tx_data_out_n),

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
