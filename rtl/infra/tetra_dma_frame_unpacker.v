// rtl/infra/tetra_dma_frame_unpacker.v
//
// Owned by Agent A1 (A1-fpga-axi-dma).
//
// Reverse of `tetra_dma_frame_packer.v`: consumes a 32-bit AXIS stream
// from the PS→FPGA direction (DMA RX = MM2S in axi_dma terminology),
// validates the 4-byte magic header + 4-byte length, and re-emits the
// payload as a per-SAP byte stream with a magic-select tag.
//
// Magic values (locked, ARCHITECTURE.md §FPGA↔SW Boundary):
//   MAGIC_TMAS = 0x544D_4153  → magic_sel=0
//   MAGIC_TMAR = 0x544D_4152  → magic_sel=1
//   MAGIC_TMDC = 0x544D_4443  → magic_sel=2
//
// Validation behaviour:
//   - Bad magic word (no match) → frame is dropped. `drop_count` increments.
//     We then re-sync by waiting for the next AXIS beat carrying tlast (i.e.
//     skip the rest of the bad frame).
//   - Length-mismatch (declared total_frame_len doesn't match the actual
//     beat-count×4 minus invalid bytes per tkeep at the final beat) →
//     frame is dropped, `drop_count` increments. Still the byte-stream
//     consumer sees no `m_byte_valid` asserts for that frame (we only
//     emit bytes after having validated the entire frame? — too memory
//     heavy; instead we stream bytes immediately and assert a per-frame
//     `m_frame_error_pulse` if the length-mismatch is detected at end-of-
//     frame. The downstream byte consumer must be tolerant of partial
//     frames followed by `m_frame_error_pulse` and should discard.)
//
// For the in-FPGA TmaSap/TmdSap RX framers (A2/A3) the consumer is a
// state machine that accumulates bytes into a fixed-length PDU; if
// `m_frame_error_pulse` fires, it resets its accumulator. This matches
// the pattern that `axi_dma:7.1` itself follows on tdest/tlast errors.
//
// Caller contract — AXIS slave side (s_axis from axi_dma MM2S channel):
//   - Standard 32-bit AXIS: tdata, tvalid, tready, tlast, tkeep.
//   - We assert `s_axis_tready` whenever we have room to consume a beat
//     (always true except during single-cycle drop re-sync).
//
// Byte-stream master (m_byte_*):
//   - `m_byte_data[7:0]`    — payload byte
//   - `m_byte_valid`        — HIGH for one cycle per byte
//   - `m_byte_ready`        — backpressure from downstream
//   - `m_frame_start_pulse` — 1-cycle pulse with `m_magic_sel` valid;
//                             fires AFTER the header is validated and
//                             BEFORE the first payload byte. Tells the
//                             downstream framer which SAP to dispatch to.
//   - `m_magic_sel[1:0]`    — 0=TMAS 1=TMAR 2=TMDC, valid when
//                             `m_frame_start_pulse` HIGH.
//   - `m_frame_end_pulse`   — 1-cycle pulse after last payload byte.
//   - `m_frame_error_pulse` — 1-cycle pulse on bad-magic or length-mismatch.
//
// Telemetry:
//   - `drop_count[15:0]`    — saturating count of dropped frames.
//
// Verilog-2001 only (CLAUDE.md §Languages).

`timescale 1ns / 1ps
`default_nettype none

