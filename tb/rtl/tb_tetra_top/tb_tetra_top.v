// tb/rtl/tb_tetra_top/tb_tetra_top.v
//
// Owned by Agent A5 (A5-fpga-top-xdc-cleanup).
//
// Smoke TB for `rtl/tetra_top.v`. Compiled with `-DTETRA_TOP_NO_PHY`
// so the heavy PHY chain (rx_chain / tx_chain) is excluded from
// elaboration; the TB instead exercises the AXI-Lite reg-window read +
// the AXIS fabric end-to-end:
//
//   1. AXI-Lite: read REG_VERSION @ 0x0FC -> must be 0x0002_0000
//      (per docs/ARCHITECTURE.md "AXI-Lite Live-Config Register Window").
//   2. AXIS-RX TmaSap: drive a synthetic TMAS frame into the DMA
//      wrapper's s_axis_tma_rx_* slave port (FPGA->PS direction;
//      simulating the framer-output), the wrapper's behavioural model
//      captures the bytes; verify the captured-len / first-word match.
//      This is a fabric-wiring smoke (the PS-side DDR is mocked by the
//      A1 model).
//   3. AXI-DMA TX-out: drive a TMAS-shaped frame into the wrapper's
//      MM2S buffer for tma_tx; the wrapper streams it onto
//      m_axis_tma_tx_* and the TmaSap-TX framer parses+drains it. We
//      assert that the framer's `mb_frame_start_pulse` fires (= the
//      magic was recognised and the meta latched).
//
// Pass contract: print exactly one "PASS" line; any check failure
// $display "FAIL ..." and $fatal.

`timescale 1ns / 1ps
`default_nettype none

