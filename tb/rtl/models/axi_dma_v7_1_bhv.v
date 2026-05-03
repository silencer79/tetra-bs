// tb/rtl/models/axi_dma_v7_1_bhv.v
//
// Owned by Agent A1 (A1-fpga-axi-dma).
//
// Behavioural model of a single Xilinx LogiCORE `axi_dma_v7_1` channel,
// for use in iverilog test benches. Models ONLY the AXIS↔DDR data-path
// (MM2S read-out and S2MM write-in) plus a frame-completion IRQ pulse;
// scatter-gather descriptor handling and AXI-Lite control register
// emulation are out of scope (those live in the userspace libaxidma
// glue + the real IP, see `tb/rtl/models/README.md`).
//
// The model exposes the same port-list as the production
// `axi_dma_channel_inst` shim (a thin wrapper around the real
// `axi_dma_v7_1` IP that is built from Vivado-IP-Tcl in the synth flow,
// not visible to iverilog). Production-vs-TB switching is done in
// `rtl/infra/tetra_axi_dma_wrapper.v` by file include path: tb files
// pull in this `.v`, synth uses the Vivado IP.
//
// Behavioural contract (per-direction):
//
//   S2MM (DIR_IS_S2MM=1, FPGA→DDR write):
//     - Always asserts s_axis_tready (consumes any back-pressure-free
//       AXIS stream) when rstn_axi is HIGH; asserts NOT-ready when in reset.
//     - Counts the bytes of each frame (delineated by tlast) using tkeep.
//     - On observing tlast: pulses irq_done (1 cycle), increments frame_count.
//     - Drives no AXI-MM master writes — TB models the DDR side as a
//       no-op (the write-data is dropped). This is sufficient for
//       framer + IRQ verification; the real IP would burst-write to DDR
//       at a configurable address/length/burst-size.
//     - Overrun: detected by the wrapper's enclosing logic (a real
//       overrun is "S2MM stalled because PS didn't refill descriptor
//       ring"). For the TB model we tie overrun_pulse=0; it can be
//       overridden by `force` from the TB if needed.
//
//   MM2S (DIR_IS_S2MM=0, DDR→FPGA read):
//     - Idle by default. The TB injects frames by calling the
//       `inject_frame` task (defined as a hierarchical reference target
//       below), which queues the frame into `inj_buf` and triggers
//       AXIS emission on m_axis.
//     - On finishing a frame, pulses irq_done and increments frame_count.
//     - Underrun: tied 0 unless the TB forces it.
//
// Pass/fail of the wrapper TB depends on these behaviours being
// deterministic. The TB drives `inject_frame` and observes the
// sequenced AXIS output.
//
// Non-behaviour stubbed ports:
//   - All AXI-MM master ports (m_axi_*) drive 0/floating; the TB does
//     NOT model DDR. This is sufficient for the wrapper TB which only
//     verifies AXIS handshake, byte/frame integrity, and IRQ pulse.
//
// Verilog-2001 only.

`timescale 1ns / 1ps
`default_nettype none