module tetra_dma_frame_unpacker #(
    parameter LEN_WIDTH = 16
) (
    input  wire                    clk,
    input  wire                    rst_n,

    // AXIS slave from axi_dma MM2S channel.
    input  wire [31:0]             s_axis_tdata,
    input  wire                    s_axis_tvalid,
    output wire                    s_axis_tready,
    input  wire                    s_axis_tlast,
    input  wire [3:0]              s_axis_tkeep,

    // Byte-stream master out to per-SAP RX framer.
    output reg  [7:0]              m_byte_data,
    output reg                     m_byte_valid,
    input  wire                    m_byte_ready,

    output reg                     m_frame_start_pulse,
    output reg  [1:0]              m_magic_sel,
    output reg                     m_frame_end_pulse,
    output reg                     m_frame_error_pulse,

    // Telemetry
    output reg  [15:0]             drop_count
);

    // ----- locked magic constants ------------------------------------
    localparam [31:0] MAGIC_TMAS = 32'h544D_4153;
    localparam [31:0] MAGIC_TMAR = 32'h544D_4152;
    localparam [31:0] MAGIC_TMDC = 32'h544D_4443;

    // ----- FSM states ------------------------------------------------
    localparam [2:0] S_HDR_M = 3'd0; // wait for magic word
    localparam [2:0] S_HDR_L = 3'd1; // wait for length word
    localparam [2:0] S_PAY   = 3'd2; // emit payload bytes
    localparam [2:0] S_DRAIN = 3'd3; // drop bad frame; wait for tlast
    localparam [2:0] S_EMIT0 = 3'd4; // emit byte 0 of latched word
    localparam [2:0] S_EMIT1 = 3'd5;
    localparam [2:0] S_EMIT2 = 3'd6;
    localparam [2:0] S_EMIT3 = 3'd7;

    reg [2:0]  state;
    reg [31:0] cur_word;             // currently emitting word
    reg [3:0]  cur_keep;             // tkeep of the currently emitting word
    reg        cur_is_last;          // tlast of currently emitting word
    reg [1:0]  cur_magic_sel;
    reg [LEN_WIDTH+8-1:0] declared_total_len;
    reg [LEN_WIDTH+8-1:0] running_byte_count;  // bytes already accounted
                                               // (header 8 + payload bytes
                                               // emitted so far)

    // We accept a header beat or claim a payload beat in S_HDR_M / S_HDR_L /
    // S_PAY. While emitting bytes (S_EMIT0..3) we do NOT accept new beats.
    // While draining a bad frame we accept and discard.
    assign s_axis_tready = (state == S_HDR_M) ||
                           (state == S_HDR_L) ||
                           (state == S_PAY)   ||
                           (state == S_DRAIN);

    // Helper: decode magic.
    function [2:0] decode_magic;
        input [31:0] w;
        begin
            case (w)
                MAGIC_TMAS: decode_magic = {1'b1, 2'd0}; // [2]=ok, [1:0]=sel
                MAGIC_TMAR: decode_magic = {1'b1, 2'd1};
                MAGIC_TMDC: decode_magic = {1'b1, 2'd2};
                default:    decode_magic = {1'b0, 2'd0};
            endcase
        end
    endfunction

    // Helper: byte-count from tkeep (4-bit, contiguous from MSB-lane).
    function [2:0] keep_to_count;
        input [3:0] k;
        begin
            case (k)
                4'b1111: keep_to_count = 3'd4;
                4'b1110: keep_to_count = 3'd3;
                4'b1100: keep_to_count = 3'd2;
                4'b1000: keep_to_count = 3'd1;
                4'b0000: keep_to_count = 3'd0;
                default: keep_to_count = 3'd0; // illegal sparse keep → drop
            endcase
        end
    endfunction

    wire        beat_fire   = s_axis_tvalid && s_axis_tready;
    wire [2:0]  m_decoded   = decode_magic(s_axis_tdata);
    wire        m_ok        = m_decoded[2];
    wire [1:0]  m_sel       = m_decoded[1:0];

    // Saturating increment helper for drop_count.
    wire [15:0] drop_count_inc = (drop_count == 16'hFFFF) ? 16'hFFFF :
                                                            (drop_count + 16'd1);

    // ----- main FSM --------------------------------------------------
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state               <= S_HDR_M;
            cur_word            <= 32'h0;
            cur_keep            <= 4'b0000;
            cur_is_last         <= 1'b0;
            cur_magic_sel       <= 2'd0;
            declared_total_len  <= {(LEN_WIDTH+8){1'b0}};
            running_byte_count  <= {(LEN_WIDTH+8){1'b0}};
            m_byte_data         <= 8'h00;
            m_byte_valid        <= 1'b0;
            m_frame_start_pulse <= 1'b0;
            m_magic_sel         <= 2'd0;
            m_frame_end_pulse   <= 1'b0;
            m_frame_error_pulse <= 1'b0;
            drop_count          <= 16'd0;
        end else begin
            // single-cycle pulses default to 0
            m_frame_start_pulse <= 1'b0;
            m_frame_end_pulse   <= 1'b0;
            m_frame_error_pulse <= 1'b0;

            case (state)
            // ---------------------------------------------------------
            S_HDR_M: begin
                m_byte_valid <= 1'b0;
                if (beat_fire) begin
                    if (m_ok) begin
                        cur_magic_sel <= m_sel;
                        running_byte_count <= {{(LEN_WIDTH+8-4){1'b0}}, 4'd4}; // 4 bytes consumed (magic)
                        if (s_axis_tlast) begin
                            // header beat is also tlast: malformed (must
                            // have at least the LEN beat). Drop.
                            drop_count <= drop_count_inc;
                            m_frame_error_pulse <= 1'b1;
                            state <= S_HDR_M;
                        end else begin
                            state <= S_HDR_L;
                        end
                    end else begin
                        // bad magic
                        drop_count <= drop_count_inc;
                        m_frame_error_pulse <= 1'b1;
                        if (s_axis_tlast) begin
                            // single-beat bad frame; we are already done.
                            state <= S_HDR_M;
                        end else begin
                            state <= S_DRAIN;
                        end
                    end
                end
            end
            // ---------------------------------------------------------
            S_HDR_L: begin
                m_byte_valid <= 1'b0;
                if (beat_fire) begin
                    declared_total_len <= s_axis_tdata[LEN_WIDTH+8-1:0];
                    running_byte_count <= running_byte_count + 4; // +4 (LEN)
                    // Emit frame_start now that header is fully validated.
                    m_frame_start_pulse <= 1'b1;
                    m_magic_sel         <= cur_magic_sel;
                    if (s_axis_tlast) begin
                        // Header-only frame (no payload).
                        // Length must be exactly 8.
                        if (s_axis_tdata[LEN_WIDTH+8-1:0] == {{(LEN_WIDTH){1'b0}}, 8'd8}) begin
                            // Valid 0-byte payload frame.
                            m_frame_end_pulse <= 1'b1;
                            state <= S_HDR_M;
                        end else begin
                            // length mismatch — declared > 8 but axis ended.
                            drop_count <= drop_count_inc;
                            m_frame_error_pulse <= 1'b1;
                            state <= S_HDR_M;
                        end
                    end else begin
                        state <= S_PAY;
                    end
                end
            end
            // ---------------------------------------------------------
            S_PAY: begin
                m_byte_valid <= 1'b0;
                if (beat_fire) begin
                    cur_word    <= s_axis_tdata;
                    cur_keep    <= s_axis_tkeep;
                    cur_is_last <= s_axis_tlast;
                    state       <= S_EMIT0;
                end
            end
            // ---------------------------------------------------------
            // Emit lanes 0..3 of cur_word (MSB lane first per wire-byte map).
            // Skip lanes whose tkeep bit is 0.
            S_EMIT0: begin
                if (cur_keep[3]) begin
                    m_byte_data  <= cur_word[31:24];
                    m_byte_valid <= 1'b1;
                    if (m_byte_ready) begin
                        running_byte_count <= running_byte_count + 1;
                        state <= S_EMIT1;
                    end
                end else begin
                    m_byte_valid <= 1'b0;
                    state <= S_EMIT1;
                end
            end
            // ---------------------------------------------------------
            S_EMIT1: begin
                if (cur_keep[2]) begin
                    m_byte_data  <= cur_word[23:16];
                    m_byte_valid <= 1'b1;
                    if (m_byte_ready) begin
                        running_byte_count <= running_byte_count + 1;
                        state <= S_EMIT2;
                    end
                end else begin
                    m_byte_valid <= 1'b0;
                    state <= S_EMIT2;
                end
            end
            // ---------------------------------------------------------
            S_EMIT2: begin
                if (cur_keep[1]) begin
                    m_byte_data  <= cur_word[15:8];
                    m_byte_valid <= 1'b1;
                    if (m_byte_ready) begin
                        running_byte_count <= running_byte_count + 1;
                        state <= S_EMIT3;
                    end
                end else begin
                    m_byte_valid <= 1'b0;
                    state <= S_EMIT3;
                end
            end
            // ---------------------------------------------------------
            S_EMIT3: begin
                if (cur_keep[0]) begin
                    m_byte_data  <= cur_word[7:0];
                    m_byte_valid <= 1'b1;
                    if (m_byte_ready) begin
                        running_byte_count <= running_byte_count + 1;
                        // After emitting the last lane:
                        if (cur_is_last) begin
                            // final beat — verify length.
                            if ((running_byte_count + 1) == declared_total_len) begin
                                m_frame_end_pulse <= 1'b1;
                            end else begin
                                drop_count <= drop_count_inc;
                                m_frame_error_pulse <= 1'b1;
                            end
                            state <= S_HDR_M;
                        end else begin
                            state <= S_PAY;
                        end
                    end
                end else begin
                    m_byte_valid <= 1'b0;
                    if (cur_is_last) begin
                        // final beat with cur_keep[0]==0 is normal partial.
                        if (running_byte_count == declared_total_len) begin
                            m_frame_end_pulse <= 1'b1;
                        end else begin
                            drop_count <= drop_count_inc;
                            m_frame_error_pulse <= 1'b1;
                        end
                        state <= S_HDR_M;
                    end else begin
                        state <= S_PAY;
                    end
                end
            end
            // ---------------------------------------------------------
            S_DRAIN: begin
                m_byte_valid <= 1'b0;
                if (beat_fire && s_axis_tlast) begin
                    state <= S_HDR_M;
                end
            end
            // ---------------------------------------------------------
            default: state <= S_HDR_M;
            endcase
        end
    end

endmodule

`default_nettype wire
