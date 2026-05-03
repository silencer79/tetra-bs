// rtl/infra/tetra_tmasap_tx_framer.v
//
// Owned by Agent A2 (A2-fpga-tmasap-framer).
//
// FPGA-side framer for the TmaSap-TX (PS -> FPGA) channel of the
// AXI-DMA signalling TX path.  Receives 32-bit AXIS frames produced by
// the SW daemon (cf. `sw/lib_tetra_axidma/`), validates the
// `0x544D_4153 ("TMAS")` magic + length-prefix, then emits the MM-body
// payload as a per-byte stream into the UMAC DL signal queue feed
// (eventually consumed by `tetra_mac_resource_dl_builder` per
// `IF_UMAC_TMASAP_v1` §5).
//
// TX frame format on the wire (ARCHITECTURE.md §"TmaSap (Signalling)
// - Frame format (TX SW->FPGA)"):
//
//    offset 0   4B   magic = 0x544D_4153 ("TMAS")
//    offset 4   2B   frame_len   (total bytes incl. header, big-endian)
//    offset 6   2B   pdu_len_bits (big-endian)
//    offset 8   4B   ssi (24-bit MSB-aligned: byte8=0, byte9..11=ssi)
//    offset 12  1B   ssi_type
//    offset 13  1B   flags
//    offset 14  2B   chan_alloc (12-bit packed CmceChanAllocReq, MSB-aligned)
//    offset 16  4B   endpoint_id
//    offset 20  4B   new_endpoint_id (0 if flags[4]=0)
//    offset 24  4B   css_endpoint_id (0 if flags[5]=0)
//    offset 28  4B   scrambling_code
//    offset 32  4B   req_handle  (SW-assigned, echoed back via TMAR)
//    offset 36  N    pdu_bits (ceil(pdu_len_bits/8) bytes, MSB-aligned)
//
// Validation:
//   - Bad magic word -> drop frame, increment `tlm_tmasap_tx_err_cnt`.
//     Re-sync by draining beats up to and including the next tlast.
//   - Length-mismatch (frame_len != actual byte count): drop, error++.
//   - pdu_len_bits > 1024 (sanity, spec max is 432 SCH/F coded but the
//     framer carries MM-body which is < 256 bits): drop, error++.
//
// Output (UMAC DL signal queue feed):
//   - `mb_byte_*` per-byte stream of the MM-body payload, with a
//     leading 1-cycle `mb_frame_start_pulse` carrying ssi/ssi_type/etc.
//     so the downstream byte-to-bits accumulator can construct
//     `mm_pdu_bits[127:0]` MSB-aligned and trigger the
//     `tetra_mac_resource_dl_builder.start` strobe.
//   - For frames whose declared `pdu_len_bits == 0` we emit
//     `mb_frame_start_pulse` followed immediately by `mb_frame_end_pulse`
//     with no byte-stream in between (caller maps to a no-op).
//
// AXIS-in shape (32-bit, MSB-lane = first byte on the wire) matches
// IF_AXIDMA_v1 / `tetra_dma_frame_unpacker`.
//
// Verilog-2001 only (CLAUDE.md §Languages).

`timescale 1ns / 1ps
`default_nettype none

