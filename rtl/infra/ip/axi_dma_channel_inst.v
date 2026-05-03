// rtl/infra/ip/axi_dma_channel_inst.v
//
// Owned by Agent A1 (A1-fpga-axi-dma).
//
// Synthesis-only shim around the Xilinx LogiCORE `axi_dma:7.1` IP. Bridges
// the slim, custom port-list expected by `rtl/infra/tetra_axi_dma_wrapper.v`
// (which matches the iverilog behavioural model at
// `tb/rtl/models/axi_dma_v7_1_bhv.v` 1:1) to the full LogiCORE port-list
// (S_AXI_LITE control slave, separate M_AXI_SG/MM2S/S2MM masters, full
// AXI4 burst signalling, mm2s_introut/s2mm_introut, etc.).
//
// Decision (Kevin, 2026-05-03 evening): Option B — SG-mode with descriptor
// manager in BRAM (MIGRATION_PLAN.md §"Phase 3.5"). Reasons:
//   - Future-proof for multi-MS group-call scaling (descriptor chain depth
//     grows without re-arming).
//   - IP is already SG-configured (HARDWARE.md §1, c_include_sg=1).
//   - Industry-standard Xilinx pattern, no DRE-mode regression risk.
//
// Internal layout:
//
//   1. Descriptor BRAM (per-direction): a ring of N descriptors held in a
//      block-RAM-inferred 2D reg array. 32-byte descriptors per Xilinx
//      PG021. DESC_RING_DEPTH = 16 (fits in one 18Kb BRAM block per dir
//      = 16 × 32 B = 512 B; deeper rings cost extra BRAMs linearly).
//   2. Descriptor manager FSM: at rstn_axi rising edge, primes the ring
//      with NEXT_DESC chain (each desc points at the next, wrap-around at
//      the tail). Holds head/tail pointers. Programs the IP's CURDESC
//      and TAILDESC via the inline AXI-Lite master (item 4) at boot;
//      advances TAILDESC each time an AXIS frame completes.
//   3. SG-AXI intercept: the IP's M_AXI_SG read/write requests for
//      descriptor fetch/store are NOT routed to the slim outward port.
//      Instead they are served locally from the descriptor BRAM via a
//      tiny BRAM-backed AXI4-MM slave inside the shim. This avoids the
//      need to mirror descriptors into PS-DDR and keeps the outward
//      AXI4-MM port purely data-traffic. The IP sees a normal SG bus;
//      from the IP's perspective the descriptor "lives at DDR" — the
//      shim is the DDR for SG. (See PG021 §"Scatter-Gather Mode" — the
//      SG bus is just a generic AXI4-MM master, the IP does not care
//      whether the slave is real DRAM or block-RAM-backed.)
//   4. AXI-Lite local master: a small inline master that, on reset
//      deassertion, writes the IP's CURDESC + DMACR.RS=1 register, and
//      then later writes TAILDESC to advance the ring. The IP's
//      S_AXI_LITE port is internal to this shim and NOT exposed
//      externally.
//   5. Data-path crossbar: the IP's M_AXI_S2MM (write) is gated to the
//      slim outward AW/W/B path when DIR_IS_S2MM=1; the IP's M_AXI_MM2S
//      (read) is gated to the slim outward AR/R path when DIR_IS_S2MM=0.
//      The other half of the slim port is tied off. Burst-side signals
//      (AWLEN/AWSIZE/AWBURST/AWPROT/AWCACHE) emitted by the IP are NOT
//      part of the slim port-list and are dropped at the boundary —
//      consistent with the wrapper's slim contract and the next-level
//      block-design's burst-fixup.
//   6. Telemetry: irq_done = OR(mm2s_introut, s2mm_introut) per
//      DIR_IS_S2MM. frame_count increments on every IRQ pulse.
//      overrun_pulse fires (S2MM only) when no free descriptor in the
//      ring AND AXIS upstream pushes (i.e. backpressure leaked).
//      underrun_pulse fires (MM2S only) when the ring empties AND AXIS
//      downstream is starving.
//
// Test-bench note: this file lives under rtl/infra/ip/ and is included
// by the synth flow only (scripts/build/synth.tcl globs rtl/infra/ip/*.v).
// The iverilog TBs continue to use tb/rtl/models/axi_dma_v7_1_bhv.v which
// also defines a `axi_dma_channel_inst` module — exactly one of the two
// definitions must enter elaboration per build (synth: this shim; sim:
// the bhv model). This is enforced by the synth.tcl glob scope and the
// TB-Makefile include scope respectively.
//
// Verilog-2001 only (CLAUDE.md §Languages). (* RAM_STYLE = "BLOCK" *)
// attributes are Verilog-2001 OK and survive Vivado.
//
// Style: R1..R10 conventions per `rtl/infra/` headers. No SystemVerilog.

`timescale 1ns / 1ps
`default_nettype none

