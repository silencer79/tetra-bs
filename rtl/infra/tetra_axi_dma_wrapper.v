// rtl/infra/tetra_axi_dma_wrapper.v
//
// Owned by Agent A1 (A1-fpga-axi-dma).
//
// Top-level wrapper that instantiates 4× Xilinx LogiCORE `axi_dma_v7_1`
// IP cores in scatter-gather mode, exposes:
//
//   - 4× AXIS streams on the fabric side (S2MM RX into the IP,
//     MM2S TX out of the IP):
//       s_axis_tma_rx_*  (S2MM IP slave; FPGA→PS direction; UMAC→DMA→DDR)
//       s_axis_tmd_rx_*  (S2MM IP slave; FPGA→PS direction; UMAC→DMA→DDR)
//       m_axis_tma_tx_*  (MM2S IP master; PS→FPGA direction; DDR→DMA→UMAC)
//       m_axis_tmd_tx_*  (MM2S IP master; PS→FPGA direction; DDR→DMA→UMAC)
//
//   - 4× AXI4 memory-mapped masters into the PS DDR (`m_axi_*` per
//     channel; in production these collapse via an AXI-Interconnect into
//     the PS7 HP slave port HP0 and HP1, see `dts/tetra_axi_dma_overlay.dtsi`).
//
//   - 1× AXI-Lite slave for status/control. The wrapper owns a small
//     16-byte sub-window of the global `tetra_axi_lite_regs.v` map
//     (Agent A5) at offsets 0x0A0..0x0AF (R/W) within the config region:
//
//        offset  name                 width  R/W   reset  description
//        ------  ----                 -----  ---   -----  -----------
//        0x0A0   REG_DMA_CH_ENABLE    [3:0]  R/W   0      bit-per-channel enable
//                                                          [0] tma_rx, [1] tma_tx,
//                                                          [2] tmd_rx, [3] tmd_tx
//        0x0A4   REG_DMA_CH_RESET     [3:0]  R/W   0      W1-pulse soft-reset per ch
//                                                          (self-clearing after 1 cyc)
//        0x0A8   REG_DMA_IRQ_ENABLE   [3:0]  R/W   0      per-channel IRQ enable mask
//        0x0AC   REG_DMA_IRQ_STATUS   [3:0]  R/W1C 0      per-channel sticky IRQ status
//
//     The sub-window lives within the 0x094..0x0FB "reserved (config)"
//     band reserved by Agent A5 in `docs/ARCHITECTURE.md` §"AXI-Lite
//     Live-Config Register Window" (no overlap with claimed offsets).
//     A5 wires this sub-window through to its top-level bank by
//     forwarding the 0x0A0..0x0AF range to this wrapper's AXI-Lite slave.
//     The wrapper is also accessible by libaxidma / xilinx_axidma.ko
//     directly via the per-IP register windows (offsets internal to each
//     `axi_dma:7.1` instance — those are the standard Xilinx layout and
//     are NOT exposed through this wrapper's sub-window; see DT overlay).
//
//   - 4× IRQ outputs, one per channel (driven by a logical OR of the
//     IP's mm2s_introut and s2mm_introut where applicable, gated by the
//     per-channel IRQ-enable bit). The PS routes these to GIC IDs per
//     `dts/tetra_axi_dma_overlay.dtsi`:
//
//        irq_tma_rx_o   (S2MM completion of the TmaSap-RX FPGA→PS channel)
//        irq_tma_tx_o   (MM2S completion of the TmaSap-TX PS→FPGA channel)
//        irq_tmd_rx_o   (S2MM completion of the TmdSap-RX FPGA→PS channel)
//        irq_tmd_tx_o   (MM2S completion of the TmdSap-TX PS→FPGA channel)
//
// AXIS handshake conventions (locked, IF_AXIDMA_v1 in MIGRATION_PLAN.md):
//   - 32-bit data: `tdata[31:0]`, MSB lane = first byte on the wire.
//   - `tvalid`/`tready` standard handshake: data must be held stable
//     while tvalid HIGH and tready LOW.
//   - `tlast` HIGH on the final beat of each frame.
//   - `tkeep[3:0]` per-byte-lane validity. We always drive 4'b1111 except
//     possibly on the final beat (contiguous from MSB). Sparse keep
//     patterns are NOT generated and are dropped on RX (`drop_count++`).
//
// IP-instantiation strategy for simulation:
//   The actual `axi_dma_v7_1` IP requires Xilinx UNISIM libraries that
//   iverilog does not ship. This wrapper instantiates the IP via a
//   parameterised wrapper macro `AXI_DMA_INSTANCE_*` that defaults to
//   the real Xilinx black-box module name; for test benches we override
//   the search path to `tb/rtl/models/axi_dma_v7_1_bhv.v` (a behavioural
//   model that mimics the AXIS↔DDR transfer, see
//   `tb/rtl/models/README.md`). The Vivado/Phase-4-synth flow uses the
//   real IP via `rtl/infra/ip/axi_dma_*.tcl` (deferred — see
//   MIGRATION_PLAN.md §A1).
//
// Verilog-2001 only (CLAUDE.md §Languages).