module axi_dma_channel_inst #(
    parameter CHANNEL_ID    = 0,
    parameter DIR_IS_S2MM   = 1,
    parameter AXIS_TDATA_W  = 32,
    parameter AXIS_TKEEP_W  = 4,
    parameter MM_ADDR_W     = 32,
    parameter MM_DATA_W     = 32,
    // TB injection buffer depth (in bytes per channel).
    parameter INJ_BUF_DEPTH = 4096
) (
    input  wire                       clk_axi,
    input  wire                       rstn_axi,

    // S2MM AXIS-slave (FPGA→DMA, only meaningful when DIR_IS_S2MM=1)
    input  wire [AXIS_TDATA_W-1:0]    s_axis_tdata,
    input  wire                       s_axis_tvalid,
    output wire                       s_axis_tready,
    input  wire                       s_axis_tlast,
    input  wire [AXIS_TKEEP_W-1:0]    s_axis_tkeep,

    // MM2S AXIS-master (DMA→FPGA, only meaningful when DIR_IS_S2MM=0)
    output reg  [AXIS_TDATA_W-1:0]    m_axis_tdata,
    output reg                        m_axis_tvalid,
    input  wire                       m_axis_tready,
    output reg                        m_axis_tlast,
    output reg  [AXIS_TKEEP_W-1:0]    m_axis_tkeep,

    // AXI-MM master — tied off / not modelled.
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

    // IRQ + telemetry
    output reg                        irq_done,
    output reg  [31:0]                frame_count,
    output reg                        overrun_pulse,
    output reg                        underrun_pulse
);

    // ----- AXI-MM tied off ------------------------------------------
    assign m_axi_awaddr  = {MM_ADDR_W{1'b0}};
    assign m_axi_awvalid = 1'b0;
    assign m_axi_wdata   = {MM_DATA_W{1'b0}};
    assign m_axi_wvalid  = 1'b0;
    assign m_axi_wlast   = 1'b0;
    assign m_axi_bready  = 1'b1;
    assign m_axi_araddr  = {MM_ADDR_W{1'b0}};
    assign m_axi_arvalid = 1'b0;
    assign m_axi_rready  = 1'b1;

    // -----------------------------------------------------------------
    // S2MM behavioural path (fabric → DDR write).
    // -----------------------------------------------------------------
    // Always-ready when not in reset. Count beat bytes (via tkeep).
    // Pulse irq_done on tlast.
    assign s_axis_tready = (DIR_IS_S2MM != 0) ? rstn_axi : 1'b0;

    // Capture the just-accepted frame's payload for TB verification (a
    // simple ring buffer accessible by the TB via hierarchical name).
    reg [7:0]  s2mm_capture [0:INJ_BUF_DEPTH-1];
    integer    s2mm_capture_len;
    integer    s2mm_capture_frames;

    // -----------------------------------------------------------------
    // MM2S behavioural path (TB → fabric AXIS-master emission).
    // The TB writes frames into the injection buffer via the
    // `inject_byte` / `inject_frame_done` tasks below. Each frame is
    // emitted as 32-bit beats with last-beat tkeep computed from
    // the byte count modulo 4.
    // -----------------------------------------------------------------
    reg [7:0]  mm2s_buf [0:INJ_BUF_DEPTH-1];
    integer    mm2s_wr_ptr;          // write pointer (TB)
    integer    mm2s_rd_ptr;          // read pointer (model)
    integer    mm2s_frame_end;       // index where current frame ends
    reg        mm2s_busy;            // currently emitting
    integer    mm2s_byte_idx;        // byte index within current beat (0..3)
    reg [31:0] mm2s_beat;            // beat under construction

    // -----------------------------------------------------------------
    // Single combined always block — both S2MM capture and MM2S
    // emission. Avoids multi-driver conflict on `irq_done` /
    // `frame_count` when one bhv-instance handles both directions
    // (DIR_IS_S2MM is a parameter, but we keep one block for clarity
    // and to give synthesisers a clean reset/datapath).
    // -----------------------------------------------------------------
    integer i;
    always @(posedge clk_axi or negedge rstn_axi) begin
        if (!rstn_axi) begin
            irq_done            <= 1'b0;
            frame_count         <= 32'd0;
            overrun_pulse       <= 1'b0;
            underrun_pulse      <= 1'b0;
            s2mm_capture_len    <= 0;
            s2mm_capture_frames <= 0;
            m_axis_tdata        <= {AXIS_TDATA_W{1'b0}};
            m_axis_tvalid       <= 1'b0;
            m_axis_tlast        <= 1'b0;
            m_axis_tkeep        <= {AXIS_TKEEP_W{1'b0}};
            mm2s_busy           <= 1'b0;
            mm2s_byte_idx       <= 0;
            mm2s_beat           <= 32'd0;
            for (i = 0; i < INJ_BUF_DEPTH; i = i + 1) begin
                s2mm_capture[i] <= 8'h00;
            end
        end else begin
            irq_done       <= 1'b0;
            overrun_pulse  <= 1'b0;
            underrun_pulse <= 1'b0;

            // -------- S2MM capture (only meaningful for S2MM dir) ----
            if ((DIR_IS_S2MM != 0) && s_axis_tvalid && s_axis_tready) begin
                // Capture each valid byte lane (MSB-first).
                if (s_axis_tkeep[3]) begin
                    if (s2mm_capture_len < INJ_BUF_DEPTH) begin
                        s2mm_capture[s2mm_capture_len] <= s_axis_tdata[31:24];
                    end
                end
                if (s_axis_tkeep[2]) begin
                    if (s2mm_capture_len + (s_axis_tkeep[3] ? 1 : 0) < INJ_BUF_DEPTH) begin
                        s2mm_capture[s2mm_capture_len + (s_axis_tkeep[3] ? 1 : 0)] <= s_axis_tdata[23:16];
                    end
                end
                if (s_axis_tkeep[1]) begin
                    if (s2mm_capture_len + (s_axis_tkeep[3] ? 1 : 0) + (s_axis_tkeep[2] ? 1 : 0) < INJ_BUF_DEPTH) begin
                        s2mm_capture[s2mm_capture_len + (s_axis_tkeep[3] ? 1 : 0) + (s_axis_tkeep[2] ? 1 : 0)] <= s_axis_tdata[15:8];
                    end
                end
                if (s_axis_tkeep[0]) begin
                    if (s2mm_capture_len + (s_axis_tkeep[3] ? 1 : 0) + (s_axis_tkeep[2] ? 1 : 0) + (s_axis_tkeep[1] ? 1 : 0) < INJ_BUF_DEPTH) begin
                        s2mm_capture[s2mm_capture_len + (s_axis_tkeep[3] ? 1 : 0) + (s_axis_tkeep[2] ? 1 : 0) + (s_axis_tkeep[1] ? 1 : 0)] <= s_axis_tdata[7:0];
                    end
                end
                // Track running byte length:
                //   add (popcount of tkeep) to s2mm_capture_len.
                s2mm_capture_len <= s2mm_capture_len +
                                    (s_axis_tkeep[3] ? 1 : 0) +
                                    (s_axis_tkeep[2] ? 1 : 0) +
                                    (s_axis_tkeep[1] ? 1 : 0) +
                                    (s_axis_tkeep[0] ? 1 : 0);
                // Frame complete on tlast → fire irq.
                if (s_axis_tlast) begin
                    irq_done             <= 1'b1;
                    frame_count          <= frame_count + 32'd1;
                    s2mm_capture_frames  <= s2mm_capture_frames + 1;
                end
            end

            // -------- MM2S emission (only meaningful for MM2S dir) ---
            if ((DIR_IS_S2MM == 0) && mm2s_busy) begin
                if (m_axis_tvalid && m_axis_tready) begin
                    if (mm2s_rd_ptr >= mm2s_frame_end) begin
                        // Frame finished.
                        m_axis_tvalid <= 1'b0;
                        m_axis_tlast  <= 1'b0;
                        m_axis_tkeep  <= {AXIS_TKEEP_W{1'b0}};
                        mm2s_busy     <= 1'b0;
                        irq_done      <= 1'b1;
                        frame_count   <= frame_count + 32'd1;
                    end else begin
                        emit_next_beat();
                    end
                end else if (!m_axis_tvalid) begin
                    emit_next_beat();
                end
            end
        end
    end

    // Combinational helper to assemble a beat from the buffer.
    task emit_next_beat;
        integer remain;
        reg [7:0] b0, b1, b2, b3;
        reg [3:0] k;
        begin
            remain = mm2s_frame_end - mm2s_rd_ptr;
            b0 = (remain >= 1) ? mm2s_buf[mm2s_rd_ptr+0] : 8'h00;
            b1 = (remain >= 2) ? mm2s_buf[mm2s_rd_ptr+1] : 8'h00;
            b2 = (remain >= 3) ? mm2s_buf[mm2s_rd_ptr+2] : 8'h00;
            b3 = (remain >= 4) ? mm2s_buf[mm2s_rd_ptr+3] : 8'h00;
            case (remain)
                1: k = 4'b1000;
                2: k = 4'b1100;
                3: k = 4'b1110;
                default: k = 4'b1111; // remain >= 4
            endcase
            m_axis_tdata  <= {b0, b1, b2, b3};
            m_axis_tkeep  <= k;
            m_axis_tvalid <= 1'b1;
            if (remain <= 4) begin
                m_axis_tlast <= 1'b1;
                mm2s_rd_ptr  <= mm2s_frame_end;
            end else begin
                m_axis_tlast <= 1'b0;
                mm2s_rd_ptr  <= mm2s_rd_ptr + 4;
            end
        end
    endtask

    // -----------------------------------------------------------------
    // TB injection task: write one byte to the buffer.
    // Called via hierarchical reference, e.g.
    //   `tb.dut.u_ch1_tma_tx.inject_byte(8'hAB);`
    // After all bytes written, call `inject_frame_done` to start emission.
    // -----------------------------------------------------------------
    task inject_byte;
        input [7:0] data;
        begin
            if (mm2s_wr_ptr < INJ_BUF_DEPTH) begin
                mm2s_buf[mm2s_wr_ptr] = data;
                mm2s_wr_ptr = mm2s_wr_ptr + 1;
            end
        end
    endtask

    task inject_frame_done;
        begin
            mm2s_rd_ptr    = 0;
            mm2s_frame_end = mm2s_wr_ptr;
            mm2s_busy      = 1'b1;
        end
    endtask

    task inject_reset;
        begin
            mm2s_wr_ptr    = 0;
            mm2s_rd_ptr    = 0;
            mm2s_frame_end = 0;
            mm2s_busy      = 1'b0;
        end
    endtask

    // Initial values for buffer pointers.
    initial begin
        mm2s_wr_ptr    = 0;
        mm2s_rd_ptr    = 0;
        mm2s_frame_end = 0;
        mm2s_busy      = 1'b0;
        mm2s_byte_idx  = 0;
        mm2s_beat      = 32'd0;
    end

endmodule

`default_nettype wire