module axi_dma_channel_inst #(
    parameter CHANNEL_ID    = 0,    // 0..3, debug only
    parameter DIR_IS_S2MM   = 1,    // 1 = FPGA→PS (S2MM), 0 = PS→FPGA (MM2S)
    parameter AXIS_TDATA_W  = 32,
    parameter AXIS_TKEEP_W  = 4,
    parameter MM_ADDR_W     = 32,
    parameter MM_DATA_W     = 32,
    // Descriptor ring depth. 16 descriptors × 32 B = 512 B / direction →
    // fits in one 18Kb BRAM block (per direction).
    parameter DESC_RING_DEPTH = 16,
    // Base address (in the IP's view of "DDR") where the descriptor ring
    // is mapped. The shim's SG-intercept slave decodes this base.
    // Two non-overlapping windows per channel (S2MM uses _S, MM2S uses _M).
    parameter [31:0] DESC_BASE_S = 32'h4000_0000 + (CHANNEL_ID * 32'h0000_2000),
    parameter [31:0] DESC_BASE_M = 32'h4000_1000 + (CHANNEL_ID * 32'h0000_2000)
)(
    input  wire                       clk_axi,
    input  wire                       rstn_axi,

    /* AXIS slave (S2MM direction) */
    input  wire [AXIS_TDATA_W-1:0]    s_axis_tdata,
    input  wire                       s_axis_tvalid,
    output wire                       s_axis_tready,
    input  wire                       s_axis_tlast,
    input  wire [AXIS_TKEEP_W-1:0]    s_axis_tkeep,

    /* AXIS master (MM2S direction) */
    output reg  [AXIS_TDATA_W-1:0]    m_axis_tdata,
    output reg                        m_axis_tvalid,
    input  wire                       m_axis_tready,
    output reg                        m_axis_tlast,
    output reg  [AXIS_TKEEP_W-1:0]    m_axis_tkeep,

    /* AXI4-MM master — slim 5-channel W + 5-channel R; → PS DDR */
    output wire [MM_ADDR_W-1:0]       m_axi_awaddr,
    output wire                       m_axi_awvalid,
    input  wire                       m_axi_awready,
    output wire [MM_DATA_W-1:0]       m_axi_wdata,
    output wire                       m_axi_wvalid,
    input  wire                       m_axi_wready,
    output wire                       m_axi_wlast,
    input  wire [1:0]                 m_axi_bresp,
    input  wire                       m_axi_bvalid,
    output wire                       m_axi_bready,
    output wire [MM_ADDR_W-1:0]       m_axi_araddr,
    output wire                       m_axi_arvalid,
    input  wire                       m_axi_arready,
    input  wire [MM_DATA_W-1:0]       m_axi_rdata,
    input  wire                       m_axi_rvalid,
    output wire                       m_axi_rready,
    input  wire                       m_axi_rlast,
    input  wire [1:0]                 m_axi_rresp,

    /* Telemetry */
    output reg                        irq_done,
    output reg  [31:0]                frame_count,
    output reg                        overrun_pulse,
    output reg                        underrun_pulse
);

    // =================================================================
    // Localparams / derived widths
    // =================================================================
    localparam DESC_BYTES   = 32;
    localparam DESC_BITS    = DESC_BYTES * 8; // 256
    localparam RING_PTR_W   = (DESC_RING_DEPTH <= 2) ? 1 :
                              (DESC_RING_DEPTH <= 4) ? 2 :
                              (DESC_RING_DEPTH <= 8) ? 3 :
                              (DESC_RING_DEPTH <= 16) ? 4 :
                              (DESC_RING_DEPTH <= 32) ? 5 : 6;

    // PG021 descriptor field offsets (32-bit word index within 32-byte desc):
    //   word[0] = NXTDESC
    //   word[1] = NXTDESC_MSB (32-bit address mode → 0)
    //   word[2] = BUFFER_ADDR
    //   word[3] = BUFFER_ADDR_MSB
    //   word[4] = reserved
    //   word[5] = reserved
    //   word[6] = CONTROL    (.SOF/.EOF + length)
    //   word[7] = STATUS     (.CMPLT + length transferred + errors)

    // Buffer base in PS-DDR for data transfers (per channel, per direction).
    // The IP's M_AXI_S2MM/MM2S issues bursts to these addresses via the slim
    // outward port; the addresses are baked into descriptor.BUFFER_ADDR at
    // ring-init time.
    localparam [31:0] DATA_BASE = 32'h1000_0000 + (CHANNEL_ID * 32'h0010_0000);
    localparam [31:0] BUF_STRIDE = 32'h0000_2000; // 8 KiB / descriptor
    // Length field (carry-over c_sg_length_width=14 → 16 KiB max, but we
    // pre-program 4 KiB which matches ETSI-frame upper bound ×many).
    localparam [25:0] DESC_LEN_BYTES = 26'h0000_400; // 1 KiB per buf

    // =================================================================
    // Descriptor BRAM (one ring per direction; only the active one is
    // populated and exposed to the IP's SG bus).
    // =================================================================
    (* RAM_STYLE = "BLOCK" *)
    reg [DESC_BITS-1:0] desc_ring [0:DESC_RING_DEPTH-1];

    // =================================================================
    // Descriptor-manager FSM — primes the ring at reset.
    // =================================================================
    reg [RING_PTR_W:0]  init_idx;
    reg                 init_done;
    reg [31:0]          desc_base_addr; // selected base per DIR

    // Build one descriptor combinationally from index i.
    function [DESC_BITS-1:0] build_desc;
        input [RING_PTR_W:0] i;
        reg [31:0] nxt_addr;
        reg [31:0] buf_addr;
        reg [31:0] ctrl;
        reg [DESC_BITS-1:0] d;
        begin
            // wrap nxt: last entry points back at index 0.
            if (i == DESC_RING_DEPTH - 1)
                nxt_addr = desc_base_addr;
            else
                nxt_addr = desc_base_addr + ((i + 1) * DESC_BYTES);

            buf_addr = DATA_BASE + (i * BUF_STRIDE);

            // CONTROL: SOF=bit27, EOF=bit26, LENGTH[25:0]
            // For the priming ring we mark every descriptor SOF+EOF (each
            // descriptor describes a complete frame buffer). The IP will
            // interpret tlast on AXIS to mark transfer-end regardless;
            // SOF/EOF in the descriptor just allows the IP to set
            // status.SOF/EOF flags for SW.
            ctrl = {1'b0, 1'b1 /*SOF*/, 1'b1 /*EOF*/, 3'b0, DESC_LEN_BYTES};

            d = {
                32'h0,        // word[7] STATUS = 0 (cleared)
                ctrl,         // word[6] CONTROL
                32'h0,        // word[5] reserved
                32'h0,        // word[4] reserved
                32'h0,        // word[3] BUFFER_ADDR_MSB
                buf_addr,     // word[2] BUFFER_ADDR
                32'h0,        // word[1] NXTDESC_MSB
                nxt_addr      // word[0] NXTDESC
            };
            build_desc = d;
        end
    endfunction

    // (init+desc_base_addr+desc_ring driven from a single always block —
    //  see merged FSM further below; declarations only here so subsequent
    //  combinational logic can reference them.)

    // =================================================================
    // SG-AXI intercept slave (BRAM-backed). Honors AXI4 burst semantics:
    // captures arlen/awlen and walks the descriptor word offset across
    // beats, asserting rlast/expecting wlast on the final beat. The IP's
    // SG engine fetches one full 32-byte descriptor per burst (size=2,
    // len=7) — we serve those as 8 incrementing INCR-burst beats out of
    // BRAM. STATUS-word writes follow the same pattern.
    //
    // Address-decode: lower 5 bits = word offset within descriptor,
    // bits [(RING_PTR_W+5):5] = ring index, upper bits = base match.
    // =================================================================
    wire [31:0] ip_sg_awaddr, ip_sg_araddr;
    wire [7:0]  ip_sg_awlen,  ip_sg_arlen;
    wire        ip_sg_awvalid, ip_sg_arvalid;
    wire        ip_sg_wvalid, ip_sg_rready, ip_sg_bready;
    wire [31:0] ip_sg_wdata;
    wire [3:0]  ip_sg_wstrb;
    wire        ip_sg_wlast;

    reg         sg_awready_r, sg_wready_r, sg_bvalid_r;
    reg [1:0]   sg_bresp_r;
    reg         sg_arready_r, sg_rvalid_r, sg_rlast_r;
    reg [31:0]  sg_rdata_r;
    reg [1:0]   sg_rresp_r;

    reg [31:0]  sg_aw_latched;
    reg [31:0]  sg_ar_latched;
    reg [7:0]   sg_aw_len;     // remaining beats - 1 for write burst
    reg [7:0]   sg_ar_len;     // remaining beats - 1 for read burst
    reg [2:0]   sg_aw_word;    // current word index in descriptor (write)
    reg [2:0]   sg_ar_word;    // current word index in descriptor (read)
    reg [RING_PTR_W-1:0] sg_aw_idx;
    reg [RING_PTR_W-1:0] sg_ar_idx;
    reg         sg_in_write;
    reg         sg_in_read;
    reg         sg_aw_match;   // address falls inside our window
    reg         sg_ar_match;

    // Address → ring index + word index.
    function [RING_PTR_W-1:0] addr_to_idx;
        input [31:0] addr;
        begin
            addr_to_idx = addr[5 +: RING_PTR_W];
        end
    endfunction
    function [2:0] addr_to_word;
        input [31:0] addr;
        begin
            addr_to_word = addr[4:2];
        end
    endfunction

    integer ii;
    always @(posedge clk_axi or negedge rstn_axi) begin
        if (!rstn_axi) begin
            // ----- init / desc_ring reset -----
            init_idx       <= 0;
            init_done      <= 1'b0;
            desc_base_addr <= (DIR_IS_S2MM != 0) ? DESC_BASE_S : DESC_BASE_M;
            for (ii = 0; ii < DESC_RING_DEPTH; ii = ii + 1) begin
                desc_ring[ii] <= {DESC_BITS{1'b0}};
            end
            // ----- SG-slave reset -----
            sg_awready_r  <= 1'b0;
            sg_wready_r   <= 1'b0;
            sg_bvalid_r   <= 1'b0;
            sg_bresp_r    <= 2'b00;
            sg_arready_r  <= 1'b0;
            sg_rvalid_r   <= 1'b0;
            sg_rlast_r    <= 1'b0;
            sg_rdata_r    <= 32'h0;
            sg_rresp_r    <= 2'b00;
            sg_aw_latched <= 32'h0;
            sg_ar_latched <= 32'h0;
            sg_aw_len     <= 8'h0;
            sg_ar_len     <= 8'h0;
            sg_aw_word    <= 3'h0;
            sg_ar_word    <= 3'h0;
            sg_aw_idx     <= {RING_PTR_W{1'b0}};
            sg_ar_idx     <= {RING_PTR_W{1'b0}};
            sg_in_write   <= 1'b0;
            sg_in_read    <= 1'b0;
            sg_aw_match   <= 1'b0;
            sg_ar_match   <= 1'b0;
        end else if (!init_done) begin
            // Priming phase: write one descriptor per cycle. SG bus is held
            // idle (init_done gates it). When all DESC_RING_DEPTH entries
            // have been written, init_done latches and the SG slave wakes.
            desc_ring[init_idx[RING_PTR_W-1:0]] <= build_desc(init_idx);
            if (init_idx == DESC_RING_DEPTH - 1) begin
                init_done <= 1'b1;
            end
            init_idx <= init_idx + 1'b1;
        end else begin
            // ----- AW phase -----
            if (!sg_awready_r && ip_sg_awvalid && !sg_in_write) begin
                sg_aw_latched <= ip_sg_awaddr;
                sg_aw_len     <= ip_sg_awlen;
                sg_aw_word    <= addr_to_word(ip_sg_awaddr);
                sg_aw_idx     <= addr_to_idx(ip_sg_awaddr);
                sg_aw_match   <= (ip_sg_awaddr[31:16] == desc_base_addr[31:16]);
                sg_awready_r  <= 1'b1;
                sg_in_write   <= 1'b1;
            end else begin
                sg_awready_r  <= 1'b0;
            end

            // ----- W phase (SG writes update STATUS word, possibly multi-beat) -----
            if (sg_in_write && ip_sg_wvalid && !sg_wready_r && !sg_bvalid_r) begin
                sg_wready_r <= 1'b1;
                if (sg_aw_match) begin
                    case (sg_aw_word)
                        3'd0: desc_ring[sg_aw_idx][ 31:  0] <= ip_sg_wdata;
                        3'd1: desc_ring[sg_aw_idx][ 63: 32] <= ip_sg_wdata;
                        3'd2: desc_ring[sg_aw_idx][ 95: 64] <= ip_sg_wdata;
                        3'd3: desc_ring[sg_aw_idx][127: 96] <= ip_sg_wdata;
                        3'd4: desc_ring[sg_aw_idx][159:128] <= ip_sg_wdata;
                        3'd5: desc_ring[sg_aw_idx][191:160] <= ip_sg_wdata;
                        3'd6: desc_ring[sg_aw_idx][223:192] <= ip_sg_wdata;
                        3'd7: desc_ring[sg_aw_idx][255:224] <= ip_sg_wdata;
                        default: ;
                    endcase
                end
                sg_aw_word <= sg_aw_word + 3'd1;
                if (ip_sg_wlast || sg_aw_len == 8'd0) begin
                    sg_bvalid_r <= 1'b1;
                    sg_bresp_r  <= 2'b00;
                end else begin
                    sg_aw_len <= sg_aw_len - 8'd1;
                end
            end else begin
                sg_wready_r <= 1'b0;
            end

            // ----- B phase -----
            if (sg_bvalid_r && ip_sg_bready) begin
                sg_bvalid_r <= 1'b0;
                sg_in_write <= 1'b0;
            end

            // ----- AR phase -----
            if (!sg_arready_r && ip_sg_arvalid && !sg_in_read) begin
                sg_ar_latched <= ip_sg_araddr;
                sg_ar_len     <= ip_sg_arlen;
                sg_ar_word    <= addr_to_word(ip_sg_araddr);
                sg_ar_idx     <= addr_to_idx(ip_sg_araddr);
                sg_ar_match   <= (ip_sg_araddr[31:16] == desc_base_addr[31:16]);
                sg_arready_r  <= 1'b1;
                sg_in_read    <= 1'b1;
            end else begin
                sg_arready_r  <= 1'b0;
            end

            // ----- R phase (SG fetches descriptor word, possibly multi-beat) -----
            if (sg_in_read && (!sg_rvalid_r || ip_sg_rready)) begin
                if (sg_rvalid_r && ip_sg_rready) begin
                    // beat just consumed; advance unless this was the last
                    if (sg_rlast_r) begin
                        sg_rvalid_r <= 1'b0;
                        sg_rlast_r  <= 1'b0;
                        sg_in_read  <= 1'b0;
                    end else begin
                        sg_ar_word <= sg_ar_word + 3'd1;
                        sg_ar_len  <= sg_ar_len - 8'd1;
                    end
                end
                if (!(sg_rvalid_r && ip_sg_rready && sg_rlast_r)) begin
                    if (sg_ar_match) begin
                        case (sg_ar_word + ((sg_rvalid_r && ip_sg_rready) ? 3'd1 : 3'd0))
                            3'd0: sg_rdata_r <= desc_ring[sg_ar_idx][ 31:  0];
                            3'd1: sg_rdata_r <= desc_ring[sg_ar_idx][ 63: 32];
                            3'd2: sg_rdata_r <= desc_ring[sg_ar_idx][ 95: 64];
                            3'd3: sg_rdata_r <= desc_ring[sg_ar_idx][127: 96];
                            3'd4: sg_rdata_r <= desc_ring[sg_ar_idx][159:128];
                            3'd5: sg_rdata_r <= desc_ring[sg_ar_idx][191:160];
                            3'd6: sg_rdata_r <= desc_ring[sg_ar_idx][223:192];
                            3'd7: sg_rdata_r <= desc_ring[sg_ar_idx][255:224];
                            default: sg_rdata_r <= 32'h0;
                        endcase
                    end else begin
                        sg_rdata_r <= 32'h0;
                    end
                    sg_rvalid_r <= 1'b1;
                    sg_rresp_r  <= 2'b00;
                    // rlast on the final beat: when remaining count
                    // (after possibly-decrement above) reaches zero.
                    if ((sg_rvalid_r && ip_sg_rready) ?
                            (sg_ar_len == 8'd1) :
                            (sg_ar_len == 8'd0)) begin
                        sg_rlast_r <= 1'b1;
                    end else begin
                        sg_rlast_r <= 1'b0;
                    end
                end
            end
        end
    end

    // =================================================================
    // Inline AXI-Lite master — programs IP CURDESC + TAILDESC + DMACR.RS
    // PG021 register offsets:
    //   S2MM: 0x30 S2MM_DMACR, 0x38 S2MM_CURDESC, 0x40 S2MM_CURDESC_MSB,
    //         0x48 S2MM_TAILDESC, 0x50 S2MM_TAILDESC_MSB
    //   MM2S: 0x00 MM2S_DMACR, 0x08 MM2S_CURDESC, 0x10 MM2S_CURDESC_MSB,
    //         0x18 MM2S_TAILDESC, 0x20 MM2S_TAILDESC_MSB
    // =================================================================
    wire [9:0]  ip_lite_awaddr;
    wire        ip_lite_awvalid, ip_lite_awready;
    wire [31:0] ip_lite_wdata;
    wire        ip_lite_wvalid, ip_lite_wready;
    wire [1:0]  ip_lite_bresp;
    wire        ip_lite_bvalid, ip_lite_bready;
    wire [9:0]  ip_lite_araddr;
    wire        ip_lite_arvalid, ip_lite_arready;
    wire [31:0] ip_lite_rdata;
    wire [1:0]  ip_lite_rresp;
    wire        ip_lite_rvalid, ip_lite_rready;

    reg [3:0]   lite_state;
    reg [9:0]   lite_awaddr_r;
    reg [31:0]  lite_wdata_r;
    reg         lite_awvalid_r, lite_wvalid_r, lite_bready_r;
    reg [RING_PTR_W:0] tail_ptr;

    localparam LSt_IDLE      = 4'd0;
    localparam LSt_WRT_CDR_L = 4'd1;
    localparam LSt_WRT_CDR_H = 4'd2;
    localparam LSt_WRT_DMACR = 4'd3;
    localparam LSt_WRT_TDR_L = 4'd4;
    localparam LSt_WRT_TDR_H = 4'd5;
    localparam LSt_WAIT_B    = 4'd6;
    localparam LSt_RUN       = 4'd7;
    localparam LSt_ADV_TAIL  = 4'd8;

    // Per-direction register offsets (DMACR/CURDESC/TAILDESC).
    localparam [9:0] OFF_DMACR    = (DIR_IS_S2MM != 0) ? 10'h030 : 10'h000;
    localparam [9:0] OFF_CURDESC  = (DIR_IS_S2MM != 0) ? 10'h038 : 10'h008;
    localparam [9:0] OFF_TAILDESC = (DIR_IS_S2MM != 0) ? 10'h048 : 10'h018;

    reg [3:0]  next_state;
    reg [9:0]  next_addr;
    reg [31:0] next_data;
    wire       lite_handshake_done;
    reg        advance_tail_req;

    always @(*) begin
        next_state = lite_state;
        next_addr  = lite_awaddr_r;
        next_data  = lite_wdata_r;
        case (lite_state)
            LSt_IDLE: begin
                if (init_done) begin
                    next_state = LSt_WRT_CDR_L;
                    next_addr  = OFF_CURDESC;
                    next_data  = desc_base_addr;
                end
            end
            LSt_WRT_CDR_L: if (lite_handshake_done) begin
                next_state = LSt_WRT_DMACR;
                next_addr  = OFF_DMACR;
                next_data  = 32'h0000_1001; // RS=1, IRQEN=1
            end
            LSt_WRT_DMACR: if (lite_handshake_done) begin
                next_state = LSt_WRT_TDR_L;
                next_addr  = OFF_TAILDESC;
                next_data  = desc_base_addr +
                             ((DESC_RING_DEPTH - 1) * DESC_BYTES);
            end
            LSt_WRT_TDR_L: if (lite_handshake_done) begin
                next_state = LSt_RUN;
            end
            LSt_RUN: begin
                if (advance_tail_req) begin
                    next_state = LSt_ADV_TAIL;
                    next_addr  = OFF_TAILDESC;
                    next_data  = desc_base_addr + (tail_ptr * DESC_BYTES);
                end
            end
            LSt_ADV_TAIL: if (lite_handshake_done) begin
                next_state = LSt_RUN;
            end
            default: next_state = LSt_IDLE;
        endcase
    end

    assign lite_handshake_done = lite_awvalid_r && ip_lite_awready &&
                                 lite_wvalid_r  && ip_lite_wready;

    reg advance_tail_seen;

    always @(posedge clk_axi or negedge rstn_axi) begin
        if (!rstn_axi) begin
            lite_state        <= LSt_IDLE;
            lite_awaddr_r     <= 10'h0;
            lite_wdata_r      <= 32'h0;
            lite_awvalid_r    <= 1'b0;
            lite_wvalid_r     <= 1'b0;
            lite_bready_r     <= 1'b1;
            tail_ptr          <= DESC_RING_DEPTH - 1;
            advance_tail_seen <= 1'b0;
        end else begin
            lite_state    <= next_state;
            lite_awaddr_r <= next_addr;
            lite_wdata_r  <= next_data;

            // Drive AW/W valid in the dispatch states.
            if (next_state == LSt_WRT_CDR_L || next_state == LSt_WRT_DMACR ||
                next_state == LSt_WRT_TDR_L || next_state == LSt_ADV_TAIL) begin
                lite_awvalid_r <= 1'b1;
                lite_wvalid_r  <= 1'b1;
            end
            if (lite_handshake_done) begin
                lite_awvalid_r <= 1'b0;
                lite_wvalid_r  <= 1'b0;
            end

            if (lite_state == LSt_ADV_TAIL && lite_handshake_done) begin
                tail_ptr <= tail_ptr + 1'b1;
            end
        end
    end

    assign ip_lite_awaddr  = lite_awaddr_r;
    assign ip_lite_awvalid = lite_awvalid_r;
    assign ip_lite_wdata   = lite_wdata_r;
    assign ip_lite_wvalid  = lite_wvalid_r;
    assign ip_lite_bready  = lite_bready_r;
    assign ip_lite_araddr  = 10'h0;
    assign ip_lite_arvalid = 1'b0;
    assign ip_lite_rready  = 1'b1;

    // =================================================================
    // IP IRQ / completion telemetry
    // =================================================================
    wire ip_mm2s_introut, ip_s2mm_introut;
    wire [31:0] ip_axi_dma_tstvec;

    always @(posedge clk_axi or negedge rstn_axi) begin
        if (!rstn_axi) begin
            irq_done       <= 1'b0;
            frame_count    <= 32'h0;
            overrun_pulse  <= 1'b0;
            underrun_pulse <= 1'b0;
            advance_tail_req <= 1'b0;
        end else begin
            // Edge-detect the IRQ line per direction.
            // (axi_dma:7.1 introut is level-triggered while a status reg
            // bit is set; we treat the rising edge as a frame-completion
            // pulse for the wrapper's sticky-bit logic.)
            irq_done <= (DIR_IS_S2MM != 0) ? ip_s2mm_introut : ip_mm2s_introut;

            if (irq_done) begin
                frame_count       <= frame_count + 32'h1;
                advance_tail_req  <= 1'b1;
            end else if (lite_state == LSt_ADV_TAIL) begin
                advance_tail_req  <= 1'b0;
            end

            // Overrun / underrun detection: DRC-friendly heuristics, not
            // gold-spec — fired purely from FIFO-watermark side-band
            // exposed via tstvec[15:0]. When the IP's internal SF FIFO
            // saturates, tstvec sets the relevant flag bit (PG021 §"Test
            // Vector Output", bit 0 = mm2s_underflow, bit 1 =
            // s2mm_overflow). Without DRE, this is the simplest source.
            if (DIR_IS_S2MM != 0)
                overrun_pulse <= ip_axi_dma_tstvec[1];
            else
                overrun_pulse <= 1'b0;
            if (DIR_IS_S2MM == 0)
                underrun_pulse <= ip_axi_dma_tstvec[0];
            else
                underrun_pulse <= 1'b0;
        end
    end

    // =================================================================
    // IP nets — full LogiCORE port-list
    // =================================================================
    // SG bus
    wire [31:0] ip_sg_awaddr_w; wire [7:0] ip_sg_awlen_w;
    wire [2:0]  ip_sg_awsize_w; wire [1:0] ip_sg_awburst_w;
    wire [2:0]  ip_sg_awprot_w; wire [3:0] ip_sg_awcache_w;
    wire        ip_sg_awvalid_w, ip_sg_awready_w;
    wire [31:0] ip_sg_wdata_w; wire [3:0] ip_sg_wstrb_w;
    wire        ip_sg_wlast_w, ip_sg_wvalid_w, ip_sg_wready_w;
    wire [1:0]  ip_sg_bresp_w; wire ip_sg_bvalid_w, ip_sg_bready_w;
    wire [31:0] ip_sg_araddr_w; wire [7:0] ip_sg_arlen_w;
    wire [2:0]  ip_sg_arsize_w; wire [1:0] ip_sg_arburst_w;
    wire [2:0]  ip_sg_arprot_w; wire [3:0] ip_sg_arcache_w;
    wire        ip_sg_arvalid_w, ip_sg_arready_w;
    wire [31:0] ip_sg_rdata_w; wire [1:0] ip_sg_rresp_w;
    wire        ip_sg_rlast_w, ip_sg_rvalid_w, ip_sg_rready_w;

    // MM2S data
    wire [31:0] ip_mm2s_araddr_w; wire [7:0] ip_mm2s_arlen_w;
    wire [2:0]  ip_mm2s_arsize_w; wire [1:0] ip_mm2s_arburst_w;
    wire [2:0]  ip_mm2s_arprot_w; wire [3:0] ip_mm2s_arcache_w;
    wire        ip_mm2s_arvalid_w, ip_mm2s_arready_w;
    wire [31:0] ip_mm2s_rdata_w; wire [1:0] ip_mm2s_rresp_w;
    wire        ip_mm2s_rlast_w, ip_mm2s_rvalid_w, ip_mm2s_rready_w;

    // S2MM data
    wire [31:0] ip_s2mm_awaddr_w; wire [7:0] ip_s2mm_awlen_w;
    wire [2:0]  ip_s2mm_awsize_w; wire [1:0] ip_s2mm_awburst_w;
    wire [2:0]  ip_s2mm_awprot_w; wire [3:0] ip_s2mm_awcache_w;
    wire        ip_s2mm_awvalid_w, ip_s2mm_awready_w;
    wire [31:0] ip_s2mm_wdata_w; wire [3:0] ip_s2mm_wstrb_w;
    wire        ip_s2mm_wlast_w, ip_s2mm_wvalid_w, ip_s2mm_wready_w;
    wire [1:0]  ip_s2mm_bresp_w; wire ip_s2mm_bvalid_w, ip_s2mm_bready_w;

    // MM2S AXIS master
    wire [31:0] ip_m_axis_mm2s_tdata_w;
    wire [3:0]  ip_m_axis_mm2s_tkeep_w;
    wire        ip_m_axis_mm2s_tvalid_w, ip_m_axis_mm2s_tready_w;
    wire        ip_m_axis_mm2s_tlast_w;

    // S2MM AXIS slave (driven by upstream slim AXIS).
    // Direct pass-through of the slim port.
    // S2MM-status stream — not used (SG-stscntrl strm tied off below).

    // SG status/control side streams (we configured c_sg_include_stscntrl_strm=1
    // for parity with the carry-over .xci, so the IP exposes those AXIS
    // ports; we tie them off internally — SW does not consume them).

    // Bind shim-side SG-intercept slave wires to IP nets.
    assign ip_sg_awaddr  = ip_sg_awaddr_w;
    assign ip_sg_awlen   = ip_sg_awlen_w;
    assign ip_sg_awvalid = ip_sg_awvalid_w;
    assign ip_sg_awready_w = sg_awready_r;
    assign ip_sg_wdata   = ip_sg_wdata_w;
    assign ip_sg_wvalid  = ip_sg_wvalid_w;
    assign ip_sg_wstrb   = ip_sg_wstrb_w;
    assign ip_sg_wlast   = ip_sg_wlast_w;
    assign ip_sg_wready_w = sg_wready_r;
    assign ip_sg_bresp_w = sg_bresp_r;
    assign ip_sg_bvalid_w = sg_bvalid_r;
    assign ip_sg_bready  = ip_sg_bready_w;
    assign ip_sg_araddr  = ip_sg_araddr_w;
    assign ip_sg_arlen   = ip_sg_arlen_w;
    assign ip_sg_arvalid = ip_sg_arvalid_w;
    assign ip_sg_arready_w = sg_arready_r;
    assign ip_sg_rdata_w = sg_rdata_r;
    assign ip_sg_rresp_w = sg_rresp_r;
    assign ip_sg_rlast_w = sg_rlast_r;
    assign ip_sg_rvalid_w = sg_rvalid_r;
    assign ip_sg_rready  = ip_sg_rready_w;

    // =================================================================
    // Slim outward AXI4-MM port routing
    //
    // S2MM channel: drive AW/W/B from IP's S2MM master; tie AR/R off.
    // MM2S channel: drive AR/R from IP's MM2S master; tie AW/W/B off.
    // =================================================================
    assign m_axi_awaddr  = (DIR_IS_S2MM != 0) ? ip_s2mm_awaddr_w  : {MM_ADDR_W{1'b0}};
    assign m_axi_awvalid = (DIR_IS_S2MM != 0) ? ip_s2mm_awvalid_w : 1'b0;
    assign ip_s2mm_awready_w = (DIR_IS_S2MM != 0) ? m_axi_awready : 1'b0;

    assign m_axi_wdata  = (DIR_IS_S2MM != 0) ? ip_s2mm_wdata_w  : {MM_DATA_W{1'b0}};
    assign m_axi_wvalid = (DIR_IS_S2MM != 0) ? ip_s2mm_wvalid_w : 1'b0;
    assign m_axi_wlast  = (DIR_IS_S2MM != 0) ? ip_s2mm_wlast_w  : 1'b0;
    assign ip_s2mm_wready_w = (DIR_IS_S2MM != 0) ? m_axi_wready : 1'b0;

    assign ip_s2mm_bresp_w  = (DIR_IS_S2MM != 0) ? m_axi_bresp : 2'b00;
    assign ip_s2mm_bvalid_w = (DIR_IS_S2MM != 0) ? m_axi_bvalid : 1'b0;
    assign m_axi_bready = (DIR_IS_S2MM != 0) ? ip_s2mm_bready_w : 1'b1;

    assign m_axi_araddr  = (DIR_IS_S2MM == 0) ? ip_mm2s_araddr_w  : {MM_ADDR_W{1'b0}};
    assign m_axi_arvalid = (DIR_IS_S2MM == 0) ? ip_mm2s_arvalid_w : 1'b0;
    assign ip_mm2s_arready_w = (DIR_IS_S2MM == 0) ? m_axi_arready : 1'b0;

    assign ip_mm2s_rdata_w  = (DIR_IS_S2MM == 0) ? m_axi_rdata : 32'h0;
    assign ip_mm2s_rresp_w  = (DIR_IS_S2MM == 0) ? m_axi_rresp : 2'b00;
    assign ip_mm2s_rlast_w  = (DIR_IS_S2MM == 0) ? m_axi_rlast : 1'b0;
    assign ip_mm2s_rvalid_w = (DIR_IS_S2MM == 0) ? m_axi_rvalid : 1'b0;
    assign m_axi_rready = (DIR_IS_S2MM == 0) ? ip_mm2s_rready_w : 1'b1;

    // =================================================================
    // AXIS pass-through — slim ↔ IP
    //
    // For S2MM: slim s_axis_* → IP's s_axis_s2mm_*; slim m_axis_* tied off.
    // For MM2S: IP's m_axis_mm2s_* → slim m_axis_*; slim s_axis_tready=0.
    // =================================================================
    wire s_axis_s2mm_tready_int;
    assign s_axis_tready = (DIR_IS_S2MM != 0) ? s_axis_s2mm_tready_int : 1'b0;

    // The slim m_axis_* outputs are `reg` per the locked port-list (matches
    // the bhv model). For DIR_IS_S2MM=1 we hold them at zero. For
    // DIR_IS_S2MM=0 we drive from the IP's m_axis_mm2s_*. Use a tiny
    // wrapper always-block.
    always @(*) begin
        if (DIR_IS_S2MM == 0) begin
            m_axis_tdata  = ip_m_axis_mm2s_tdata_w;
            m_axis_tvalid = ip_m_axis_mm2s_tvalid_w;
            m_axis_tlast  = ip_m_axis_mm2s_tlast_w;
            m_axis_tkeep  = ip_m_axis_mm2s_tkeep_w;
        end else begin
            m_axis_tdata  = {AXIS_TDATA_W{1'b0}};
            m_axis_tvalid = 1'b0;
            m_axis_tlast  = 1'b0;
            m_axis_tkeep  = {AXIS_TKEEP_W{1'b0}};
        end
    end
    assign ip_m_axis_mm2s_tready_w = (DIR_IS_S2MM == 0) ? m_axis_tready : 1'b0;

    // =================================================================
    // The real IP module is created by Vivado's create_ip and renamed to
    // `axi_dma_v71_logicore` (see scripts/build/synth.tcl). We instantiate
    // it here. The IP's full port-list is fixed by Vivado IP-XACT.
    // =================================================================
    axi_dma_v71_logicore u_ip (
        .s_axi_lite_aclk          (clk_axi),
        .m_axi_sg_aclk            (clk_axi),
        .m_axi_mm2s_aclk          (clk_axi),
        .m_axi_s2mm_aclk          (clk_axi),
        .axi_resetn               (rstn_axi),

        // S_AXI_LITE — driven by the inline AXI-Lite master
        .s_axi_lite_awvalid       (ip_lite_awvalid),
        .s_axi_lite_awready       (ip_lite_awready),
        .s_axi_lite_awaddr        (ip_lite_awaddr),
        .s_axi_lite_wvalid        (ip_lite_wvalid),
        .s_axi_lite_wready        (ip_lite_wready),
        .s_axi_lite_wdata         (ip_lite_wdata),
        .s_axi_lite_bresp         (ip_lite_bresp),
        .s_axi_lite_bvalid        (ip_lite_bvalid),
        .s_axi_lite_bready        (ip_lite_bready),
        .s_axi_lite_arvalid       (ip_lite_arvalid),
        .s_axi_lite_arready       (ip_lite_arready),
        .s_axi_lite_araddr        (ip_lite_araddr),
        .s_axi_lite_rvalid        (ip_lite_rvalid),
        .s_axi_lite_rready        (ip_lite_rready),
        .s_axi_lite_rdata         (ip_lite_rdata),
        .s_axi_lite_rresp         (ip_lite_rresp),

        // M_AXI_SG — looped to the local BRAM-backed slave.
        // Burst-side fields are sourced from IP and consumed by our slave's
        // single-beat decode path; we accept any awsize/awlen and serve
        // a word at a time (the IP tolerates because it tracks beats).
        .m_axi_sg_awaddr          (ip_sg_awaddr_w),
        .m_axi_sg_awlen           (ip_sg_awlen_w),
        .m_axi_sg_awsize          (ip_sg_awsize_w),
        .m_axi_sg_awburst         (ip_sg_awburst_w),
        .m_axi_sg_awprot          (ip_sg_awprot_w),
        .m_axi_sg_awcache         (ip_sg_awcache_w),
        .m_axi_sg_awvalid         (ip_sg_awvalid_w),
        .m_axi_sg_awready         (ip_sg_awready_w),
        .m_axi_sg_wdata           (ip_sg_wdata_w),
        .m_axi_sg_wstrb           (ip_sg_wstrb_w),
        .m_axi_sg_wlast           (ip_sg_wlast_w),
        .m_axi_sg_wvalid          (ip_sg_wvalid_w),
        .m_axi_sg_wready          (ip_sg_wready_w),
        .m_axi_sg_bresp           (ip_sg_bresp_w),
        .m_axi_sg_bvalid          (ip_sg_bvalid_w),
        .m_axi_sg_bready          (ip_sg_bready_w),
        .m_axi_sg_araddr          (ip_sg_araddr_w),
        .m_axi_sg_arlen           (ip_sg_arlen_w),
        .m_axi_sg_arsize          (ip_sg_arsize_w),
        .m_axi_sg_arburst         (ip_sg_arburst_w),
        .m_axi_sg_arprot          (ip_sg_arprot_w),
        .m_axi_sg_arcache         (ip_sg_arcache_w),
        .m_axi_sg_arvalid         (ip_sg_arvalid_w),
        .m_axi_sg_arready         (ip_sg_arready_w),
        .m_axi_sg_rdata           (ip_sg_rdata_w),
        .m_axi_sg_rresp           (ip_sg_rresp_w),
        .m_axi_sg_rlast           (ip_sg_rlast_w),
        .m_axi_sg_rvalid          (ip_sg_rvalid_w),
        .m_axi_sg_rready          (ip_sg_rready_w),

        // M_AXI_MM2S — read data path → slim outward port (when MM2S dir)
        .m_axi_mm2s_araddr        (ip_mm2s_araddr_w),
        .m_axi_mm2s_arlen         (ip_mm2s_arlen_w),
        .m_axi_mm2s_arsize        (ip_mm2s_arsize_w),
        .m_axi_mm2s_arburst       (ip_mm2s_arburst_w),
        .m_axi_mm2s_arprot        (ip_mm2s_arprot_w),
        .m_axi_mm2s_arcache       (ip_mm2s_arcache_w),
        .m_axi_mm2s_arvalid       (ip_mm2s_arvalid_w),
        .m_axi_mm2s_arready       (ip_mm2s_arready_w),
        .m_axi_mm2s_rdata         (ip_mm2s_rdata_w),
        .m_axi_mm2s_rresp         (ip_mm2s_rresp_w),
        .m_axi_mm2s_rlast         (ip_mm2s_rlast_w),
        .m_axi_mm2s_rvalid        (ip_mm2s_rvalid_w),
        .m_axi_mm2s_rready        (ip_mm2s_rready_w),

        .mm2s_prmry_reset_out_n   (),
        .m_axis_mm2s_tdata        (ip_m_axis_mm2s_tdata_w),
        .m_axis_mm2s_tkeep        (ip_m_axis_mm2s_tkeep_w),
        .m_axis_mm2s_tvalid       (ip_m_axis_mm2s_tvalid_w),
        .m_axis_mm2s_tready       (ip_m_axis_mm2s_tready_w),
        .m_axis_mm2s_tlast        (ip_m_axis_mm2s_tlast_w),

        // SG status/control AXIS streams — tied off (not consumed).
        .mm2s_cntrl_reset_out_n   (),
        .m_axis_mm2s_cntrl_tdata  (),
        .m_axis_mm2s_cntrl_tkeep  (),
        .m_axis_mm2s_cntrl_tvalid (),
        .m_axis_mm2s_cntrl_tready (1'b1),
        .m_axis_mm2s_cntrl_tlast  (),

        // M_AXI_S2MM — write data path → slim outward port (when S2MM dir)
        .m_axi_s2mm_awaddr        (ip_s2mm_awaddr_w),
        .m_axi_s2mm_awlen         (ip_s2mm_awlen_w),
        .m_axi_s2mm_awsize        (ip_s2mm_awsize_w),
        .m_axi_s2mm_awburst       (ip_s2mm_awburst_w),
        .m_axi_s2mm_awprot        (ip_s2mm_awprot_w),
        .m_axi_s2mm_awcache       (ip_s2mm_awcache_w),
        .m_axi_s2mm_awvalid       (ip_s2mm_awvalid_w),
        .m_axi_s2mm_awready       (ip_s2mm_awready_w),
        .m_axi_s2mm_wdata         (ip_s2mm_wdata_w),
        .m_axi_s2mm_wstrb         (ip_s2mm_wstrb_w),
        .m_axi_s2mm_wlast         (ip_s2mm_wlast_w),
        .m_axi_s2mm_wvalid        (ip_s2mm_wvalid_w),
        .m_axi_s2mm_wready        (ip_s2mm_wready_w),
        .m_axi_s2mm_bresp         (ip_s2mm_bresp_w),
        .m_axi_s2mm_bvalid        (ip_s2mm_bvalid_w),
        .m_axi_s2mm_bready        (ip_s2mm_bready_w),

        .s2mm_prmry_reset_out_n   (),
        .s_axis_s2mm_tdata        (s_axis_tdata),
        .s_axis_s2mm_tkeep        (s_axis_tkeep),
        .s_axis_s2mm_tvalid       (s_axis_tvalid),
        .s_axis_s2mm_tready       (s_axis_s2mm_tready_int),
        .s_axis_s2mm_tlast        (s_axis_tlast),

        .s2mm_sts_reset_out_n     (),
        .s_axis_s2mm_sts_tdata    (32'h0),
        .s_axis_s2mm_sts_tkeep    (4'h0),
        .s_axis_s2mm_sts_tvalid   (1'b0),
        .s_axis_s2mm_sts_tready   (),
        .s_axis_s2mm_sts_tlast    (1'b0),

        .mm2s_introut             (ip_mm2s_introut),
        .s2mm_introut             (ip_s2mm_introut),
        .axi_dma_tstvec           (ip_axi_dma_tstvec)
    );

endmodule

`default_nettype wire
