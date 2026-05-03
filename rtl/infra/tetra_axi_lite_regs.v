// rtl/infra/tetra_axi_lite_regs.v
//
// Owned by Agent A5 (A5-fpga-top-xdc-cleanup).
//
// AXI4-Lite Live-Config Register Window per
// docs/ARCHITECTURE.md §"AXI-Lite Live-Config Register Window".
//
// Single AXI4-Lite slave at PS-side base address 0x4000_0000 (set in DT;
// the slave only sees address bits [11:0]). Address-decode regions:
//
//   0x000..0x0FF  Configuration  (R/W mostly)
//   0x100..0x1FF  Telemetry      (R/O; REG_IRQ_STATUS@0x10C is R/W1C)
//   0x200..0x2FF  Test/Scratch   (R/W; production-gated, see ifdefs)
//   0x300+        Reserved       (read 0 / write drop)
//
// SAFE-DEFAULT decode: undecoded reads return 0; undecoded writes drop
// silently. No protection-level check; awprot/arprot ignored.
// Misaligned access returns SLVERR (low 2 bits of address must be 0).
//
// IF_TETRA_TOP_v1 register-window contract (locked):
//   - All offsets and bit-fields match docs/ARCHITECTURE.md §AXI-Lite.
//   - REG_VERSION reset = 0x0002_0000 (carry-over was 0x0001_0000).
//   - Forbidden carry-over registers (REG_SHADOW_*, REG_PROFILE_*,
//     REG_DB_POLICY, REG_AACH_GRANT_HINT) are NOT decoded — their
//     hypothetical addresses fall in the reserved bands and read 0.
//
// This module aggregates telemetry inputs from PHY + LMAC + UMAC + the
// 4x DMA wrapper (Agent A1) + TmaSap framers (Agent A2) + TmdSap
// framers (Agent A3). Counters are saturating 32-bit (16-bit where the
// spec says so) and reset to 0 on RST_CNTRS pulse from REG_CTRL[3].
//
// SLOT_TABLE (0x030..0x07F) is a 20x32 window written from this module
// directly into a flat register array (no separate BRAM module — the
// table is small enough that distributed regs are simpler at this
// scale and the spec's note about RAM32M-style mapping is a synth
// hint, not a behavioural requirement).
//
// Verilog-2001 only (CLAUDE.md §Languages).

`timescale 1ns / 1ps
`default_nettype none

