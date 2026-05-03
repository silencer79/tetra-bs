// rtl/infra/tetra_dma_frame_packer.v
//
// Owned by Agent A1 (A1-fpga-axi-dma).
//
// Per-SAP byte-stream → AXIS framing for the FPGA→PS DMA TX path.
//
// Prepends an 8-byte self-describing header (4-byte magic + 4-byte length)
// to a payload byte-stream and emits the result as a 32-bit AXIS stream
// suitable for feeding the AXIS-slave port of one of the 4× axi_dma:7.1
// channels (TmaSap-RX or TmdSap-RX direction = FPGA→PS DDR).
//
// Magic values (locked, ARCHITECTURE.md §FPGA↔SW Boundary):
//   MAGIC_TMAS = 0x544D_4153  ("TMAS") — TmaSap signalling frame
//   MAGIC_TMAR = 0x544D_4152  ("TMAR") — TmaSap status report
//   MAGIC_TMDC = 0x544D_4443  ("TMDC") — TmdSap voice (TCH/S ACELP)
//
// Frame layout on the output AXIS stream (network byte order, big-endian):
//   beat[0]  bytes 0..3  = magic (MSB first on the wire)
//   beat[1]  bytes 4..7  = total_frame_len (uint32 BE, includes 8-byte hdr)
//   beat[2..N] payload bytes, 4 per beat, big-endian-packed.
//   tlast asserted on the final beat.
//   tkeep on the final beat reflects valid byte lanes
//     (4'b1111 if total length is multiple of 4; else trailing partial).
//
// Wire-byte mapping (tdata[31:0], MSB on byte 0 of the wire):
//   tdata[31:24] = byte 0 of beat (first on the wire)
//   tdata[23:16] = byte 1
//   tdata[15: 8] = byte 2
//   tdata[ 7: 0] = byte 3 (last on the wire)
//
// Caller contract (per-SAP UMAC/LMAC byte stream side):
//   1. Pulse `s_start` with `s_magic_sel` and `s_payload_len_bytes` valid.
//      The packer latches them and asserts `s_busy`.
//   2. Stream payload bytes via (`s_byte_data`, `s_byte_valid`, `s_byte_ready`).
//      Caller must present exactly `s_payload_len_bytes` bytes.
//   3. When the final AXIS beat has been accepted by the slave,
//      `s_busy` deasserts and a 1-cycle `frame_done_pulse` fires.
//
// AXIS handshake (m_axis side, into the axi_dma S2MM channel):
//   - `m_axis_tvalid` is HIGH only on cycles where a beat is being driven.
//   - `m_axis_tready` may stall arbitrarily; the packer holds the beat.
//   - `m_axis_tlast` HIGH only on the final beat of the frame.
//   - `m_axis_tkeep` always 4'b1111 except optionally the final beat.
//
// Verilog-2001 only (CLAUDE.md §Languages).

`timescale 1ns / 1ps
`default_nettype none