`timescale 1ns / 1ps
`default_nettype none

module tetra_axi_dma_wrapper #(
    // Fabric AXIS data width (locked at 32 per IF_AXIDMA_v1).
    parameter AXIS_TDATA_WIDTH = 32,
    parameter AXIS_TKEEP_WIDTH = 4,
    // PS-DDR AXI4 master geometry. 32-bit data path matches PS7 HP slave.
    parameter MM_ADDR_WIDTH    = 32,
    parameter MM_DATA_WIDTH    = 32,
    // Sub-window AXI-Lite slave geometry.
    parameter LITE_ADDR_WIDTH  = 4   // 16 bytes / 4 regs
) (
    // -----------------------------------------------------------------
    // Clocks / resets (locked: single AXI-clock domain at the wrapper
    // boundary; clk_sys ↔ clk_axi CDC handled by Agent A4 outside).
    // -----------------------------------------------------------------
    input  wire                            clk_axi,
    input  wire                            rstn_axi,

    // -----------------------------------------------------------------
    // AXI-Lite slave — wrapper sub-window (0x0A0..0x0AF in the global map,
    // local offsets 0x0..0xF). Standard AXI4-Lite signals.
    // -----------------------------------------------------------------
    input  wire [LITE_ADDR_WIDTH-1:0]      s_axil_awaddr,
    input  wire                            s_axil_awvalid,
    output wire                            s_axil_awready,
    input  wire [31:0]                     s_axil_wdata,
    input  wire [3:0]                      s_axil_wstrb,
    input  wire                            s_axil_wvalid,
    output wire                            s_axil_wready,
    output wire [1:0]                      s_axil_bresp,
    output wire                            s_axil_bvalid,
    input  wire                            s_axil_bready,
    input  wire [LITE_ADDR_WIDTH-1:0]      s_axil_araddr,
    input  wire                            s_axil_arvalid,
    output wire                            s_axil_arready,
    output wire [31:0]                     s_axil_rdata,
    output wire [1:0]                      s_axil_rresp,
    output wire                            s_axil_rvalid,
    input  wire                            s_axil_rready,

    // -----------------------------------------------------------------
    // 4× AXIS slave (FPGA→DMA, the IP's S2MM input). RX direction.
    // tma_rx + tmd_rx are the FPGA→PS channels. Note that "RX" here
    // refers to the SAP-level direction (FPGA→PS = SW receives), not
    // the AXIS-level direction.
    // -----------------------------------------------------------------
    input  wire [AXIS_TDATA_WIDTH-1:0]     s_axis_tma_rx_tdata,
    input  wire                            s_axis_tma_rx_tvalid,
    output wire                            s_axis_tma_rx_tready,
    input  wire                            s_axis_tma_rx_tlast,
    input  wire [AXIS_TKEEP_WIDTH-1:0]     s_axis_tma_rx_tkeep,

    input  wire [AXIS_TDATA_WIDTH-1:0]     s_axis_tmd_rx_tdata,
    input  wire                            s_axis_tmd_rx_tvalid,
    output wire                            s_axis_tmd_rx_tready,
    input  wire                            s_axis_tmd_rx_tlast,
    input  wire [AXIS_TKEEP_WIDTH-1:0]     s_axis_tmd_rx_tkeep,

    // -----------------------------------------------------------------
    // 4× AXIS master (DMA→FPGA, the IP's MM2S output). TX direction.
    // tma_tx + tmd_tx are the PS→FPGA channels.
    // -----------------------------------------------------------------
    output wire [AXIS_TDATA_WIDTH-1:0]     m_axis_tma_tx_tdata,
    output wire                            m_axis_tma_tx_tvalid,
    input  wire                            m_axis_tma_tx_tready,
    output wire                            m_axis_tma_tx_tlast,
    output wire [AXIS_TKEEP_WIDTH-1:0]     m_axis_tma_tx_tkeep,

    output wire [AXIS_TDATA_WIDTH-1:0]     m_axis_tmd_tx_tdata,
    output wire                            m_axis_tmd_tx_tvalid,
    input  wire                            m_axis_tmd_tx_tready,
    output wire                            m_axis_tmd_tx_tlast,
    output wire [AXIS_TKEEP_WIDTH-1:0]     m_axis_tmd_tx_tkeep,

    // -----------------------------------------------------------------
    // 4× AXI4 MM master to PS DDR (one per channel). Heavily abridged:
    // we expose only the address/data/write-strobe/handshake ports
    // needed in simulation. In synth, the full AXI4 interface is wired
    // through the Vivado Block Design auto-generated by `axi_dma_*.tcl`.
    //
    // For Verilog-2001 portability and TB compactness, only the read +
    // write address/data are exposed here — the burst/lock/cache/prot
    // ports are tied off internally.
    //
    // The 4 sets are: tma_rx (S2MM write-only), tma_tx (MM2S read-only),
    // tmd_rx (S2MM write-only), tmd_tx (MM2S read-only). For a uniform
    // port list we expose all 5 channels as full AXI4; the unused half
    // (read on S2MM, write on MM2S) is tied 0 internally by the IP and
    // by the behavioural model.
    // -----------------------------------------------------------------
    output wire [MM_ADDR_WIDTH-1:0]        m_axi_tma_rx_awaddr,
    output wire                            m_axi_tma_rx_awvalid,
    input  wire                            m_axi_tma_rx_awready,
    output wire [MM_DATA_WIDTH-1:0]        m_axi_tma_rx_wdata,
    output wire                            m_axi_tma_rx_wvalid,
    input  wire                            m_axi_tma_rx_wready,
    output wire                            m_axi_tma_rx_wlast,
    input  wire [1:0]                      m_axi_tma_rx_bresp,
    input  wire                            m_axi_tma_rx_bvalid,
    output wire                            m_axi_tma_rx_bready,

    output wire [MM_ADDR_WIDTH-1:0]        m_axi_tma_tx_araddr,
    output wire                            m_axi_tma_tx_arvalid,
    input  wire                            m_axi_tma_tx_arready,
    input  wire [MM_DATA_WIDTH-1:0]        m_axi_tma_tx_rdata,
    input  wire                            m_axi_tma_tx_rvalid,
    output wire                            m_axi_tma_tx_rready,
    input  wire                            m_axi_tma_tx_rlast,
    input  wire [1:0]                      m_axi_tma_tx_rresp,

    output wire [MM_ADDR_WIDTH-1:0]        m_axi_tmd_rx_awaddr,
    output wire                            m_axi_tmd_rx_awvalid,
    input  wire                            m_axi_tmd_rx_awready,
    output wire [MM_DATA_WIDTH-1:0]        m_axi_tmd_rx_wdata,
    output wire                            m_axi_tmd_rx_wvalid,
    input  wire                            m_axi_tmd_rx_wready,
    output wire                            m_axi_tmd_rx_wlast,
    input  wire [1:0]                      m_axi_tmd_rx_bresp,
    input  wire                            m_axi_tmd_rx_bvalid,
    output wire                            m_axi_tmd_rx_bready,

    output wire [MM_ADDR_WIDTH-1:0]        m_axi_tmd_tx_araddr,
    output wire                            m_axi_tmd_tx_arvalid,
    input  wire                            m_axi_tmd_tx_arready,
    input  wire [MM_DATA_WIDTH-1:0]        m_axi_tmd_tx_rdata,
    input  wire                            m_axi_tmd_tx_rvalid,
    output wire                            m_axi_tmd_tx_rready,
    input  wire                            m_axi_tmd_tx_rlast,
    input  wire [1:0]                      m_axi_tmd_tx_rresp,

    // -----------------------------------------------------------------
    // Per-channel IRQs to PS GIC.
    // -----------------------------------------------------------------
    output wire                            irq_tma_rx_o,
    output wire                            irq_tma_tx_o,
    output wire                            irq_tmd_rx_o,
    output wire                            irq_tmd_tx_o,

    // -----------------------------------------------------------------
    // Telemetry passed up to A5 register window (REG_DMA_*_FRAMES @
    // 0x120..0x12C and REG_DMA_OVERRUN_CNT @ 0x138).
    // -----------------------------------------------------------------
    output wire [31:0]                     tlm_tma_rx_frames,
    output wire [31:0]                     tlm_tma_tx_frames,
    output wire [31:0]                     tlm_tmd_rx_frames,
    output wire [31:0]                     tlm_tmd_tx_frames,
    output wire [15:0]                     tlm_overrun_cnt,
    output wire [15:0]                     tlm_underrun_cnt
);

    // ================================================================
    // Channel index legend (used in arrays below):
    //   0 = tma_rx (S2MM, FPGA→PS)
    //   1 = tma_tx (MM2S, PS→FPGA)
    //   2 = tmd_rx (S2MM, FPGA→PS)
    //   3 = tmd_tx (MM2S, PS→FPGA)
    // ================================================================

    // ----- AXI-Lite sub-window register file --------------------------
    // 4 registers @ offsets 0x0..0xC (local). Word-aligned.
    reg [3:0] reg_ch_enable;
    reg [3:0] reg_ch_reset;     // self-clearing: each bit pulses 1 cycle
    reg [3:0] reg_irq_enable;
    reg [3:0] reg_irq_status;   // R/W1C; HW-set wins

    // Simple AXI-Lite slave FSM (no outstanding transactions; one at a time).
    reg        axil_awready_r, axil_wready_r, axil_bvalid_r;
    reg        axil_arready_r, axil_rvalid_r;
    reg [31:0] axil_rdata_r;
    reg [1:0]  axil_bresp_r, axil_rresp_r;
    reg        wr_addr_seen, wr_data_seen;
    reg [LITE_ADDR_WIDTH-1:0] wr_addr_q;
    reg [LITE_ADDR_WIDTH-1:0] rd_addr_q;
    reg [31:0] wr_data_q;
    reg [3:0]  wr_strb_q;

    assign s_axil_awready = axil_awready_r;
    assign s_axil_wready  = axil_wready_r;
    assign s_axil_bvalid  = axil_bvalid_r;
    assign s_axil_bresp   = axil_bresp_r;
    assign s_axil_arready = axil_arready_r;
    assign s_axil_rvalid  = axil_rvalid_r;
    assign s_axil_rdata   = axil_rdata_r;
    assign s_axil_rresp   = axil_rresp_r;

    // Per-channel IRQ rising-edge detect (IP completion → set sticky bit).
    wire [3:0] ch_irq_pulse;     // 1-cycle pulse from each IP instance
    wire [3:0] ch_irq_pulse_q;   // edge-detected version (this design uses
                                 // pulses directly from the IPs/models)
    assign ch_irq_pulse_q = ch_irq_pulse;

    // Per-channel write-1-pulse helpers.
    reg [3:0] ch_reset_pulse;    // combinational pulse to IP rstn

    always @(posedge clk_axi or negedge rstn_axi) begin
        if (!rstn_axi) begin
            reg_ch_enable  <= 4'b0000;
            reg_ch_reset   <= 4'b0000;
            reg_irq_enable <= 4'b0000;
            reg_irq_status <= 4'b0000;
            axil_awready_r <= 1'b0;
            axil_wready_r  <= 1'b0;
            axil_bvalid_r  <= 1'b0;
            axil_bresp_r   <= 2'b00;
            axil_arready_r <= 1'b0;
            axil_rvalid_r  <= 1'b0;
            axil_rdata_r   <= 32'h0;
            axil_rresp_r   <= 2'b00;
            wr_addr_seen   <= 1'b0;
            wr_data_seen   <= 1'b0;
            wr_addr_q      <= {LITE_ADDR_WIDTH{1'b0}};
            rd_addr_q      <= {LITE_ADDR_WIDTH{1'b0}};
            wr_data_q      <= 32'h0;
            wr_strb_q      <= 4'b0;
            ch_reset_pulse <= 4'b0;
        end else begin
            // ---- IRQ-status sticky update (HW-set wins over SW-clear) ----
            // Each pulse from the IP/model latches the bit.
            // SW-W1C clear is applied below in the write-handler.
            reg_irq_status <= reg_irq_status | ch_irq_pulse_q;

            // ---- self-clearing soft-reset pulse register ----
            // Bits set by an SW write below pulse for exactly 1 cycle.
            ch_reset_pulse <= reg_ch_reset;
            reg_ch_reset   <= 4'b0000;

            // ----- AW + W phase ------------------------------------
            if (!axil_awready_r && s_axil_awvalid && !wr_addr_seen) begin
                wr_addr_q      <= s_axil_awaddr;
                wr_addr_seen   <= 1'b1;
                axil_awready_r <= 1'b1;
            end else begin
                axil_awready_r <= 1'b0;
            end
            if (!axil_wready_r && s_axil_wvalid && !wr_data_seen) begin
                wr_data_q     <= s_axil_wdata;
                wr_strb_q     <= s_axil_wstrb;
                wr_data_seen  <= 1'b1;
                axil_wready_r <= 1'b1;
            end else begin
                axil_wready_r <= 1'b0;
            end

            // ----- write commit (AW + W both seen, no outstanding B) ---
            if (wr_addr_seen && wr_data_seen && !axil_bvalid_r) begin
                case (wr_addr_q[LITE_ADDR_WIDTH-1:2])
                    2'd0: begin // 0x0 — REG_DMA_CH_ENABLE
                        if (wr_strb_q[0]) reg_ch_enable <= wr_data_q[3:0];
                        axil_bresp_r <= 2'b00;
                    end
                    2'd1: begin // 0x4 — REG_DMA_CH_RESET (W1-pulse)
                        if (wr_strb_q[0]) reg_ch_reset <= wr_data_q[3:0];
                        axil_bresp_r <= 2'b00;
                    end
                    2'd2: begin // 0x8 — REG_DMA_IRQ_ENABLE
                        if (wr_strb_q[0]) reg_irq_enable <= wr_data_q[3:0];
                        axil_bresp_r <= 2'b00;
                    end
                    2'd3: begin // 0xC — REG_DMA_IRQ_STATUS (W1C)
                        if (wr_strb_q[0]) begin
                            // bits set by HW this cycle stay set;
                            // SW-set bits in wr_data_q clear the corresponding sticky bits.
                            reg_irq_status <= (reg_irq_status |
                                               ch_irq_pulse_q) &
                                              ~wr_data_q[3:0];
                        end
                        axil_bresp_r <= 2'b00;
                    end
                    default: axil_bresp_r <= 2'b00; // silently drop
                endcase
                axil_bvalid_r <= 1'b1;
                wr_addr_seen  <= 1'b0;
                wr_data_seen  <= 1'b0;
            end
            if (axil_bvalid_r && s_axil_bready) begin
                axil_bvalid_r <= 1'b0;
            end

            // ----- AR + R phase ------------------------------------
            if (!axil_arready_r && s_axil_arvalid && !axil_rvalid_r) begin
                rd_addr_q      <= s_axil_araddr;
                axil_arready_r <= 1'b1;
            end else begin
                axil_arready_r <= 1'b0;
            end
            if (axil_arready_r && !axil_rvalid_r) begin
                case (rd_addr_q[LITE_ADDR_WIDTH-1:2])
                    2'd0: axil_rdata_r <= {28'h0, reg_ch_enable};
                    2'd1: axil_rdata_r <= 32'h0; // reset register reads as 0
                    2'd2: axil_rdata_r <= {28'h0, reg_irq_enable};
                    2'd3: axil_rdata_r <= {28'h0, reg_irq_status};
                    default: axil_rdata_r <= 32'h0;
                endcase
                axil_rresp_r  <= 2'b00;
                axil_rvalid_r <= 1'b1;
            end
            if (axil_rvalid_r && s_axil_rready) begin
                axil_rvalid_r <= 1'b0;
            end
        end
    end

    // ----- Per-channel IP / behavioural-model instances --------------
    // Selectable via macro (TB defines AXI_DMA_BHV_MODEL to swap).
    //
    // For Verilog-2001 we cannot use a parameterised module-name, so we
    // gate the instantiation with `ifdef AXI_DMA_BHV_MODEL — when defined,
    // we use the behavioural model from `tb/rtl/models/axi_dma_v7_1_bhv.v`.
    // Otherwise we use the real Xilinx black-box `axi_dma_v7_1`.

    wire [3:0] ch_irq_mm2s, ch_irq_s2mm;
    wire [3:0] ch_overrun, ch_underrun;
    wire [31:0] frames_per_ch [3:0];

    // Per-channel reset: hold low when reg_ch_enable[i]=0 OR pulse from soft-reset.
    wire [3:0] ch_rstn;
    assign ch_rstn[0] = rstn_axi & reg_ch_enable[0] & ~ch_reset_pulse[0];
    assign ch_rstn[1] = rstn_axi & reg_ch_enable[1] & ~ch_reset_pulse[1];
    assign ch_rstn[2] = rstn_axi & reg_ch_enable[2] & ~ch_reset_pulse[2];
    assign ch_rstn[3] = rstn_axi & reg_ch_enable[3] & ~ch_reset_pulse[3];

    // ---- Channel 0: tma_rx (S2MM) ----
    axi_dma_channel_inst #(
        .CHANNEL_ID    (0),
        .DIR_IS_S2MM   (1),
        .AXIS_TDATA_W  (AXIS_TDATA_WIDTH),
        .AXIS_TKEEP_W  (AXIS_TKEEP_WIDTH),
        .MM_ADDR_W     (MM_ADDR_WIDTH),
        .MM_DATA_W     (MM_DATA_WIDTH)
    ) u_ch0_tma_rx (
        .clk_axi        (clk_axi),
        .rstn_axi       (ch_rstn[0]),
        // S2MM AXIS-slave (FPGA→DMA)
        .s_axis_tdata   (s_axis_tma_rx_tdata),
        .s_axis_tvalid  (s_axis_tma_rx_tvalid),
        .s_axis_tready  (s_axis_tma_rx_tready),
        .s_axis_tlast   (s_axis_tma_rx_tlast),
        .s_axis_tkeep   (s_axis_tma_rx_tkeep),
        // MM2S AXIS-master (unused on S2MM channel, tied off by model)
        .m_axis_tdata   (),
        .m_axis_tvalid  (),
        .m_axis_tready  (1'b0),
        .m_axis_tlast   (),
        .m_axis_tkeep   (),
        // AXI-MM master (S2MM = write to DDR)
        .m_axi_awaddr   (m_axi_tma_rx_awaddr),
        .m_axi_awvalid  (m_axi_tma_rx_awvalid),
        .m_axi_awready  (m_axi_tma_rx_awready),
        .m_axi_wdata    (m_axi_tma_rx_wdata),
        .m_axi_wvalid   (m_axi_tma_rx_wvalid),
        .m_axi_wready   (m_axi_tma_rx_wready),
        .m_axi_wlast    (m_axi_tma_rx_wlast),
        .m_axi_bresp    (m_axi_tma_rx_bresp),
        .m_axi_bvalid   (m_axi_tma_rx_bvalid),
        .m_axi_bready   (m_axi_tma_rx_bready),
        .m_axi_araddr   (),
        .m_axi_arvalid  (),
        .m_axi_arready  (1'b0),
        .m_axi_rdata    ({MM_DATA_WIDTH{1'b0}}),
        .m_axi_rvalid   (1'b0),
        .m_axi_rready   (),
        .m_axi_rlast    (1'b0),
        .m_axi_rresp    (2'b00),
        // IRQ + tlm
        .irq_done       (ch_irq_pulse[0]),
        .frame_count    (frames_per_ch[0]),
        .overrun_pulse  (ch_overrun[0]),
        .underrun_pulse (ch_underrun[0])
    );

    // ---- Channel 1: tma_tx (MM2S) ----
    axi_dma_channel_inst #(
        .CHANNEL_ID    (1),
        .DIR_IS_S2MM   (0),
        .AXIS_TDATA_W  (AXIS_TDATA_WIDTH),
        .AXIS_TKEEP_W  (AXIS_TKEEP_WIDTH),
        .MM_ADDR_W     (MM_ADDR_WIDTH),
        .MM_DATA_W     (MM_DATA_WIDTH)
    ) u_ch1_tma_tx (
        .clk_axi        (clk_axi),
        .rstn_axi       (ch_rstn[1]),
        .s_axis_tdata   ({AXIS_TDATA_WIDTH{1'b0}}),
        .s_axis_tvalid  (1'b0),
        .s_axis_tready  (),
        .s_axis_tlast   (1'b0),
        .s_axis_tkeep   ({AXIS_TKEEP_WIDTH{1'b0}}),
        .m_axis_tdata   (m_axis_tma_tx_tdata),
        .m_axis_tvalid  (m_axis_tma_tx_tvalid),
        .m_axis_tready  (m_axis_tma_tx_tready),
        .m_axis_tlast   (m_axis_tma_tx_tlast),
        .m_axis_tkeep   (m_axis_tma_tx_tkeep),
        .m_axi_awaddr   (),
        .m_axi_awvalid  (),
        .m_axi_awready  (1'b0),
        .m_axi_wdata    (),
        .m_axi_wvalid   (),
        .m_axi_wready   (1'b0),
        .m_axi_wlast    (),
        .m_axi_bresp    (2'b00),
        .m_axi_bvalid   (1'b0),
        .m_axi_bready   (),
        .m_axi_araddr   (m_axi_tma_tx_araddr),
        .m_axi_arvalid  (m_axi_tma_tx_arvalid),
        .m_axi_arready  (m_axi_tma_tx_arready),
        .m_axi_rdata    (m_axi_tma_tx_rdata),
        .m_axi_rvalid   (m_axi_tma_tx_rvalid),
        .m_axi_rready   (m_axi_tma_tx_rready),
        .m_axi_rlast    (m_axi_tma_tx_rlast),
        .m_axi_rresp    (m_axi_tma_tx_rresp),
        .irq_done       (ch_irq_pulse[1]),
        .frame_count    (frames_per_ch[1]),
        .overrun_pulse  (ch_overrun[1]),
        .underrun_pulse (ch_underrun[1])
    );

    // ---- Channel 2: tmd_rx (S2MM) ----
    axi_dma_channel_inst #(
        .CHANNEL_ID    (2),
        .DIR_IS_S2MM   (1),
        .AXIS_TDATA_W  (AXIS_TDATA_WIDTH),
        .AXIS_TKEEP_W  (AXIS_TKEEP_WIDTH),
        .MM_ADDR_W     (MM_ADDR_WIDTH),
        .MM_DATA_W     (MM_DATA_WIDTH)
    ) u_ch2_tmd_rx (
        .clk_axi        (clk_axi),
        .rstn_axi       (ch_rstn[2]),
        .s_axis_tdata   (s_axis_tmd_rx_tdata),
        .s_axis_tvalid  (s_axis_tmd_rx_tvalid),
        .s_axis_tready  (s_axis_tmd_rx_tready),
        .s_axis_tlast   (s_axis_tmd_rx_tlast),
        .s_axis_tkeep   (s_axis_tmd_rx_tkeep),
        .m_axis_tdata   (),
        .m_axis_tvalid  (),
        .m_axis_tready  (1'b0),
        .m_axis_tlast   (),
        .m_axis_tkeep   (),
        .m_axi_awaddr   (m_axi_tmd_rx_awaddr),
        .m_axi_awvalid  (m_axi_tmd_rx_awvalid),
        .m_axi_awready  (m_axi_tmd_rx_awready),
        .m_axi_wdata    (m_axi_tmd_rx_wdata),
        .m_axi_wvalid   (m_axi_tmd_rx_wvalid),
        .m_axi_wready   (m_axi_tmd_rx_wready),
        .m_axi_wlast    (m_axi_tmd_rx_wlast),
        .m_axi_bresp    (m_axi_tmd_rx_bresp),
        .m_axi_bvalid   (m_axi_tmd_rx_bvalid),
        .m_axi_bready   (m_axi_tmd_rx_bready),
        .m_axi_araddr   (),
        .m_axi_arvalid  (),
        .m_axi_arready  (1'b0),
        .m_axi_rdata    ({MM_DATA_WIDTH{1'b0}}),
        .m_axi_rvalid   (1'b0),
        .m_axi_rready   (),
        .m_axi_rlast    (1'b0),
        .m_axi_rresp    (2'b00),
        .irq_done       (ch_irq_pulse[2]),
        .frame_count    (frames_per_ch[2]),
        .overrun_pulse  (ch_overrun[2]),
        .underrun_pulse (ch_underrun[2])
    );

    // ---- Channel 3: tmd_tx (MM2S) ----
    axi_dma_channel_inst #(
        .CHANNEL_ID    (3),
        .DIR_IS_S2MM   (0),
        .AXIS_TDATA_W  (AXIS_TDATA_WIDTH),
        .AXIS_TKEEP_W  (AXIS_TKEEP_WIDTH),
        .MM_ADDR_W     (MM_ADDR_WIDTH),
        .MM_DATA_W     (MM_DATA_WIDTH)
    ) u_ch3_tmd_tx (
        .clk_axi        (clk_axi),
        .rstn_axi       (ch_rstn[3]),
        .s_axis_tdata   ({AXIS_TDATA_WIDTH{1'b0}}),
        .s_axis_tvalid  (1'b0),
        .s_axis_tready  (),
        .s_axis_tlast   (1'b0),
        .s_axis_tkeep   ({AXIS_TKEEP_WIDTH{1'b0}}),
        .m_axis_tdata   (m_axis_tmd_tx_tdata),
        .m_axis_tvalid  (m_axis_tmd_tx_tvalid),
        .m_axis_tready  (m_axis_tmd_tx_tready),
        .m_axis_tlast   (m_axis_tmd_tx_tlast),
        .m_axis_tkeep   (m_axis_tmd_tx_tkeep),
        .m_axi_awaddr   (),
        .m_axi_awvalid  (),
        .m_axi_awready  (1'b0),
        .m_axi_wdata    (),
        .m_axi_wvalid   (),
        .m_axi_wready   (1'b0),
        .m_axi_wlast    (),
        .m_axi_bresp    (2'b00),
        .m_axi_bvalid   (1'b0),
        .m_axi_bready   (),
        .m_axi_araddr   (m_axi_tmd_tx_araddr),
        .m_axi_arvalid  (m_axi_tmd_tx_arvalid),
        .m_axi_arready  (m_axi_tmd_tx_arready),
        .m_axi_rdata    (m_axi_tmd_tx_rdata),
        .m_axi_rvalid   (m_axi_tmd_tx_rvalid),
        .m_axi_rready   (m_axi_tmd_tx_rready),
        .m_axi_rlast    (m_axi_tmd_tx_rlast),
        .m_axi_rresp    (m_axi_tmd_tx_rresp),
        .irq_done       (ch_irq_pulse[3]),
        .frame_count    (frames_per_ch[3]),
        .overrun_pulse  (ch_overrun[3]),
        .underrun_pulse (ch_underrun[3])
    );

    // ----- IRQ output gating -----------------------------------------
    // Per-channel IRQ output is the sticky status bit AND the enable mask.
    assign irq_tma_rx_o = reg_irq_status[0] & reg_irq_enable[0];
    assign irq_tma_tx_o = reg_irq_status[1] & reg_irq_enable[1];
    assign irq_tmd_rx_o = reg_irq_status[2] & reg_irq_enable[2];
    assign irq_tmd_tx_o = reg_irq_status[3] & reg_irq_enable[3];

    // ----- Telemetry to A5 --------------------------------------------
    assign tlm_tma_rx_frames = frames_per_ch[0];
    assign tlm_tma_tx_frames = frames_per_ch[1];
    assign tlm_tmd_rx_frames = frames_per_ch[2];
    assign tlm_tmd_tx_frames = frames_per_ch[3];

    // Saturating overrun/underrun counters across all channels.
    reg [15:0] overrun_cnt_r, underrun_cnt_r;
    wire any_overrun  = |ch_overrun;
    wire any_underrun = |ch_underrun;
    always @(posedge clk_axi or negedge rstn_axi) begin
        if (!rstn_axi) begin
            overrun_cnt_r  <= 16'd0;
            underrun_cnt_r <= 16'd0;
        end else begin
            if (any_overrun  && overrun_cnt_r  != 16'hFFFF)
                overrun_cnt_r  <= overrun_cnt_r  + 16'd1;
            if (any_underrun && underrun_cnt_r != 16'hFFFF)
                underrun_cnt_r <= underrun_cnt_r + 16'd1;
        end
    end
    assign tlm_overrun_cnt  = overrun_cnt_r;
    assign tlm_underrun_cnt = underrun_cnt_r;

endmodule

`default_nettype wire