module tetra_axi_lite_regs #(
    parameter ADDR_WIDTH = 12,
    parameter DATA_WIDTH = 32
) (
    // ---- AXI-Lite clock + reset (PS-side) ---------------------------
    input  wire                          clk_axi,
    input  wire                          rstn_axi,

    // ---- AXI4-Lite slave port ---------------------------------------
    input  wire [ADDR_WIDTH-1:0]         s_axil_awaddr,
    input  wire [2:0]                    s_axil_awprot,
    input  wire                          s_axil_awvalid,
    output wire                          s_axil_awready,
    input  wire [DATA_WIDTH-1:0]         s_axil_wdata,
    input  wire [DATA_WIDTH/8-1:0]       s_axil_wstrb,
    input  wire                          s_axil_wvalid,
    output wire                          s_axil_wready,
    output wire [1:0]                    s_axil_bresp,
    output wire                          s_axil_bvalid,
    input  wire                          s_axil_bready,
    input  wire [ADDR_WIDTH-1:0]         s_axil_araddr,
    input  wire [2:0]                    s_axil_arprot,
    input  wire                          s_axil_arvalid,
    output wire                          s_axil_arready,
    output wire [DATA_WIDTH-1:0]         s_axil_rdata,
    output wire [1:0]                    s_axil_rresp,
    output wire                          s_axil_rvalid,
    input  wire                          s_axil_rready,

    // ---- Configuration outputs (to PHY/LMAC/UMAC consumers) ----------
    output wire [9:0]                    cfg_cell_mcc,
    output wire [13:0]                   cfg_cell_mnc,
    output wire [5:0]                    cfg_cell_cc,
    output wire [13:0]                   cfg_cell_la,
    output wire [31:0]                   cfg_rx_carrier_hz,
    output wire [31:0]                   cfg_tx_carrier_hz,
    output wire [7:0]                    cfg_tx_gain_trim,
    output wire [1:0]                    cfg_cipher_mode,
    output wire [31:0]                   cfg_scrambler_init,
    output wire [11:0]                   cfg_ts_n,
    output wire [11:0]                   cfg_ts_p,
    output wire [11:0]                   cfg_ts_q,
    output wire                          cfg_ctrl_rx_en,
    output wire                          cfg_ctrl_tx_en,
    output wire                          cfg_ctrl_loopback,
    output wire                          cfg_ctrl_rst_cntrs_pulse,
    output wire [7:0]                    cfg_sync_thresh,
    output wire [6:0]                    cfg_rx_gain,
    output wire [7:0]                    cfg_tx_att,
    output wire [4:0]                    cfg_irq_enable,

    // ---- 20-entry SLOT_TABLE (flat 20x32) ----------------------------
    // Read-only port for tetra_dl_signal_scheduler.
    output wire [31:0]                   slot_table_word0,
    output wire [31:0]                   slot_table_word1,
    output wire [31:0]                   slot_table_word2,
    output wire [31:0]                   slot_table_word3,
    output wire [31:0]                   slot_table_word4,
    output wire [31:0]                   slot_table_word5,
    output wire [31:0]                   slot_table_word6,
    output wire [31:0]                   slot_table_word7,
    output wire [31:0]                   slot_table_word8,
    output wire [31:0]                   slot_table_word9,
    output wire [31:0]                   slot_table_word10,
    output wire [31:0]                   slot_table_word11,
    output wire [31:0]                   slot_table_word12,
    output wire [31:0]                   slot_table_word13,
    output wire [31:0]                   slot_table_word14,
    output wire [31:0]                   slot_table_word15,
    output wire [31:0]                   slot_table_word16,
    output wire [31:0]                   slot_table_word17,
    output wire [31:0]                   slot_table_word18,
    output wire [31:0]                   slot_table_word19,

    // ---- DMA-wrapper sub-window forward (0x0A0..0x0AF) ---------------
    // The 4-register DMA sub-window is owned by Agent A1's wrapper but
    // its address range is mapped through this top-level decoder to
    // keep a single AXI-Lite slave on the PS side. Top-level wires
    // these to the wrapper's AXI-Lite slave port.
    output wire [3:0]                    dma_subwin_awaddr,
    output wire                          dma_subwin_awvalid,
    input  wire                          dma_subwin_awready,
    output wire [31:0]                   dma_subwin_wdata,
    output wire [3:0]                    dma_subwin_wstrb,
    output wire                          dma_subwin_wvalid,
    input  wire                          dma_subwin_wready,
    input  wire [1:0]                    dma_subwin_bresp,
    input  wire                          dma_subwin_bvalid,
    output wire                          dma_subwin_bready,
    output wire [3:0]                    dma_subwin_araddr,
    output wire                          dma_subwin_arvalid,
    input  wire                          dma_subwin_arready,
    input  wire [31:0]                   dma_subwin_rdata,
    input  wire [1:0]                    dma_subwin_rresp,
    input  wire                          dma_subwin_rvalid,
    output wire                          dma_subwin_rready,

    // ---- Telemetry inputs (R/O at 0x100..0x17C) ----------------------
    input  wire [7:0]                    tlm_phy_status,
    input  wire [4:0]                    tlm_frame_num,
    input  wire [1:0]                    tlm_slot_num,
    // R/W1C: per-bit set strobes from hardware aggregator
    input  wire [4:0]                    tlm_irq_set_pulse,
    output wire [4:0]                    irq_status_o,
    input  wire [15:0]                   tlm_dma_blk_cnt,
    input  wire [15:0]                   tlm_crc_err_cnt,
    input  wire [15:0]                   tlm_sync_lst_cnt,
    input  wire [31:0]                   tlm_frame_tick_cnt,
    input  wire [31:0]                   tlm_dma_tma_rx_frames,
    input  wire [31:0]                   tlm_dma_tma_tx_frames,
    input  wire [31:0]                   tlm_dma_tmd_rx_frames,
    input  wire [31:0]                   tlm_dma_tmd_tx_frames,
    input  wire [31:0]                   tlm_dma_irq_cnt_rx,
    input  wire [31:0]                   tlm_dma_irq_cnt_tx,
    input  wire [15:0]                   tlm_dma_overrun_cnt,
    input  wire [15:0]                   tlm_dma_underrun_cnt,
    input  wire [15:0]                   tlm_aach_last_raw,
    input  wire [31:0]                   tlm_aach_transition_cnt,
    input  wire [31:0]                   tlm_aach_idle_cnt,
    input  wire [31:0]                   tlm_aach_sig_active_cnt,
    input  wire [31:0]                   tlm_aach_traffic_cnt,
    input  wire [7:0]                    tlm_umac_dlq_depth,
    input  wire [15:0]                   tlm_umac_dlq_drops,
    input  wire [15:0]                   tlm_umac_reasm_fail_cnt,
    input  wire [31:0]                   tlm_tmasap_rx_frames_cnt,
    input  wire [31:0]                   tlm_tmasap_tx_frames_cnt,
    input  wire [15:0]                   tlm_tmasap_tx_err_cnt,
    input  wire [31:0]                   tlm_tmar_frames_cnt,
    input  wire [31:0]                   tlm_tmdsap_tx_frames_cnt,
    input  wire [31:0]                   tlm_tmdsap_rx_frames_cnt,
    input  wire [31:0]                   tlm_tmdsap_err_cnt
);

    // -----------------------------------------------------------------
    // Locked constants (R1: bit-layouts per docs/ARCHITECTURE.md verbatim).
    // -----------------------------------------------------------------
    localparam [31:0] VERSION_DEFAULT       = 32'h0002_0000;

    // Reset defaults (Gold-Cell from reference_gold_full_attach_timeline.md).
    localparam [9:0]  RST_CELL_MCC          = 10'd262;
    localparam [13:0] RST_CELL_MNC          = 14'd1010;
    localparam [5:0]  RST_CELL_CC           = 6'd1;
    localparam [13:0] RST_CELL_LA           = 14'd0;
    localparam [31:0] RST_RX_CARRIER_HZ     = 32'd392_987_500;
    localparam [31:0] RST_TX_CARRIER_HZ     = 32'd382_891_062;
    localparam [7:0]  RST_TX_GAIN_TRIM      = 8'd0;
    localparam [1:0]  RST_CIPHER_MODE       = 2'd0;
    localparam [31:0] RST_SCRAMBLER_INIT    = 32'h4183_F207;
    localparam [11:0] RST_TS_N              = 12'hCB2;
    localparam [11:0] RST_TS_P              = 12'h536;
    localparam [11:0] RST_TS_Q              = 12'h0E2;
    localparam [7:0]  RST_SYNC_THRESH       = 8'hC8;
    localparam [6:0]  RST_RX_GAIN           = 7'h20;
    localparam [7:0]  RST_TX_ATT            = 8'h28;

    // -----------------------------------------------------------------
    // Configuration registers (R/W).
    // -----------------------------------------------------------------
    reg [9:0]  reg_cell_mcc;
    reg [13:0] reg_cell_mnc;
    reg [5:0]  reg_cell_cc;
    reg [13:0] reg_cell_la;
    reg [31:0] reg_rx_carrier_hz;
    reg [31:0] reg_tx_carrier_hz;
    reg [7:0]  reg_tx_gain_trim;
    reg [1:0]  reg_cipher_mode;
    reg [31:0] reg_scrambler_init;
    reg [11:0] reg_ts_n;
    reg [11:0] reg_ts_p;
    reg [11:0] reg_ts_q;
    reg [3:0]  reg_ctrl;            // [0]RX_EN [1]TX_EN [2]LOOPBACK [3]RST_CNTRS-pulse
    reg        ctrl_rst_cntrs_pulse;
    reg [7:0]  reg_sync_thresh;
    reg [6:0]  reg_rx_gain;
    reg [7:0]  reg_tx_att;
    reg [4:0]  reg_irq_enable;

    // 20-entry slot table (offsets 0x030..0x07F = 5 frames * 4 slots * 4B).
    reg [31:0] reg_slot_table [0:19];

    // R/W1C IRQ status. tlm_irq_set_pulse pulses HIGH for 1 cycle;
    // SW-W1C clears the bit on a write of 1; HW-set wins on collision.
    reg [4:0]  reg_irq_status;

    // Test/scratch.
    reg [31:0] reg_scratch;

    integer i;

    // -----------------------------------------------------------------
    // AXI-Lite slave FSM (carry-over R2..R5: simple, no outstanding
    // transactions, single beat at a time).
    // -----------------------------------------------------------------
    reg        awready_r, wready_r, bvalid_r;
    reg [1:0]  bresp_r;
    reg        arready_r, rvalid_r;
    reg [1:0]  rresp_r;
    reg [31:0] rdata_r;

    reg        wr_addr_seen, wr_data_seen;
    reg [ADDR_WIDTH-1:0] wr_addr_q;
    reg [31:0] wr_data_q;
    reg [3:0]  wr_strb_q;
    reg [ADDR_WIDTH-1:0] rd_addr_q;
    reg        rd_addr_seen;

    // DMA sub-window forwarding bookkeeping.
    reg        wr_to_dma, rd_to_dma;
    reg        dma_aw_pending, dma_w_pending, dma_b_pending;
    reg        dma_ar_pending, dma_r_pending;

    assign s_axil_awready = awready_r;
    assign s_axil_wready  = wready_r;
    assign s_axil_bvalid  = bvalid_r;
    assign s_axil_bresp   = bresp_r;
    assign s_axil_arready = arready_r;
    assign s_axil_rvalid  = rvalid_r;
    assign s_axil_rresp   = rresp_r;
    assign s_axil_rdata   = rdata_r;

    // DMA sub-window passthrough (combinational).
    assign dma_subwin_awaddr  = wr_addr_q[3:0];
    assign dma_subwin_awvalid = dma_aw_pending;
    assign dma_subwin_wdata   = wr_data_q;
    assign dma_subwin_wstrb   = wr_strb_q;
    assign dma_subwin_wvalid  = dma_w_pending;
    assign dma_subwin_bready  = dma_b_pending;
    assign dma_subwin_araddr  = rd_addr_q[3:0];
    assign dma_subwin_arvalid = dma_ar_pending;
    assign dma_subwin_rready  = dma_r_pending;

    // -----------------------------------------------------------------
    // Configuration drivers.
    // -----------------------------------------------------------------
    assign cfg_cell_mcc        = reg_cell_mcc;
    assign cfg_cell_mnc        = reg_cell_mnc;
    assign cfg_cell_cc         = reg_cell_cc;
    assign cfg_cell_la         = reg_cell_la;
    assign cfg_rx_carrier_hz   = reg_rx_carrier_hz;
    assign cfg_tx_carrier_hz   = reg_tx_carrier_hz;
    assign cfg_tx_gain_trim    = reg_tx_gain_trim;
    assign cfg_cipher_mode     = reg_cipher_mode;
    assign cfg_scrambler_init  = reg_scrambler_init;
    assign cfg_ts_n            = reg_ts_n;
    assign cfg_ts_p            = reg_ts_p;
    assign cfg_ts_q            = reg_ts_q;
    assign cfg_ctrl_rx_en      = reg_ctrl[0];
    assign cfg_ctrl_tx_en      = reg_ctrl[1];
    assign cfg_ctrl_loopback   = reg_ctrl[2];
    assign cfg_ctrl_rst_cntrs_pulse = ctrl_rst_cntrs_pulse;
    assign cfg_sync_thresh     = reg_sync_thresh;
    assign cfg_rx_gain         = reg_rx_gain;
    assign cfg_tx_att          = reg_tx_att;
    assign cfg_irq_enable      = reg_irq_enable;

    assign slot_table_word0  = reg_slot_table[0];
    assign slot_table_word1  = reg_slot_table[1];
    assign slot_table_word2  = reg_slot_table[2];
    assign slot_table_word3  = reg_slot_table[3];
    assign slot_table_word4  = reg_slot_table[4];
    assign slot_table_word5  = reg_slot_table[5];
    assign slot_table_word6  = reg_slot_table[6];
    assign slot_table_word7  = reg_slot_table[7];
    assign slot_table_word8  = reg_slot_table[8];
    assign slot_table_word9  = reg_slot_table[9];
    assign slot_table_word10 = reg_slot_table[10];
    assign slot_table_word11 = reg_slot_table[11];
    assign slot_table_word12 = reg_slot_table[12];
    assign slot_table_word13 = reg_slot_table[13];
    assign slot_table_word14 = reg_slot_table[14];
    assign slot_table_word15 = reg_slot_table[15];
    assign slot_table_word16 = reg_slot_table[16];
    assign slot_table_word17 = reg_slot_table[17];
    assign slot_table_word18 = reg_slot_table[18];
    assign slot_table_word19 = reg_slot_table[19];

    assign irq_status_o = reg_irq_status;

    // -----------------------------------------------------------------
    // Address-range helpers.
    // -----------------------------------------------------------------
    // DMA sub-window: 0x0A0..0x0AF (4 regs).
    function automatic is_dma_subwin;
        input [ADDR_WIDTH-1:0] addr;
        begin
            is_dma_subwin = (addr[ADDR_WIDTH-1:4] == {{(ADDR_WIDTH-12){1'b0}}, 8'h0A});
        end
    endfunction

    // Misalignment check: low 2 bits must be 0.
    function automatic is_misaligned;
        input [ADDR_WIDTH-1:0] addr;
        begin
            is_misaligned = |addr[1:0];
        end
    endfunction

    // -----------------------------------------------------------------
    // Main FSM (synchronous; reset-aware).
    // -----------------------------------------------------------------
    always @(posedge clk_axi or negedge rstn_axi) begin
        if (!rstn_axi) begin
            // Reset config defaults.
            reg_cell_mcc        <= RST_CELL_MCC;
            reg_cell_mnc        <= RST_CELL_MNC;
            reg_cell_cc         <= RST_CELL_CC;
            reg_cell_la         <= RST_CELL_LA;
            reg_rx_carrier_hz   <= RST_RX_CARRIER_HZ;
            reg_tx_carrier_hz   <= RST_TX_CARRIER_HZ;
            reg_tx_gain_trim    <= RST_TX_GAIN_TRIM;
            reg_cipher_mode     <= RST_CIPHER_MODE;
            reg_scrambler_init  <= RST_SCRAMBLER_INIT;
            reg_ts_n            <= RST_TS_N;
            reg_ts_p            <= RST_TS_P;
            reg_ts_q            <= RST_TS_Q;
            reg_ctrl            <= 4'd0;
            ctrl_rst_cntrs_pulse<= 1'b0;
            reg_sync_thresh     <= RST_SYNC_THRESH;
            reg_rx_gain         <= RST_RX_GAIN;
            reg_tx_att          <= RST_TX_ATT;
            reg_irq_enable      <= 5'd0;
            reg_irq_status      <= 5'd0;
            reg_scratch         <= 32'd0;
            for (i = 0; i < 20; i = i + 1)
                reg_slot_table[i] <= 32'd0;

            awready_r           <= 1'b0;
            wready_r            <= 1'b0;
            bvalid_r            <= 1'b0;
            bresp_r             <= 2'b00;
            arready_r           <= 1'b0;
            rvalid_r            <= 1'b0;
            rresp_r             <= 2'b00;
            rdata_r             <= 32'd0;
            wr_addr_seen        <= 1'b0;
            wr_data_seen        <= 1'b0;
            wr_addr_q           <= {ADDR_WIDTH{1'b0}};
            wr_data_q           <= 32'd0;
            wr_strb_q           <= 4'd0;
            rd_addr_q           <= {ADDR_WIDTH{1'b0}};
            rd_addr_seen        <= 1'b0;
            wr_to_dma           <= 1'b0;
            rd_to_dma           <= 1'b0;
            dma_aw_pending      <= 1'b0;
            dma_w_pending       <= 1'b0;
            dma_b_pending       <= 1'b0;
            dma_ar_pending      <= 1'b0;
            dma_r_pending       <= 1'b0;
        end else begin
            // -------- HW-set R/W1C IRQ status (HW wins) -----------------
            reg_irq_status <= reg_irq_status | tlm_irq_set_pulse;

            // RST_CNTRS pulse self-clears.
            ctrl_rst_cntrs_pulse <= 1'b0;

            // ===== Write address phase ==================================
            if (!awready_r && s_axil_awvalid && !wr_addr_seen) begin
                wr_addr_q    <= s_axil_awaddr;
                wr_addr_seen <= 1'b1;
                awready_r    <= 1'b1;
            end else begin
                awready_r    <= 1'b0;
            end

            // ===== Write data phase =====================================
            if (!wready_r && s_axil_wvalid && !wr_data_seen) begin
                wr_data_q    <= s_axil_wdata;
                wr_strb_q    <= s_axil_wstrb;
                wr_data_seen <= 1'b1;
                wready_r     <= 1'b1;
            end else begin
                wready_r     <= 1'b0;
            end

            // ===== Write commit =========================================
            if (wr_addr_seen && wr_data_seen && !bvalid_r &&
                !dma_aw_pending && !dma_w_pending && !dma_b_pending) begin

                if (is_misaligned(wr_addr_q)) begin
                    bresp_r <= 2'b10;       // SLVERR
                    bvalid_r <= 1'b1;
                end else if (is_dma_subwin(wr_addr_q)) begin
                    // Forward to DMA wrapper sub-window.
                    wr_to_dma      <= 1'b1;
                    dma_aw_pending <= 1'b1;
                    dma_w_pending  <= 1'b1;
                    dma_b_pending  <= 1'b1;
                    bresp_r        <= 2'b00;
                end else begin
                    bresp_r  <= 2'b00;       // OKAY
                    bvalid_r <= 1'b1;

                    case (wr_addr_q[11:0])
                        // ---- Configuration region 0x000..0x0FC -------
                        12'h000: reg_cell_mcc       <= wr_data_q[9:0];
                        12'h004: reg_cell_mnc       <= wr_data_q[13:0];
                        12'h008: reg_cell_cc        <= wr_data_q[5:0];
                        12'h00C: reg_cell_la        <= wr_data_q[13:0];
                        12'h010: reg_rx_carrier_hz  <= wr_data_q;
                        12'h014: reg_tx_carrier_hz  <= wr_data_q;
                        12'h018: reg_tx_gain_trim   <= wr_data_q[7:0];
                        12'h01C: reg_cipher_mode    <= wr_data_q[1:0];
                        12'h020: reg_scrambler_init <= wr_data_q;
                        12'h024: reg_ts_n           <= wr_data_q[11:0];
                        12'h028: reg_ts_p           <= wr_data_q[11:0];
                        12'h02C: reg_ts_q           <= wr_data_q[11:0];
                        12'h080: begin
                            reg_ctrl[2:0] <= wr_data_q[2:0];
                            // RST_CNTRS bit pulses: latch a 1-cycle pulse
                            // when the SW writes 1; the bit itself
                            // self-clears (it never holds 1 in reg_ctrl).
                            if (wr_data_q[3]) ctrl_rst_cntrs_pulse <= 1'b1;
                        end
                        12'h084: reg_sync_thresh    <= wr_data_q[7:0];
                        12'h088: reg_rx_gain        <= wr_data_q[6:0];
                        12'h08C: reg_tx_att         <= wr_data_q[7:0];
                        12'h090: reg_irq_enable     <= wr_data_q[4:0];
                        // ---- Telemetry region 0x100..0x17F -----------
                        // Only REG_IRQ_STATUS is W1C; everything else
                        // is R/O.
                        12'h10C: begin
                            // W1C: clear bits where wr_data_q has 1,
                            // BUT HW-set wins — i.e. if a HW pulse
                            // happens this cycle it overrides the clear.
                            reg_irq_status <= (reg_irq_status & ~wr_data_q[4:0])
                                              | tlm_irq_set_pulse;
                        end
                        // ---- Test/Scratch region 0x200 ---------------
                        12'h200: begin
                            // Byte-laned scratch; honour wstrb.
                            if (wr_strb_q[0]) reg_scratch[7:0]   <= wr_data_q[7:0];
                            if (wr_strb_q[1]) reg_scratch[15:8]  <= wr_data_q[15:8];
                            if (wr_strb_q[2]) reg_scratch[23:16] <= wr_data_q[23:16];
                            if (wr_strb_q[3]) reg_scratch[31:24] <= wr_data_q[31:24];
                        end
                        default: begin
                            // SLOT_TABLE region 0x030..0x07C (20 words).
                            if ((wr_addr_q[11:0] >= 12'h030) &&
                                (wr_addr_q[11:0] <= 12'h07C)) begin
                                reg_slot_table[(wr_addr_q[11:0] - 12'h030) >> 2]
                                    <= wr_data_q;
                            end
                            // All other addresses: writes silently
                            // dropped (incl. forbidden REG_SHADOW_*,
                            // REG_PROFILE_*, REG_DB_POLICY,
                            // REG_AACH_GRANT_HINT — they fall in the
                            // reserved bands).
                        end
                    endcase
                end

                // Clear the per-transaction latches on commit (DMA
                // path defers BVALID until subwin response arrives).
                if (!is_dma_subwin(wr_addr_q) || is_misaligned(wr_addr_q)) begin
                    wr_addr_seen <= 1'b0;
                    wr_data_seen <= 1'b0;
                end
            end

            // ===== B-channel handshake (local writes) ==================
            if (bvalid_r && s_axil_bready) begin
                bvalid_r <= 1'b0;
            end

            // ===== DMA sub-window write handshake ======================
            if (dma_aw_pending && dma_subwin_awready) begin
                dma_aw_pending <= 1'b0;
            end
            if (dma_w_pending && dma_subwin_wready) begin
                dma_w_pending  <= 1'b0;
            end
            if (dma_b_pending && dma_subwin_bvalid) begin
                bresp_r        <= dma_subwin_bresp;
                bvalid_r       <= 1'b1;
                dma_b_pending  <= 1'b0;
                wr_to_dma      <= 1'b0;
                wr_addr_seen   <= 1'b0;
                wr_data_seen   <= 1'b0;
            end

            // ===== Read address phase ==================================
            if (!arready_r && s_axil_arvalid && !rd_addr_seen && !rvalid_r &&
                !dma_ar_pending && !dma_r_pending) begin
                rd_addr_q    <= s_axil_araddr;
                rd_addr_seen <= 1'b1;
                arready_r    <= 1'b1;
            end else begin
                arready_r    <= 1'b0;
            end

            // ===== Read commit =========================================
            if (rd_addr_seen && !rvalid_r && !dma_ar_pending && !dma_r_pending) begin
                if (is_misaligned(rd_addr_q)) begin
                    rresp_r <= 2'b10;       // SLVERR
                    rdata_r <= 32'd0;
                    rvalid_r <= 1'b1;
                    rd_addr_seen <= 1'b0;
                end else if (is_dma_subwin(rd_addr_q)) begin
                    // Forward to DMA wrapper.
                    rd_to_dma      <= 1'b1;
                    dma_ar_pending <= 1'b1;
                    dma_r_pending  <= 1'b1;
                end else begin
                    rresp_r  <= 2'b00;
                    rvalid_r <= 1'b1;
                    rd_addr_seen <= 1'b0;
                    case (rd_addr_q[11:0])
                        // Configuration
                        12'h000: rdata_r <= {22'd0, reg_cell_mcc};
                        12'h004: rdata_r <= {18'd0, reg_cell_mnc};
                        12'h008: rdata_r <= {26'd0, reg_cell_cc};
                        12'h00C: rdata_r <= {18'd0, reg_cell_la};
                        12'h010: rdata_r <= reg_rx_carrier_hz;
                        12'h014: rdata_r <= reg_tx_carrier_hz;
                        12'h018: rdata_r <= {24'd0, reg_tx_gain_trim};
                        12'h01C: rdata_r <= {30'd0, reg_cipher_mode};
                        12'h020: rdata_r <= reg_scrambler_init;
                        12'h024: rdata_r <= {20'd0, reg_ts_n};
                        12'h028: rdata_r <= {20'd0, reg_ts_p};
                        12'h02C: rdata_r <= {20'd0, reg_ts_q};
                        12'h080: rdata_r <= {28'd0, 1'b0, reg_ctrl[2:0]};
                        12'h084: rdata_r <= {24'd0, reg_sync_thresh};
                        12'h088: rdata_r <= {25'd0, reg_rx_gain};
                        12'h08C: rdata_r <= {24'd0, reg_tx_att};
                        12'h090: rdata_r <= {27'd0, reg_irq_enable};
                        12'h0FC: rdata_r <= VERSION_DEFAULT;
                        // Telemetry
                        12'h100: rdata_r <= {24'd0, tlm_phy_status};
                        12'h104: rdata_r <= {27'd0, tlm_frame_num};
                        12'h108: rdata_r <= {30'd0, tlm_slot_num};
                        12'h10C: rdata_r <= {27'd0, reg_irq_status};
                        12'h110: rdata_r <= {16'd0, tlm_dma_blk_cnt};
                        12'h114: rdata_r <= {16'd0, tlm_crc_err_cnt};
                        12'h118: rdata_r <= {16'd0, tlm_sync_lst_cnt};
                        12'h11C: rdata_r <= tlm_frame_tick_cnt;
                        12'h120: rdata_r <= tlm_dma_tma_rx_frames;
                        12'h124: rdata_r <= tlm_dma_tma_tx_frames;
                        12'h128: rdata_r <= tlm_dma_tmd_rx_frames;
                        12'h12C: rdata_r <= tlm_dma_tmd_tx_frames;
                        12'h130: rdata_r <= tlm_dma_irq_cnt_rx;
                        12'h134: rdata_r <= tlm_dma_irq_cnt_tx;
                        12'h138: rdata_r <= {16'd0, tlm_dma_overrun_cnt};
                        12'h13C: rdata_r <= {16'd0, tlm_dma_underrun_cnt};
                        12'h140: rdata_r <= {16'd0, tlm_aach_last_raw};
                        12'h144: rdata_r <= tlm_aach_transition_cnt;
                        12'h148: rdata_r <= tlm_aach_idle_cnt;
                        12'h14C: rdata_r <= tlm_aach_sig_active_cnt;
                        12'h150: rdata_r <= tlm_aach_traffic_cnt;
                        12'h154: rdata_r <= {24'd0, tlm_umac_dlq_depth};
                        12'h158: rdata_r <= {16'd0, tlm_umac_dlq_drops};
                        12'h15C: rdata_r <= {16'd0, tlm_umac_reasm_fail_cnt};
                        12'h160: rdata_r <= tlm_tmasap_rx_frames_cnt;
                        12'h164: rdata_r <= tlm_tmasap_tx_frames_cnt;
                        12'h168: rdata_r <= {16'd0, tlm_tmasap_tx_err_cnt};
                        12'h16C: rdata_r <= tlm_tmar_frames_cnt;
                        12'h170: rdata_r <= tlm_tmdsap_tx_frames_cnt;
                        12'h174: rdata_r <= tlm_tmdsap_rx_frames_cnt;
                        12'h178: rdata_r <= tlm_tmdsap_err_cnt;
                        // Test/Scratch
                        12'h200: rdata_r <= reg_scratch;
                        default: begin
                            // SLOT_TABLE readback.
                            if ((rd_addr_q[11:0] >= 12'h030) &&
                                (rd_addr_q[11:0] <= 12'h07C)) begin
                                rdata_r <= reg_slot_table[
                                    (rd_addr_q[11:0] - 12'h030) >> 2];
                            end else begin
                                // Reserved / forbidden -> 0.
                                rdata_r <= 32'd0;
                            end
                        end
                    endcase
                end
            end

            // ===== R-channel handshake =================================
            if (rvalid_r && s_axil_rready) begin
                rvalid_r <= 1'b0;
            end

            // ===== DMA sub-window read handshake =======================
            if (dma_ar_pending && dma_subwin_arready) begin
                dma_ar_pending <= 1'b0;
            end
            if (dma_r_pending && dma_subwin_rvalid) begin
                rdata_r        <= dma_subwin_rdata;
                rresp_r        <= dma_subwin_rresp;
                rvalid_r       <= 1'b1;
                dma_r_pending  <= 1'b0;
                rd_to_dma      <= 1'b0;
                rd_addr_seen   <= 1'b0;
            end
        end
    end

endmodule

`default_nettype wire
