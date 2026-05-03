// rtl/tetra_synth_top.v
//
// Synthesis-only top-level wrapper for `tetra_top`.
//
// Why this exists: `rtl/tetra_top.v` exposes 4× AXI4-MM master ports
// + 1× AXI4-Lite slave + 4 IRQ outputs at the top-level interface
// (per IF_TETRA_TOP_v1, locked). Those interfaces are intentional —
// they describe the boundary between the PL and the PS-side AXI fabric,
// and are how the full system gets wired up by the *next* tooling step
// (Vivado block design + PS7 IP, follow-up to Phase 3.5).
//
// As long as that wiring step has not yet been added, running Vivado P&R
// on `tetra_top` directly fails with `[Place 30-415] IO Placement
// failed due to overutilization` — the LibreSDR's xc7z020clg400-1
// has 221 user I/O while `tetra_top` declares 402 top-level ports.
//
// To still produce a bitstream end-to-end (Phase 3.5 hardware-bringup
// gate), this wrapper:
//   - Exposes ONLY the real board pins (AD9361 LVDS, AD9361 control/GPIO,
//     I2C, board misc IO, plus a single PL clock+reset pin pair). All
//     other PL→PS interfaces are absorbed internally.
//   - Internally instantiates `tetra_top` with all AXI ports tied off
//     to a benign no-op slave/master. The IRQs are merged into a single
//     PL LED so they remain visible. The AXI-Lite slave is held idle.
//
// This is a transient bring-up artefact. The follow-up that adds a real
// PS7 instance (or a Vivado block design hosting `tetra_top`) replaces
// this wrapper.
//
// Verilog-2001 only (CLAUDE.md §Languages).

`timescale 1ns / 1ps
`default_nettype none