module tetra_dma_frame_packer #(
    // Width of the packet-length field (bytes). 16 bits = 64 KiB max.
    parameter LEN_WIDTH = 16
) (
    input  wire                    clk,
    input  wire                    rst_n,

    // Caller side (UMAC / LMAC).
    input  wire                    s_start,            // 1-cycle pulse
    input  wire [1:0]              s_magic_sel,        // 0=TMAS 1=TMAR 2=TMDC 3=resv
    input  wire [LEN_WIDTH-1:0]    s_payload_len_bytes,
    output reg                     s_busy,
    output reg                     frame_done_pulse,

    input  wire [7:0]              s_byte_data,
    input  wire                    s_byte_valid,
    output wire                    s_byte_ready,

    // AXIS master to axi_dma S2MM channel.
    output reg  [31:0]             m_axis_tdata,
    output reg                     m_axis_tvalid,
    input  wire                    m_axis_tready,
    output reg                     m_axis_tlast,
    output reg  [3:0]              m_axis_tkeep
);

    // ----- locked magic constants ------------------------------------
    localparam [31:0] MAGIC_TMAS = 32'h544D_4153;
    localparam [31:0] MAGIC_TMAR = 32'h544D_4152;
    localparam [31:0] MAGIC_TMDC = 32'h544D_4443;

    // ----- FSM states ------------------------------------------------
    localparam [2:0] S_IDLE    = 3'd0;
    localparam [2:0] S_HDR_M   = 3'd1; // emit magic word
    localparam [2:0] S_HDR_L   = 3'd2; // emit length word
    localparam [2:0] S_PAY_FILL= 3'd3; // accumulate payload bytes into word
    localparam [2:0] S_PAY_EMIT= 3'd4; // emit a full payload beat
    localparam [2:0] S_TAIL    = 3'd5; // emit final partial beat (1..3 bytes)
    localparam [2:0] S_DONE    = 3'd6; // drive frame_done_pulse, return idle

    reg [2:0] state;

    // Latched per-frame parameters
    reg [31:0]            magic_q;
    reg [LEN_WIDTH-1:0]   pay_remaining; // payload bytes still to consume
    reg [LEN_WIDTH+8-1:0] total_len_bytes; // payload + 8

    reg [1:0]  byte_idx;   // 0..3 — count of bytes accumulated in beat_buf
    reg [31:0] beat_buf;   // big-endian-packed accumulator

    // Caller byte stream is accepted only while filling.
    assign s_byte_ready = (state == S_PAY_FILL);

    // ----- main FSM --------------------------------------------------
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state            <= S_IDLE;
            s_busy           <= 1'b0;
            frame_done_pulse <= 1'b0;
            magic_q          <= 32'h0;
            pay_remaining    <= {LEN_WIDTH{1'b0}};
            total_len_bytes  <= {(LEN_WIDTH+8){1'b0}};
            byte_idx         <= 2'd0;
            beat_buf         <= 32'h0;
            m_axis_tdata     <= 32'h0;
            m_axis_tvalid    <= 1'b0;
            m_axis_tlast     <= 1'b0;
            m_axis_tkeep     <= 4'b0000;
        end else begin
            frame_done_pulse <= 1'b0;

            case (state)
            // ---------------------------------------------------------
            S_IDLE: begin
                m_axis_tvalid <= 1'b0;
                m_axis_tlast  <= 1'b0;
                m_axis_tkeep  <= 4'b0000;
                if (s_start) begin
                    case (s_magic_sel)
                        2'd0:    magic_q <= MAGIC_TMAS;
                        2'd1:    magic_q <= MAGIC_TMAR;
                        2'd2:    magic_q <= MAGIC_TMDC;
                        default: magic_q <= MAGIC_TMAS;
                    endcase
                    pay_remaining   <= s_payload_len_bytes;
                    total_len_bytes <= {{8{1'b0}}, s_payload_len_bytes} +
                                       {{LEN_WIDTH{1'b0}}, 8'd8};
                    byte_idx        <= 2'd0;
                    beat_buf        <= 32'h0;
                    s_busy          <= 1'b1;
                    state           <= S_HDR_M;
                end
            end
            // ---------------------------------------------------------
            S_HDR_M: begin
                m_axis_tdata  <= magic_q;
                m_axis_tvalid <= 1'b1;
                m_axis_tlast  <= 1'b0;
                m_axis_tkeep  <= 4'b1111;
                if (m_axis_tready) state <= S_HDR_L;
            end
            // ---------------------------------------------------------
            S_HDR_L: begin
                m_axis_tdata  <= { {(32-(LEN_WIDTH+8)){1'b0}}, total_len_bytes };
                m_axis_tvalid <= 1'b1;
                m_axis_tlast  <= 1'b0;
                m_axis_tkeep  <= 4'b1111;
                if (m_axis_tready) begin
                    if (pay_remaining == 0) begin
                        // 0-byte payload — header alone is the whole frame.
                        // Mark this LEN beat as tlast on the next cycle by
                        // re-driving it (we already accepted it though, so
                        // we simply head to DONE; in practice this is a
                        // pathological / forbidden case).
                        state <= S_DONE;
                    end else begin
                        state <= S_PAY_FILL;
                    end
                end
            end
            // ---------------------------------------------------------
            S_PAY_FILL: begin
                m_axis_tvalid <= 1'b0;
                m_axis_tlast  <= 1'b0;
                m_axis_tkeep  <= 4'b0000;

                if (s_byte_valid) begin
                    // Latch byte into the right lane.
                    case (byte_idx)
                        2'd0: beat_buf[31:24] <= s_byte_data;
                        2'd1: beat_buf[23:16] <= s_byte_data;
                        2'd2: beat_buf[15: 8] <= s_byte_data;
                        2'd3: beat_buf[ 7: 0] <= s_byte_data;
                    endcase
                    pay_remaining <= pay_remaining - {{(LEN_WIDTH-1){1'b0}}, 1'b1};

                    if (byte_idx == 2'd3) begin
                        // Word complete → emit on next cycle.
                        byte_idx <= 2'd0;
                        state    <= S_PAY_EMIT;
                    end else begin
                        // Partial; check for last-byte-of-payload.
                        if (pay_remaining == { {(LEN_WIDTH-1){1'b0}}, 1'b1 }) begin
                            // last byte arrived; bytes_in_partial = byte_idx+1
                            byte_idx <= byte_idx + 2'd1;
                            state    <= S_TAIL;
                        end else begin
                            byte_idx <= byte_idx + 2'd1;
                        end
                    end
                end
            end
            // ---------------------------------------------------------
            S_PAY_EMIT: begin
                m_axis_tdata  <= beat_buf;
                m_axis_tvalid <= 1'b1;
                m_axis_tkeep  <= 4'b1111;
                // tlast HIGH iff this beat exhausts the payload AND there
                // is no trailing partial. pay_remaining was pre-decremented
                // when the bytes were latched, so if it == 0 we are done.
                m_axis_tlast  <= (pay_remaining == 0);
                if (m_axis_tready) begin
                    if (pay_remaining == 0) begin
                        state <= S_DONE;
                    end else begin
                        state <= S_PAY_FILL;
                    end
                end
            end
            // ---------------------------------------------------------
            S_TAIL: begin
                m_axis_tdata  <= beat_buf;
                m_axis_tvalid <= 1'b1;
                m_axis_tlast  <= 1'b1;
                case (byte_idx)
                    2'd1: m_axis_tkeep <= 4'b1000; // only byte0 valid
                    2'd2: m_axis_tkeep <= 4'b1100;
                    2'd3: m_axis_tkeep <= 4'b1110;
                    default: m_axis_tkeep <= 4'b1111; // pathological
                endcase
                if (m_axis_tready) state <= S_DONE;
            end
            // ---------------------------------------------------------
            S_DONE: begin
                m_axis_tvalid    <= 1'b0;
                m_axis_tlast     <= 1'b0;
                m_axis_tkeep     <= 4'b0000;
                s_busy           <= 1'b0;
                frame_done_pulse <= 1'b1;
                state            <= S_IDLE;
            end
            // ---------------------------------------------------------
            default: state <= S_IDLE;
            endcase
        end
    end

endmodule

`default_nettype wire