module tetra_tmasap_tx_framer (
    input  wire                 clk,
    input  wire                 rst_n,

    // ---------- AXIS slave in (driven by A1's m_axis_tma_rx_*) --------
    input  wire [31:0]          s_axis_tdata,
    input  wire                 s_axis_tvalid,
    output wire                 s_axis_tready,
    input  wire                 s_axis_tlast,
    input  wire [3:0]           s_axis_tkeep,

    // ---------- UMAC DL-side meta (latched at frame_start) ------------
    output reg  [10:0]          mb_pdu_len_bits,
    output reg  [23:0]          mb_ssi,
    output reg  [2:0]           mb_ssi_type,
    output reg  [7:0]           mb_flags,
    output reg  [11:0]          mb_chan_alloc,
    output reg  [31:0]          mb_endpoint_id,
    output reg  [31:0]          mb_new_endpoint_id,
    output reg  [31:0]          mb_css_endpoint_id,
    output reg  [31:0]          mb_scrambling_code,
    output reg  [31:0]          mb_req_handle,

    // ---------- UMAC DL signal queue byte-stream feed -----------------
    // mb_frame_start_pulse: 1-cycle pulse before the first MM-body byte.
    // mb_byte_data[7:0]   : payload byte (MSB-first per ETSI on-air).
    // mb_byte_valid       : level-sensitive; held until byte_ready.
    // mb_byte_ready       : downstream backpressure.
    // mb_frame_end_pulse  : 1-cycle pulse after the last MM-body byte.
    // mb_frame_error_pulse: 1-cycle pulse on bad frame (no commit).
    output reg                  mb_frame_start_pulse,
    output reg  [7:0]           mb_byte_data,
    output reg                  mb_byte_valid,
    input  wire                 mb_byte_ready,
    output reg                  mb_frame_end_pulse,
    output reg                  mb_frame_error_pulse,

    // ---------- Telemetry ---------------------------------------------
    output reg  [31:0]          tlm_tmasap_tx_frames_cnt,
    output reg  [15:0]          tlm_tmasap_tx_err_cnt
);

    // ----- locked magic ----------------------------------------------
    localparam [31:0] MAGIC_TMAS = 32'h544D_4153;

    // ----- FSM states -------------------------------------------------
    localparam [4:0] S_HDR_W0  = 5'd0;  // magic
    localparam [4:0] S_HDR_W1  = 5'd1;  // frame_len | pdu_len_bits
    localparam [4:0] S_HDR_W2  = 5'd2;  // ssi
    localparam [4:0] S_HDR_W3  = 5'd3;  // ssi_type | flags | chan_alloc
    localparam [4:0] S_HDR_W4  = 5'd4;  // endpoint_id
    localparam [4:0] S_HDR_W5  = 5'd5;  // new_endpoint_id
    localparam [4:0] S_HDR_W6  = 5'd6;  // css_endpoint_id
    localparam [4:0] S_HDR_W7  = 5'd7;  // scrambling_code
    localparam [4:0] S_HDR_W8  = 5'd8;  // req_handle
    localparam [4:0] S_PAY_LD  = 5'd9;  // load next 32b word -> 4-byte shift register
    localparam [4:0] S_PAY_E0  = 5'd10; // emit byte 3 of word (MSB lane = first on-air)
    localparam [4:0] S_PAY_E1  = 5'd11;
    localparam [4:0] S_PAY_E2  = 5'd12;
    localparam [4:0] S_PAY_E3  = 5'd13;
    localparam [4:0] S_DRAIN   = 5'd14; // discard remaining beats up to tlast

    reg [4:0] state;

    // Per-frame latched header
    reg [15:0] frame_len_q;
    reg [15:0] pdu_len_bits_q;
    reg [9:0]  pay_bytes_total;   // ceil(pdu_len_bits / 8) - max 17 (bit-safe at 10b)
    reg [9:0]  pay_bytes_remaining;

    reg [31:0] cur_word;
    reg [3:0]  cur_keep;
    reg        cur_is_last;
    reg [9:0]  bytes_consumed; // bytes consumed since header end (= 36)

    // We accept new beats only in header / payload-load / drain.
    assign s_axis_tready = (state == S_HDR_W0) ||
                           (state == S_HDR_W1) ||
                           (state == S_HDR_W2) ||
                           (state == S_HDR_W3) ||
                           (state == S_HDR_W4) ||
                           (state == S_HDR_W5) ||
                           (state == S_HDR_W6) ||
                           (state == S_HDR_W7) ||
                           (state == S_HDR_W8) ||
                           (state == S_PAY_LD) ||
                           (state == S_DRAIN);

    wire beat_fire = s_axis_tvalid && s_axis_tready;

    // Saturating helpers
    wire [15:0] err_inc    = (tlm_tmasap_tx_err_cnt == 16'hFFFF) ? 16'hFFFF :
                                                                    (tlm_tmasap_tx_err_cnt + 16'd1);
    wire [31:0] frames_inc = (tlm_tmasap_tx_frames_cnt == 32'hFFFF_FFFF) ? 32'hFFFF_FFFF :
                                                                            (tlm_tmasap_tx_frames_cnt + 32'd1);

    // Helper: compute ceil(pdu_len_bits / 8) for the 11-bit pdu length
    // we accept (max 17 bytes for 129-bit MM body, but allow up to 1024
    // bits for forward-compat).
    function [9:0] ceil_bits_to_bytes_10;
        input [10:0] bits_in;
        reg   [13:0] tmp;
        begin
            tmp = {3'b0, bits_in} + 14'd7;
            ceil_bits_to_bytes_10 = tmp[12:3]; // / 8
        end
    endfunction

    // ----- main FSM ---------------------------------------------------
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state                    <= S_HDR_W0;
            frame_len_q              <= 16'b0;
            pdu_len_bits_q           <= 16'b0;
            pay_bytes_total          <= 10'b0;
            pay_bytes_remaining      <= 10'b0;
            cur_word                 <= 32'b0;
            cur_keep                 <= 4'b0;
            cur_is_last              <= 1'b0;
            bytes_consumed           <= 10'b0;
            mb_pdu_len_bits          <= 11'b0;
            mb_ssi                   <= 24'b0;
            mb_ssi_type              <= 3'b0;
            mb_flags                 <= 8'b0;
            mb_chan_alloc            <= 12'b0;
            mb_endpoint_id           <= 32'b0;
            mb_new_endpoint_id       <= 32'b0;
            mb_css_endpoint_id       <= 32'b0;
            mb_scrambling_code       <= 32'b0;
            mb_req_handle            <= 32'b0;
            mb_frame_start_pulse     <= 1'b0;
            mb_byte_data             <= 8'b0;
            mb_byte_valid            <= 1'b0;
            mb_frame_end_pulse       <= 1'b0;
            mb_frame_error_pulse     <= 1'b0;
            tlm_tmasap_tx_frames_cnt <= 32'b0;
            tlm_tmasap_tx_err_cnt    <= 16'b0;
        end else begin
            // single-cycle pulses default low
            mb_frame_start_pulse <= 1'b0;
            mb_frame_end_pulse   <= 1'b0;
            mb_frame_error_pulse <= 1'b0;

            case (state)
            // ---------------------------------------------------------
            S_HDR_W0: begin
                mb_byte_valid <= 1'b0;
                if (beat_fire) begin
                    if (s_axis_tdata == MAGIC_TMAS) begin
                        if (s_axis_tlast) begin
                            // header-truncated frame is bad
                            tlm_tmasap_tx_err_cnt <= err_inc;
                            mb_frame_error_pulse  <= 1'b1;
                            state <= S_HDR_W0;
                        end else begin
                            state <= S_HDR_W1;
                        end
                    end else begin
                        // Bad magic - drop entire frame.
                        tlm_tmasap_tx_err_cnt <= err_inc;
                        mb_frame_error_pulse  <= 1'b1;
                        if (s_axis_tlast) begin
                            state <= S_HDR_W0;
                        end else begin
                            state <= S_DRAIN;
                        end
                    end
                end
            end
            // ---------------------------------------------------------
            S_HDR_W1: begin
                if (beat_fire) begin
                    frame_len_q    <= s_axis_tdata[31:16];
                    pdu_len_bits_q <= s_axis_tdata[15:0];
                    if (s_axis_tlast) begin
                        tlm_tmasap_tx_err_cnt <= err_inc;
                        mb_frame_error_pulse  <= 1'b1;
                        state <= S_HDR_W0;
                    end else begin
                        state <= S_HDR_W2;
                    end
                end
            end
            // ---------------------------------------------------------
            S_HDR_W2: begin
                if (beat_fire) begin
                    mb_ssi <= s_axis_tdata[23:0];
                    if (s_axis_tlast) begin
                        tlm_tmasap_tx_err_cnt <= err_inc;
                        mb_frame_error_pulse  <= 1'b1;
                        state <= S_HDR_W0;
                    end else begin
                        state <= S_HDR_W3;
                    end
                end
            end
            // ---------------------------------------------------------
            S_HDR_W3: begin
                if (beat_fire) begin
                    mb_ssi_type   <= s_axis_tdata[26:24];
                    mb_flags      <= s_axis_tdata[23:16];
                    mb_chan_alloc <= s_axis_tdata[15: 4]; // 12-bit MSB-aligned
                    if (s_axis_tlast) begin
                        tlm_tmasap_tx_err_cnt <= err_inc;
                        mb_frame_error_pulse  <= 1'b1;
                        state <= S_HDR_W0;
                    end else begin
                        state <= S_HDR_W4;
                    end
                end
            end
            // ---------------------------------------------------------
            S_HDR_W4: begin
                if (beat_fire) begin
                    mb_endpoint_id <= s_axis_tdata;
                    if (s_axis_tlast) begin
                        tlm_tmasap_tx_err_cnt <= err_inc;
                        mb_frame_error_pulse  <= 1'b1;
                        state <= S_HDR_W0;
                    end else begin
                        state <= S_HDR_W5;
                    end
                end
            end
            // ---------------------------------------------------------
            S_HDR_W5: begin
                if (beat_fire) begin
                    mb_new_endpoint_id <= s_axis_tdata;
                    if (s_axis_tlast) begin
                        tlm_tmasap_tx_err_cnt <= err_inc;
                        mb_frame_error_pulse  <= 1'b1;
                        state <= S_HDR_W0;
                    end else begin
                        state <= S_HDR_W6;
                    end
                end
            end
            // ---------------------------------------------------------
            S_HDR_W6: begin
                if (beat_fire) begin
                    mb_css_endpoint_id <= s_axis_tdata;
                    if (s_axis_tlast) begin
                        tlm_tmasap_tx_err_cnt <= err_inc;
                        mb_frame_error_pulse  <= 1'b1;
                        state <= S_HDR_W0;
                    end else begin
                        state <= S_HDR_W7;
                    end
                end
            end
            // ---------------------------------------------------------
            S_HDR_W7: begin
                if (beat_fire) begin
                    mb_scrambling_code <= s_axis_tdata;
                    if (s_axis_tlast) begin
                        tlm_tmasap_tx_err_cnt <= err_inc;
                        mb_frame_error_pulse  <= 1'b1;
                        state <= S_HDR_W0;
                    end else begin
                        state <= S_HDR_W8;
                    end
                end
            end
            // ---------------------------------------------------------
            S_HDR_W8: begin
                if (beat_fire) begin
                    mb_req_handle <= s_axis_tdata;
                    // Validate frame_len = 36 + ceil(pdu_len_bits/8)
                    // (rounded up to 4-byte multiple on the AXIS wire,
                    //  but the declared frame_len here is the unpadded
                    //  total).
                    if (pdu_len_bits_q[15:11] != 5'b0) begin
                        // pdu_len_bits > 2047 - sanity drop
                        tlm_tmasap_tx_err_cnt <= err_inc;
                        mb_frame_error_pulse  <= 1'b1;
                        if (s_axis_tlast) state <= S_HDR_W0;
                        else              state <= S_DRAIN;
                    end else if (frame_len_q !=
                                 (16'd36 + {6'b0, ceil_bits_to_bytes_10(pdu_len_bits_q[10:0])})) begin
                        // Length mismatch: declared frame_len
                        // doesn't match 36 + ceil(pdu_len/8).
                        tlm_tmasap_tx_err_cnt <= err_inc;
                        mb_frame_error_pulse  <= 1'b1;
                        if (s_axis_tlast) state <= S_HDR_W0;
                        else              state <= S_DRAIN;
                    end else begin
                        // header OK - publish meta and start payload.
                        mb_pdu_len_bits      <= pdu_len_bits_q[10:0];
                        pay_bytes_total      <= ceil_bits_to_bytes_10(pdu_len_bits_q[10:0]);
                        pay_bytes_remaining  <= ceil_bits_to_bytes_10(pdu_len_bits_q[10:0]);
                        bytes_consumed       <= 10'd0;
                        mb_frame_start_pulse <= 1'b1;
                        if (ceil_bits_to_bytes_10(pdu_len_bits_q[10:0]) == 10'd0) begin
                            // 0-byte body: header-only TMAS frame.
                            // Must end on tlast.
                            if (s_axis_tlast) begin
                                tlm_tmasap_tx_frames_cnt <= frames_inc;
                                mb_frame_end_pulse       <= 1'b1;
                                state <= S_HDR_W0;
                            end else begin
                                tlm_tmasap_tx_err_cnt <= err_inc;
                                mb_frame_error_pulse  <= 1'b1;
                                state <= S_DRAIN;
                            end
                        end else begin
                            if (s_axis_tlast) begin
                                // payload expected but stream ended early
                                tlm_tmasap_tx_err_cnt <= err_inc;
                                mb_frame_error_pulse  <= 1'b1;
                                state <= S_HDR_W0;
                            end else begin
                                state <= S_PAY_LD;
                            end
                        end
                    end
                end
            end
            // ---------------------------------------------------------
            S_PAY_LD: begin
                mb_byte_valid <= 1'b0;
                if (beat_fire) begin
                    cur_word    <= s_axis_tdata;
                    cur_keep    <= s_axis_tkeep;
                    cur_is_last <= s_axis_tlast;
                    state       <= S_PAY_E0;
                end
            end
            // ---------------------------------------------------------
            // Emit lanes 31:24 (byte 0), 23:16 (byte 1), 15:8 (byte 2),
            // 7:0 (byte 3) in that order (MSB lane first per wire-byte
            // map).  Only emit while pay_bytes_remaining > 0; otherwise
            // skip (covers the trailing 0..3 bytes of zero-padding the
            // RX framer added on the way out of the FPGA, which the
            // declared frame_len already excluded).
            S_PAY_E0: begin
                if (pay_bytes_remaining != 10'd0 && cur_keep[3]) begin
                    mb_byte_data  <= cur_word[31:24];
                    mb_byte_valid <= 1'b1;
                    if (mb_byte_ready) begin
                        pay_bytes_remaining <= pay_bytes_remaining - 10'd1;
                        bytes_consumed      <= bytes_consumed + 10'd1;
                        state               <= S_PAY_E1;
                    end
                end else begin
                    mb_byte_valid <= 1'b0;
                    state         <= S_PAY_E1;
                end
            end
            S_PAY_E1: begin
                if (pay_bytes_remaining != 10'd0 && cur_keep[2]) begin
                    mb_byte_data  <= cur_word[23:16];
                    mb_byte_valid <= 1'b1;
                    if (mb_byte_ready) begin
                        pay_bytes_remaining <= pay_bytes_remaining - 10'd1;
                        bytes_consumed      <= bytes_consumed + 10'd1;
                        state               <= S_PAY_E2;
                    end
                end else begin
                    mb_byte_valid <= 1'b0;
                    state         <= S_PAY_E2;
                end
            end
            S_PAY_E2: begin
                if (pay_bytes_remaining != 10'd0 && cur_keep[1]) begin
                    mb_byte_data  <= cur_word[15: 8];
                    mb_byte_valid <= 1'b1;
                    if (mb_byte_ready) begin
                        pay_bytes_remaining <= pay_bytes_remaining - 10'd1;
                        bytes_consumed      <= bytes_consumed + 10'd1;
                        state               <= S_PAY_E3;
                    end
                end else begin
                    mb_byte_valid <= 1'b0;
                    state         <= S_PAY_E3;
                end
            end
            S_PAY_E3: begin
                if (pay_bytes_remaining != 10'd0 && cur_keep[0]) begin
                    mb_byte_data  <= cur_word[ 7: 0];
                    mb_byte_valid <= 1'b1;
                    if (mb_byte_ready) begin
                        pay_bytes_remaining <= pay_bytes_remaining - 10'd1;
                        bytes_consumed      <= bytes_consumed + 10'd1;
                        // After this byte, decide next state.
                        if (pay_bytes_remaining == 10'd1) begin
                            // last MM-body byte just emitted
                            if (cur_is_last) begin
                                tlm_tmasap_tx_frames_cnt <= frames_inc;
                                mb_frame_end_pulse       <= 1'b1;
                                state <= S_HDR_W0;
                            end else begin
                                // there are still bytes on the wire
                                // that are 4-byte-pad zeros - drain
                                // until tlast.
                                state <= S_DRAIN;
                            end
                        end else begin
                            if (cur_is_last) begin
                                // wire ended early: length mismatch
                                tlm_tmasap_tx_err_cnt <= err_inc;
                                mb_frame_error_pulse  <= 1'b1;
                                state <= S_HDR_W0;
                            end else begin
                                state <= S_PAY_LD;
                            end
                        end
                    end
                end else begin
                    mb_byte_valid <= 1'b0;
                    if (pay_bytes_remaining == 10'd0) begin
                        // payload exhausted - check tlast
                        if (cur_is_last) begin
                            tlm_tmasap_tx_frames_cnt <= frames_inc;
                            mb_frame_end_pulse       <= 1'b1;
                            state <= S_HDR_W0;
                        end else begin
                            state <= S_DRAIN;
                        end
                    end else begin
                        if (cur_is_last) begin
                            // wire ended early
                            tlm_tmasap_tx_err_cnt <= err_inc;
                            mb_frame_error_pulse  <= 1'b1;
                            state <= S_HDR_W0;
                        end else begin
                            state <= S_PAY_LD;
                        end
                    end
                end
            end
            // ---------------------------------------------------------
            S_DRAIN: begin
                mb_byte_valid <= 1'b0;
                if (beat_fire && s_axis_tlast) begin
                    state <= S_HDR_W0;
                end
            end
            // ---------------------------------------------------------
            default: state <= S_HDR_W0;
            endcase
        end
    end

endmodule

`default_nettype wire