module tetra_synth_top (
    // PL clock + reset (board pins).
    input  wire        clk_axi,
    input  wire        rstn_axi,
    input  wire        clk_sys,
    input  wire        rstn_sys,

    // AD9361 LVDS (matches `constraints/libresdr_tetra.xdc`).
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

    // AD9361 control + GPIO (carry-over).
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

    // Board misc IO (carry-over).
    inout  wire        iic_scl,
    inout  wire        iic_sda,
    output wire        pl_led0,
    output wire        pl_led1,
    output wire        dac_sync,
    output wire        dac_sclk,
    output wire        dac_din
);

    // -----------------------------------------------------------------
    // Internal AXI-MM master tie-offs.
    // The AXI-DMA wrapper expects a slave to ack address/data and signal
    // bvalid/rvalid. We provide a degenerate always-ready / always-zero
    // slave so the masters don't stall mid-burst (FF synthesis would
    // otherwise leave the read/write FSMs stuck and Vivado may complain).
    //
    // Per channel: capture aw → emit bvalid one cycle later (ok-resp);
    //              capture ar → emit rvalid one cycle later (zero data,
    //                           rlast asserted to terminate any burst).
    // The IP only ever issues bursts of length up to ~256 — collapsing
    // every burst to a single beat is fine because the data is going
    // nowhere; the IP's own status registers track the (fake) completion.
    // -----------------------------------------------------------------
    // ---- ch0 tma_rx (S2MM write) ----
    wire        m_axi_tma_rx_awvalid;
    wire        m_axi_tma_rx_wvalid;
    wire        m_axi_tma_rx_bready;
    reg         m_axi_tma_rx_bvalid_r;
    wire [31:0] m_axi_tma_rx_awaddr_unused;
    wire [31:0] m_axi_tma_rx_wdata_unused;
    wire        m_axi_tma_rx_wlast_unused;
    always @(posedge clk_axi or negedge rstn_axi) begin
        if (!rstn_axi)            m_axi_tma_rx_bvalid_r <= 1'b0;
        else if (m_axi_tma_rx_wvalid) m_axi_tma_rx_bvalid_r <= 1'b1;
        else if (m_axi_tma_rx_bready) m_axi_tma_rx_bvalid_r <= 1'b0;
    end

    // ---- ch1 tma_tx (MM2S read) ----
    wire        m_axi_tma_tx_arvalid;
    wire        m_axi_tma_tx_rready;
    reg         m_axi_tma_tx_rvalid_r;
    wire [31:0] m_axi_tma_tx_araddr_unused;
    always @(posedge clk_axi or negedge rstn_axi) begin
        if (!rstn_axi)               m_axi_tma_tx_rvalid_r <= 1'b0;
        else if (m_axi_tma_tx_arvalid) m_axi_tma_tx_rvalid_r <= 1'b1;
        else if (m_axi_tma_tx_rready)  m_axi_tma_tx_rvalid_r <= 1'b0;
    end

    // ---- ch2 tmd_rx (S2MM write) ----
    wire        m_axi_tmd_rx_awvalid;
    wire        m_axi_tmd_rx_wvalid;
    wire        m_axi_tmd_rx_bready;
    reg         m_axi_tmd_rx_bvalid_r;
    wire [31:0] m_axi_tmd_rx_awaddr_unused;
    wire [31:0] m_axi_tmd_rx_wdata_unused;
    wire        m_axi_tmd_rx_wlast_unused;
    always @(posedge clk_axi or negedge rstn_axi) begin
        if (!rstn_axi)            m_axi_tmd_rx_bvalid_r <= 1'b0;
        else if (m_axi_tmd_rx_wvalid) m_axi_tmd_rx_bvalid_r <= 1'b1;
        else if (m_axi_tmd_rx_bready) m_axi_tmd_rx_bvalid_r <= 1'b0;
    end

    // ---- ch3 tmd_tx (MM2S read) ----
    wire        m_axi_tmd_tx_arvalid;
    wire        m_axi_tmd_tx_rready;
    reg         m_axi_tmd_tx_rvalid_r;
    wire [31:0] m_axi_tmd_tx_araddr_unused;
    always @(posedge clk_axi or negedge rstn_axi) begin
        if (!rstn_axi)               m_axi_tmd_tx_rvalid_r <= 1'b0;
        else if (m_axi_tmd_tx_arvalid) m_axi_tmd_tx_rvalid_r <= 1'b1;
        else if (m_axi_tmd_tx_rready)  m_axi_tmd_tx_rvalid_r <= 1'b0;
    end

    // -----------------------------------------------------------------
    // IRQ aggregator → onto pl_led0 (visible blink for any DMA-completion).
    // -----------------------------------------------------------------
    wire irq_tma_rx_o, irq_tma_tx_o, irq_tmd_rx_o, irq_tmd_tx_o;
    wire any_irq = irq_tma_rx_o | irq_tma_tx_o | irq_tmd_rx_o | irq_tmd_tx_o;

    // pl_led0/pl_led1 are also driven by tetra_top — but we override here.
    // We let tetra_top drive its own pl_led0/1 and absorb them; the
    // synth-top exposes a separately-derived pair. Easier: tie tetra_top's
    // pl_led nets to internal-only wires and drive the external ones from
    // the IRQ aggregator.
    wire tt_pl_led0, tt_pl_led1;
    assign pl_led0 = tt_pl_led0 | any_irq; // any_irq pulses are visible
    assign pl_led1 = tt_pl_led1;

    // -----------------------------------------------------------------
    // tetra_top instance — full DUT.
    // -----------------------------------------------------------------
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
        .pl_led0              (tt_pl_led0),
        .pl_led1              (tt_pl_led1),
        .dac_sync             (dac_sync),
        .dac_sclk             (dac_sclk),
        .dac_din              (dac_din),

        // AXI-Lite slave: idle (no master driving — all-zero stimulus,
        // ready signals can float).
        .s_axil_awaddr        (12'h0),
        .s_axil_awprot        (3'b0),
        .s_axil_awvalid       (1'b0),
        .s_axil_awready       (),
        .s_axil_wdata         (32'h0),
        .s_axil_wstrb         (4'h0),
        .s_axil_wvalid        (1'b0),
        .s_axil_wready        (),
        .s_axil_bresp         (),
        .s_axil_bvalid        (),
        .s_axil_bready        (1'b1),
        .s_axil_araddr        (12'h0),
        .s_axil_arprot        (3'b0),
        .s_axil_arvalid       (1'b0),
        .s_axil_arready       (),
        .s_axil_rdata         (),
        .s_axil_rresp         (),
        .s_axil_rvalid        (),
        .s_axil_rready        (1'b1),

        // 4× AXI-MM master — local stub-slave inside this wrapper.
        .m_axi_tma_rx_awaddr  (m_axi_tma_rx_awaddr_unused),
        .m_axi_tma_rx_awvalid (m_axi_tma_rx_awvalid),
        .m_axi_tma_rx_awready (1'b1),
        .m_axi_tma_rx_wdata   (m_axi_tma_rx_wdata_unused),
        .m_axi_tma_rx_wvalid  (m_axi_tma_rx_wvalid),
        .m_axi_tma_rx_wready  (1'b1),
        .m_axi_tma_rx_wlast   (m_axi_tma_rx_wlast_unused),
        .m_axi_tma_rx_bresp   (2'b00),
        .m_axi_tma_rx_bvalid  (m_axi_tma_rx_bvalid_r),
        .m_axi_tma_rx_bready  (m_axi_tma_rx_bready),

        .m_axi_tma_tx_araddr  (m_axi_tma_tx_araddr_unused),
        .m_axi_tma_tx_arvalid (m_axi_tma_tx_arvalid),
        .m_axi_tma_tx_arready (1'b1),
        .m_axi_tma_tx_rdata   (32'h0),
        .m_axi_tma_tx_rvalid  (m_axi_tma_tx_rvalid_r),
        .m_axi_tma_tx_rready  (m_axi_tma_tx_rready),
        .m_axi_tma_tx_rlast   (1'b1),
        .m_axi_tma_tx_rresp   (2'b00),

        .m_axi_tmd_rx_awaddr  (m_axi_tmd_rx_awaddr_unused),
        .m_axi_tmd_rx_awvalid (m_axi_tmd_rx_awvalid),
        .m_axi_tmd_rx_awready (1'b1),
        .m_axi_tmd_rx_wdata   (m_axi_tmd_rx_wdata_unused),
        .m_axi_tmd_rx_wvalid  (m_axi_tmd_rx_wvalid),
        .m_axi_tmd_rx_wready  (1'b1),
        .m_axi_tmd_rx_wlast   (m_axi_tmd_rx_wlast_unused),
        .m_axi_tmd_rx_bresp   (2'b00),
        .m_axi_tmd_rx_bvalid  (m_axi_tmd_rx_bvalid_r),
        .m_axi_tmd_rx_bready  (m_axi_tmd_rx_bready),

        .m_axi_tmd_tx_araddr  (m_axi_tmd_tx_araddr_unused),
        .m_axi_tmd_tx_arvalid (m_axi_tmd_tx_arvalid),
        .m_axi_tmd_tx_arready (1'b1),
        .m_axi_tmd_tx_rdata   (32'h0),
        .m_axi_tmd_tx_rvalid  (m_axi_tmd_tx_rvalid_r),
        .m_axi_tmd_tx_rready  (m_axi_tmd_tx_rready),
        .m_axi_tmd_tx_rlast   (1'b1),
        .m_axi_tmd_tx_rresp   (2'b00),

        // IRQs from PL.
        .irq_tma_rx_o (irq_tma_rx_o),
        .irq_tma_tx_o (irq_tma_tx_o),
        .irq_tmd_rx_o (irq_tmd_rx_o),
        .irq_tmd_tx_o (irq_tmd_tx_o)
    );

endmodule

`default_nettype wire