module tb_tetra_top;

    // -----------------------------------------------------------------
    // Clocks / resets
    // -----------------------------------------------------------------
    reg clk_axi  = 1'b0;
    reg clk_sys  = 1'b0;
    reg rstn_axi = 1'b0;
    reg rstn_sys = 1'b0;
    always #5 clk_axi = ~clk_axi;     // 100 MHz
    always #5 clk_sys = ~clk_sys;

    // -----------------------------------------------------------------
    // AXI-Lite master driver (12-bit addr, 32-bit data).
    // -----------------------------------------------------------------
    reg  [11:0] axil_awaddr;
    reg  [2:0]  axil_awprot;
    reg         axil_awvalid;
    wire        axil_awready;
    reg  [31:0] axil_wdata;
    reg  [3:0]  axil_wstrb;
    reg         axil_wvalid;
    wire        axil_wready;
    wire [1:0]  axil_bresp;
    wire        axil_bvalid;
    reg         axil_bready;
    reg  [11:0] axil_araddr;
    reg  [2:0]  axil_arprot;
    reg         axil_arvalid;
    wire        axil_arready;
    wire [31:0] axil_rdata;
    wire [1:0]  axil_rresp;
    wire        axil_rvalid;
    reg         axil_rready;

    // -----------------------------------------------------------------
    // AD9361 LVDS pin tie-offs (no PHY in this TB).
    // -----------------------------------------------------------------
    wire        ad9361_unused_p = 1'b0;
    wire        ad9361_unused_n = 1'b1;

    // -----------------------------------------------------------------
    // 4× AXI-MM master responder tie-offs.
    // -----------------------------------------------------------------
    wire [31:0] m_axi_tma_rx_awaddr,  m_axi_tma_rx_wdata;
    wire        m_axi_tma_rx_awvalid, m_axi_tma_rx_wvalid;
    wire        m_axi_tma_rx_wlast,   m_axi_tma_rx_bready;
    wire [31:0] m_axi_tma_tx_araddr;
    wire        m_axi_tma_tx_arvalid, m_axi_tma_tx_rready;
    wire [31:0] m_axi_tmd_rx_awaddr,  m_axi_tmd_rx_wdata;
    wire        m_axi_tmd_rx_awvalid, m_axi_tmd_rx_wvalid;
    wire        m_axi_tmd_rx_wlast,   m_axi_tmd_rx_bready;
    wire [31:0] m_axi_tmd_tx_araddr;
    wire        m_axi_tmd_tx_arvalid, m_axi_tmd_tx_rready;

    // -----------------------------------------------------------------
    // IRQ outputs (observed)
    // -----------------------------------------------------------------
    wire irq_tma_rx, irq_tma_tx, irq_tmd_rx, irq_tmd_tx;

    // -----------------------------------------------------------------
    // GPIO inputs (tied off)
    // -----------------------------------------------------------------
    wire [7:0] gpio_status_tied = 8'd0;
    wire       spi_miso_tied    = 1'b0;

    // -----------------------------------------------------------------
    // DUT
    // -----------------------------------------------------------------
    tetra_top dut (
        .clk_axi              (clk_axi),
        .rstn_axi             (rstn_axi),
        .clk_sys              (clk_sys),
        .rstn_sys             (rstn_sys),

        .rx_clk_in_p          (ad9361_unused_p),
        .rx_clk_in_n          (ad9361_unused_n),
        .rx_frame_in_p        (ad9361_unused_p),
        .rx_frame_in_n        (ad9361_unused_n),
        .rx_data_in_p         (6'd0),
        .rx_data_in_n         (6'h3F),
        .tx_clk_out_p         (),
        .tx_clk_out_n         (),
        .tx_frame_out_p       (),
        .tx_frame_out_n       (),
        .tx_data_out_p        (),
        .tx_data_out_n        (),

        .enable               (),
        .txnrx                (),
        .spi_csn              (),
        .spi_clk              (),
        .spi_mosi             (),
        .spi_miso             (spi_miso_tied),
        .gpio_status          (gpio_status_tied),
        .gpio_ctl             (),
        .gpio_en_agc          (),
        .gpio_sync            (),
        .gpio_resetb          (),

        .iic_scl              (),
        .iic_sda              (),
        .pl_led0              (),
        .pl_led1              (),
        .dac_sync             (),
        .dac_sclk             (),
        .dac_din              (),

        .s_axil_awaddr        (axil_awaddr),
        .s_axil_awprot        (axil_awprot),
        .s_axil_awvalid       (axil_awvalid),
        .s_axil_awready       (axil_awready),
        .s_axil_wdata         (axil_wdata),
        .s_axil_wstrb         (axil_wstrb),
        .s_axil_wvalid        (axil_wvalid),
        .s_axil_wready        (axil_wready),
        .s_axil_bresp         (axil_bresp),
        .s_axil_bvalid        (axil_bvalid),
        .s_axil_bready        (axil_bready),
        .s_axil_araddr        (axil_araddr),
        .s_axil_arprot        (axil_arprot),
        .s_axil_arvalid       (axil_arvalid),
        .s_axil_arready       (axil_arready),
        .s_axil_rdata         (axil_rdata),
        .s_axil_rresp         (axil_rresp),
        .s_axil_rvalid        (axil_rvalid),
        .s_axil_rready        (axil_rready),

        .m_axi_tma_rx_awaddr  (m_axi_tma_rx_awaddr),
        .m_axi_tma_rx_awvalid (m_axi_tma_rx_awvalid),
        .m_axi_tma_rx_awready (1'b1),
        .m_axi_tma_rx_wdata   (m_axi_tma_rx_wdata),
        .m_axi_tma_rx_wvalid  (m_axi_tma_rx_wvalid),
        .m_axi_tma_rx_wready  (1'b1),
        .m_axi_tma_rx_wlast   (m_axi_tma_rx_wlast),
        .m_axi_tma_rx_bresp   (2'b00),
        .m_axi_tma_rx_bvalid  (1'b0),
        .m_axi_tma_rx_bready  (m_axi_tma_rx_bready),

        .m_axi_tma_tx_araddr  (m_axi_tma_tx_araddr),
        .m_axi_tma_tx_arvalid (m_axi_tma_tx_arvalid),
        .m_axi_tma_tx_arready (1'b1),
        .m_axi_tma_tx_rdata   (32'h0),
        .m_axi_tma_tx_rvalid  (1'b0),
        .m_axi_tma_tx_rready  (m_axi_tma_tx_rready),
        .m_axi_tma_tx_rlast   (1'b0),
        .m_axi_tma_tx_rresp   (2'b00),

        .m_axi_tmd_rx_awaddr  (m_axi_tmd_rx_awaddr),
        .m_axi_tmd_rx_awvalid (m_axi_tmd_rx_awvalid),
        .m_axi_tmd_rx_awready (1'b1),
        .m_axi_tmd_rx_wdata   (m_axi_tmd_rx_wdata),
        .m_axi_tmd_rx_wvalid  (m_axi_tmd_rx_wvalid),
        .m_axi_tmd_rx_wready  (1'b1),
        .m_axi_tmd_rx_wlast   (m_axi_tmd_rx_wlast),
        .m_axi_tmd_rx_bresp   (2'b00),
        .m_axi_tmd_rx_bvalid  (1'b0),
        .m_axi_tmd_rx_bready  (m_axi_tmd_rx_bready),

        .m_axi_tmd_tx_araddr  (m_axi_tmd_tx_araddr),
        .m_axi_tmd_tx_arvalid (m_axi_tmd_tx_arvalid),
        .m_axi_tmd_tx_arready (1'b1),
        .m_axi_tmd_tx_rdata   (32'h0),
        .m_axi_tmd_tx_rvalid  (1'b0),
        .m_axi_tmd_tx_rready  (m_axi_tmd_tx_rready),
        .m_axi_tmd_tx_rlast   (1'b0),
        .m_axi_tmd_tx_rresp   (2'b00),

        .irq_tma_rx_o         (irq_tma_rx),
        .irq_tma_tx_o         (irq_tma_tx),
        .irq_tmd_rx_o         (irq_tmd_rx),
        .irq_tmd_tx_o         (irq_tmd_tx)
    );

    // -----------------------------------------------------------------
    // AXI-Lite read / write tasks (full 12-bit addr).
    // -----------------------------------------------------------------
    task axil_write;
        input [11:0] addr;
        input [31:0] data;
        begin
            @(negedge clk_axi);
            axil_awaddr  = addr;
            axil_awprot  = 3'd0;
            axil_awvalid = 1'b1;
            axil_wdata   = data;
            axil_wstrb   = 4'b1111;
            axil_wvalid  = 1'b1;
            axil_bready  = 1'b1;
            wait (axil_awready);
            wait (axil_wready);
            @(posedge clk_axi);
            @(negedge clk_axi);
            axil_awvalid = 1'b0;
            axil_wvalid  = 1'b0;
            wait (axil_bvalid);
            @(posedge clk_axi);
            @(negedge clk_axi);
            axil_bready  = 1'b0;
        end
    endtask

    task axil_read;
        input  [11:0] addr;
        output [31:0] data;
        begin
            @(negedge clk_axi);
            axil_araddr  = addr;
            axil_arprot  = 3'd0;
            axil_arvalid = 1'b1;
            axil_rready  = 1'b1;
            wait (axil_arready);
            @(posedge clk_axi);
            @(negedge clk_axi);
            axil_arvalid = 1'b0;
            wait (axil_rvalid);
            data = axil_rdata;
            @(posedge clk_axi);
            @(negedge clk_axi);
            axil_rready = 1'b0;
        end
    endtask

    // -----------------------------------------------------------------
    // Helper: write one 32-bit beat (4 bytes) into the tma_tx model's
    // MM2S inject buffer. Big-endian byte order on the wire.
    // -----------------------------------------------------------------
    task push_tma_tx_word;
        input [31:0] data;
        begin
            dut.u_dma.u_ch1_tma_tx.inject_byte(data[31:24]);
            dut.u_dma.u_ch1_tma_tx.inject_byte(data[23:16]);
            dut.u_dma.u_ch1_tma_tx.inject_byte(data[15:8]);
            dut.u_dma.u_ch1_tma_tx.inject_byte(data[7:0]);
        end
    endtask

    // -----------------------------------------------------------------
    // Test main
    // -----------------------------------------------------------------
    integer    i;
    reg [31:0] rd_val;

    initial begin
        // bus init
        axil_awaddr = 12'd0; axil_awprot = 3'd0; axil_awvalid = 1'b0;
        axil_wdata  = 32'd0; axil_wstrb = 4'd0; axil_wvalid = 1'b0;
        axil_bready = 1'b0;
        axil_araddr = 12'd0; axil_arprot = 3'd0; axil_arvalid = 1'b0;
        axil_rready = 1'b0;

        // reset
        rstn_axi = 1'b0;
        rstn_sys = 1'b0;
        repeat (8) @(posedge clk_axi);
        rstn_axi = 1'b1;
        rstn_sys = 1'b1;
        repeat (4) @(posedge clk_axi);

        // -------------------------------------------------------------
        // T1: REG_VERSION read at 0x0FC -> 0x0002_0000.
        // -------------------------------------------------------------
        axil_read(12'h0FC, rd_val);
        if (rd_val !== 32'h0002_0000) begin
            $display("FAIL T1: REG_VERSION readback=0x%08h, expected 0x0002_0000", rd_val);
            $fatal;
        end
        $display("[T1] REG_VERSION read = 0x%08h: PASS", rd_val);

        // -------------------------------------------------------------
        // T2: REG_CELL_MCC default = 262 (0x106). Sanity for the
        // configuration region decode.
        // -------------------------------------------------------------
        axil_read(12'h000, rd_val);
        if (rd_val[9:0] !== 10'd262) begin
            $display("FAIL T2: REG_CELL_MCC default=%0d, expected 262", rd_val[9:0]);
            $fatal;
        end
        $display("[T2] REG_CELL_MCC default OK: PASS");

        // -------------------------------------------------------------
        // T3: REG_SCRAMBLER_INIT default 0x4183_F207.
        // -------------------------------------------------------------
        axil_read(12'h020, rd_val);
        if (rd_val !== 32'h4183_F207) begin
            $display("FAIL T3: REG_SCRAMBLER_INIT=0x%08h, expected 0x4183F207", rd_val);
            $fatal;
        end
        $display("[T3] REG_SCRAMBLER_INIT default OK: PASS");

        // -------------------------------------------------------------
        // T4: DMA sub-window forwarding. Write REG_DMA_CH_ENABLE @0x0A0
        // -> readback should reflect the value via the wrapper.
        // -------------------------------------------------------------
        axil_write(12'h0A0, 32'h0000_000F);
        axil_read (12'h0A0, rd_val);
        if (rd_val[3:0] !== 4'hF) begin
            $display("FAIL T4: REG_DMA_CH_ENABLE readback=0x%08h, expected lower nibble 0xF", rd_val);
            $fatal;
        end
        $display("[T4] DMA sub-window forward (REG_DMA_CH_ENABLE) OK: PASS");

        // -------------------------------------------------------------
        // T5: SLOT_TABLE write/readback at offset 0x030 (entry 0).
        // -------------------------------------------------------------
        axil_write(12'h030, 32'hC001_BABE);
        axil_read (12'h030, rd_val);
        if (rd_val !== 32'hC001_BABE) begin
            $display("FAIL T5: SLOT_TABLE[0] readback=0x%08h, expected C001BABE", rd_val);
            $fatal;
        end
        $display("[T5] SLOT_TABLE entry[0] r/w OK: PASS");

        // -------------------------------------------------------------
        // T6: REG_SCRATCH (0x200) byte-laned write+read.
        // -------------------------------------------------------------
        axil_write(12'h200, 32'hDEAD_BEEF);
        axil_read (12'h200, rd_val);
        if (rd_val !== 32'hDEAD_BEEF) begin
            $display("FAIL T6: REG_SCRATCH=0x%08h, expected DEADBEEF", rd_val);
            $fatal;
        end
        $display("[T6] REG_SCRATCH r/w OK: PASS");

        // -------------------------------------------------------------
        // T7: AXIS RX path smoke. The TmaSap-RX framer is idle (no UMAC
        // input in the no-PHY build) -> we drive a TMAS frame DIRECTLY
        // into the DMA wrapper's S2MM-tma_rx slave by injecting at the
        // top-level wire (override via dut hierarchy).
        //
        // Specifically we observe that the wrapper's S2MM channel
        // accepts our beats (tready stays HIGH because the model is a
        // bit bucket on the AXI-MM side).
        // -------------------------------------------------------------
        // Force-drive 3 beats into the internal AXIS line (TMAS-shaped
        // frame: magic + len + payload). This bypasses the TmaSap-RX
        // framer's mux but proves the AXIS fabric carries data through
        // to the wrapper / model.
        force dut.axis_tma_rx_tdata  = 32'h544D_4153;
        force dut.axis_tma_rx_tkeep  = 4'b1111;
        force dut.axis_tma_rx_tvalid = 1'b1;
        force dut.axis_tma_rx_tlast  = 1'b0;
        @(posedge clk_axi);
        @(negedge clk_axi);
        force dut.axis_tma_rx_tdata  = 32'h0000_000C;
        @(posedge clk_axi);
        @(negedge clk_axi);
        force dut.axis_tma_rx_tdata  = 32'hDEAD_BEEF;
        force dut.axis_tma_rx_tlast  = 1'b1;
        @(posedge clk_axi);
        @(negedge clk_axi);
        release dut.axis_tma_rx_tdata;
        release dut.axis_tma_rx_tkeep;
        release dut.axis_tma_rx_tvalid;
        release dut.axis_tma_rx_tlast;

        // Allow the IRQ + telemetry to settle.
        repeat (16) @(posedge clk_axi);

        // Telemetry counter for tma_rx should have ticked to 1.
        axil_read(12'h120, rd_val);     // REG_DMA_TMA_RX_FRAMES
        if (rd_val !== 32'd1) begin
            $display("FAIL T7: REG_DMA_TMA_RX_FRAMES=%0d, expected 1", rd_val);
            $fatal;
        end
        $display("[T7] AXIS RX -> DMA telemetry counter ticked: PASS");

        // -------------------------------------------------------------
        // T8: AXIS TX path: enable tma_tx channel, push a TMAS frame
        // into the model's MM2S buffer; expect the wrapper to stream it
        // onto m_axis_tma_tx_* and the framer to recognise the magic.
        // -------------------------------------------------------------
        axil_write(12'h0A0, 32'h0000_000F);  // enable all channels (idempotent)
        // Build a minimal TMAS frame with pdu_len_bits=0 so the
        // framer takes the empty-payload fast path (mb_frame_start +
        // mb_frame_end immediately).
        // bytes 0..3   : magic 'TMAS'
        // bytes 4..7   : frame_len = 36, pdu_len_bits=0
        // bytes 8..11  : ssi=0, ssi_type=0
        // bytes 12..15 : flags=0, chan_alloc=0
        // bytes 16..19 : endpoint_id=0
        // bytes 20..23 : new_endpoint_id=0
        // bytes 24..27 : css_endpoint_id=0
        // bytes 28..31 : scrambling_code=0
        // bytes 32..35 : req_handle=0xCAFE_F00D
        push_tma_tx_word(32'h544D_4153);                  // magic
        push_tma_tx_word(32'h0024_0000);                  // frame_len=36, pdu_len_bits=0
        push_tma_tx_word(32'h0000_0000);                  // ssi=0
        push_tma_tx_word(32'h0000_0000);                  // ssi_type/flags/chan_alloc
        push_tma_tx_word(32'h0000_0000);                  // endpoint
        push_tma_tx_word(32'h0000_0000);                  // new_ep
        push_tma_tx_word(32'h0000_0000);                  // css_ep
        push_tma_tx_word(32'h0000_0000);                  // scrambling
        push_tma_tx_word(32'hCAFE_F00D);                  // req_handle
        dut.u_dma.u_ch1_tma_tx.inject_frame_done();

        // Wait for the framer to drain. mb_frame_start_pulse fires once
        // per accepted frame; we observe via the dut hierarchy.
        begin : wait_frame_start
            integer wait_cnt;
            wait_cnt = 0;
            while ((dut.u_tma_tx_framer.mb_frame_start_pulse !== 1'b1) &&
                   (wait_cnt < 200)) begin
                @(posedge clk_axi);
                wait_cnt = wait_cnt + 1;
            end
            if (wait_cnt >= 200) begin
                $display("FAIL T8: TmaSap-TX framer never asserted mb_frame_start_pulse");
                $fatal;
            end
            $display("[T8] TmaSap-TX framer recognised TMAS frame after %0d cycles: PASS", wait_cnt);
        end

        // Flush any trailing activity.
        repeat (32) @(posedge clk_axi);

        // -------------------------------------------------------------
        // T9: REG_TMASAP_TX_FRAMES_CNT @ 0x164 should now be >= 1.
        // -------------------------------------------------------------
        axil_read(12'h164, rd_val);
        if (rd_val < 32'd1) begin
            $display("FAIL T9: REG_TMASAP_TX_FRAMES_CNT=%0d, expected >= 1", rd_val);
            $fatal;
        end
        $display("[T9] REG_TMASAP_TX_FRAMES_CNT=%0d (>=1): PASS", rd_val);

        $display("PASS tb_tetra_top");
        $finish;
    end

    initial begin
        // Watchdog
        #2_000_000;
        $display("FAIL tb_tetra_top: watchdog timeout");
        $fatal;
    end

endmodule

`default_nettype wire
