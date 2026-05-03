// rtl/infra/tetra_tmasap_rx_framer.v
//
// Owned by Agent A2 (A2-fpga-tmasap-framer).
//
// FPGA-side framer for the TmaSap-RX (FPGA -> PS) channel of the AXI-DMA
// signalling RX path.  Two producer roles share one AXIS-out:
//
//   1. TMAS frames (signalling): triggered by the UMAC reassembly
//      (`tetra_ul_demand_reassembly`, see `IF_UMAC_TMASAP_v1` in
//      `docs/references/umac_port_contract.md` §1).  The reassembled MM
//      body (1..129 bits, MSB-aligned in `umac_to_tmasap_rx_pdu[128:0]`)
//      together with the 24-bit SSI and slot meta becomes a TMAS frame
//      per ARCHITECTURE.md §"TmaSap (Signalling) - Frame format (RX)":
//
//        offset 0  4B   magic = 0x544D_4153 ("TMAS")
//        offset 4  2B   frame_len (total bytes incl. header, big-endian)
//        offset 6  2B   pdu_len_bits (big-endian)
//        offset 8  4B   ssi (24-bit MSB-aligned: byte8=0, byte9..11=ssi)
//        offset 12 1B   ssi_type
//        offset 13 1B   flags
//        offset 14 2B   reserved
//        offset 16 4B   endpoint_id
//        offset 20 4B   new_endpoint_id
//        offset 24 4B   css_endpoint_id
//        offset 28 4B   scrambling_code
//        offset 32 4B   reserved
//        offset 36 N    pdu_bits (ceil(pdu_len_bits/8) bytes, MSB-aligned)
//
//   2. TMAR frames (reports): triggered by an external `tmar_emit_pulse`.
//      Per Decision #4 + ARCHITECTURE.md §"Report frame (FPGA->SW status)":
//
//        offset 0  4B   magic = 0x544D_4152 ("TMAR")
//        offset 4  2B   frame_len = 12 (big-endian)
//        offset 6  2B   reserved = 0
//        offset 8  4B   req_handle (echo of TX commit)
//        offset 12 1B   report_code (0..5; see ARCHITECTURE.md)
//        offset 13 3B   pad = 0
//
// Magic and field byte-order are bit-exact per ARCHITECTURE.md.  The
// `tetra_dma_frame_packer` carry-over encodes the same magic+length
// header but is NOT used here because it does not understand the
// per-frame TMAS/TMAR shape (variable trailing pad / fixed-width meta).
// Instead this framer drives the AXIS slave port directly.
//
// Output AXIS shape: 32-bit, MSB-lane = first byte on the wire.
// `tkeep` always 4'b1111 because both TMAS and TMAR frames are
// designed to be multiples of 4 bytes (TMAS = 36 + ceil(pdu_len_bits/8)
// padded up to 4-byte multiple via the trailing reserved field at
// offset 32; TMAR = 16 bytes total).  See pad rationale below.
//
// TMAS pad rationale: MM body MSB-alignment puts the bits in the high
// bits of byte 36..; we round up to a 4-byte boundary by zero-padding
// the trailing tail of the last word.  This keeps tkeep=4'b1111 for
// every beat.  Frame length declared in offset[4..5] is the unpadded
// total (i.e. 36 + ceil(pdu_len_bits/8)); frame length on the wire
// (the AXIS beat count * 4) may be 1..3 bytes greater.  The PS-side
// dispatcher is required to honour the declared length, not the AXIS
// beat-count - see `sw/lib_tetra_axidma/`.
//
// Verilog-2001 only (CLAUDE.md §Languages).

`timescale 1ns / 1ps
`default_nettype none

module tetra_tmasap_rx_framer (
    input  wire                 clk,
    input  wire                 rst_n,

    // ---------- UMAC TmaSap-RX side (per IF_UMAC_TMASAP_v1) ------------
    // Pulse on `umac_to_tmasap_rx_valid` while the framer is idle latches
    // a TMAS frame request.  The framer asserts `umac_to_tmasap_rx_ready`
    // for one cycle when the request is captured.
    input  wire                 umac_to_tmasap_rx_valid,
    output wire                 umac_to_tmasap_rx_ready,
    input  wire [128:0]         umac_to_tmasap_rx_pdu,       // MSB-first
    input  wire [10:0]          umac_to_tmasap_rx_pdu_len,   // bits, 1..129
    input  wire [23:0]          umac_to_tmasap_rx_ssi,
    input  wire [2:0]           umac_to_tmasap_rx_ssi_type,
    // Slot meta (parallel signal sources; not in IF_UMAC_TMASAP_v1
    // proper, but listed in the doc as "+ slot meta from a parallel
    // signal source").  Driven 0 if not connected.
    input  wire [31:0]          umac_to_tmasap_rx_endpoint_id,
    input  wire [31:0]          umac_to_tmasap_rx_scrambling_code,

    // ---------- TMAR report-emit side ---------------------------------
    // 1-cycle pulse with `tmar_req_handle` + `tmar_report_code` valid.
    input  wire                 tmar_emit_pulse,
    input  wire [31:0]          tmar_req_handle,
    input  wire [7:0]           tmar_report_code,

    // ---------- AXIS master out (drives A1's s_axis_tma_rx_* slave) ---
    output reg  [31:0]          m_axis_tdata,
    output reg                  m_axis_tvalid,
    input  wire                 m_axis_tready,
    output reg                  m_axis_tlast,
    output reg  [3:0]           m_axis_tkeep,

    // ---------- Telemetry counters (saturating) -----------------------
    output reg  [31:0]          tlm_tmas_frames_cnt,
    output reg  [31:0]          tlm_tmar_frames_cnt,
    output reg  [15:0]          tlm_rx_drop_cnt
);

    // ----- locked magic constants ------------------------------------
    localparam [31:0] MAGIC_TMAS = 32'h544D_4153;
    localparam [31:0] MAGIC_TMAR = 32'h544D_4152;

    // ----- FSM states -------------------------------------------------
    localparam [3:0] S_IDLE      = 4'd0;
    localparam [3:0] S_TMAS_W0   = 4'd1;  // magic
    localparam [3:0] S_TMAS_W1   = 4'd2;  // frame_len[15:0] | pdu_len_bits[15:0]
    localparam [3:0] S_TMAS_W2   = 4'd3;  // 0x00 | ssi[23:0]
    localparam [3:0] S_TMAS_W3   = 4'd4;  // ssi_type | flags | reserved
    localparam [3:0] S_TMAS_W4   = 4'd5;  // endpoint_id
    localparam [3:0] S_TMAS_W5   = 4'd6;  // new_endpoint_id (0)
    localparam [3:0] S_TMAS_W6   = 4'd7;  // css_endpoint_id (0)
    localparam [3:0] S_TMAS_W7   = 4'd8;  // scrambling_code
    localparam [3:0] S_TMAS_W8   = 4'd9;  // reserved
    localparam [3:0] S_TMAS_PAY  = 4'd10; // payload words (0..N)
    localparam [3:0] S_TMAR_W0   = 4'd11; // magic
    localparam [3:0] S_TMAR_W1   = 4'd12; // frame_len=12 | reserved
    localparam [3:0] S_TMAR_W2   = 4'd13; // req_handle
    localparam [3:0] S_TMAR_W3   = 4'd14; // report_code | pad

    reg [3:0] state;

    // Latched per-frame parameters
    reg [128:0] pdu_q;
    reg [10:0]  pdu_len_bits_q;
    reg [23:0]  ssi_q;
    reg [2:0]   ssi_type_q;
    reg [31:0]  endpoint_id_q;
    reg [31:0]  scrambling_code_q;
    reg [31:0]  req_handle_q;
    reg [7:0]   report_code_q;

    // Number of payload bytes (ceil(pdu_len_bits / 8)) and 4-byte words
    // (ceil(payload_bytes / 4)).  Max payload = 129 bits = 17 bytes = 5 words.
    reg [4:0]  pay_words_total;  // 0..5
    reg [4:0]  pay_word_idx;     // current payload word index 0..pay_words_total
    reg [15:0] frame_len_bytes;  // declared TMAS frame_len = 36 + ceil(pdu_len_bits/8)

    // ----- IDLE accept handshake -------------------------------------
    // Ready ONLY in S_IDLE when no TMAR pulse is being serviced this
    // cycle (TMAR has implicit priority because it is a 1-cycle pulse
    // with no back-pressure - we must not drop it).  Practically the
    // emitter ensures non-overlap.
    assign umac_to_tmasap_rx_ready = (state == S_IDLE) && !tmar_emit_pulse;

    // Helper: ceil-div by 8 for 11-bit input -> max 17 (bits 4..0).
    function [4:0] ceil_bits_to_bytes;
        input [10:0] bits_in;
        reg   [13:0] tmp;
        begin
            tmp = {3'b0, bits_in} + 14'd7;
            ceil_bits_to_bytes = tmp[7:3]; // / 8
        end
    endfunction

    function [4:0] ceil_bytes_to_words;
        input [4:0]  bytes_in;
        reg   [6:0]  tmp;
        begin
            tmp = {2'b0, bytes_in} + 7'd3;
            ceil_bytes_to_words = tmp[6:2]; // / 4
        end
    endfunction

    // ----- payload word selector --------------------------------------
    // Returns the 32-bit word at offset 4*idx into the MSB-aligned
    // 17-byte (136-bit) buffer formed by left-padding `pdu_q` from
    // 129 to 136 bits with zeros at LSB end (so MSB of pdu_q is byte 0
    // of the payload area, and LSB-aligned residual bits stay at the
    // end).  Concretely: the MM body's first on-air bit is `pdu_q[128]`,
    // which becomes bit [31] of payload_word(0) (= byte 36 bit 7 on the
    // wire).  See umac_port_contract.md §1 "MSB-first ... bit 128 =
    // first on-air bit".
    //
    // Construction:
    //   pad_buf[135:0] = { pdu_q[128:0], 7'b0 } padded to a multiple of
    //                    32 bits at the LSB end with zeros up to 160
    //                    (= 5 words = 20 bytes); we only emit the first
    //                    pay_words_total words.
    wire [159:0] pay_pad_buf = { pdu_q[128:0], 31'b0 };

    function [31:0] pay_word_at;
        input [4:0] idx;
        begin
            case (idx)
                5'd0: pay_word_at = pay_pad_buf[159:128];
                5'd1: pay_word_at = pay_pad_buf[127: 96];
                5'd2: pay_word_at = pay_pad_buf[ 95: 64];
                5'd3: pay_word_at = pay_pad_buf[ 63: 32];
                5'd4: pay_word_at = pay_pad_buf[ 31:  0];
                default: pay_word_at = 32'h0;
            endcase
        end
    endfunction

    // ----- main FSM ---------------------------------------------------
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state              <= S_IDLE;
            pdu_q              <= 129'b0;
            pdu_len_bits_q     <= 11'b0;
            ssi_q              <= 24'b0;
            ssi_type_q         <= 3'b0;
            endpoint_id_q      <= 32'b0;
            scrambling_code_q  <= 32'b0;
            req_handle_q       <= 32'b0;
            report_code_q      <= 8'b0;
            pay_words_total    <= 5'b0;
            pay_word_idx       <= 5'b0;
            frame_len_bytes    <= 16'b0;
            m_axis_tdata       <= 32'h0;
            m_axis_tvalid      <= 1'b0;
            m_axis_tlast       <= 1'b0;
            m_axis_tkeep       <= 4'b0000;
            tlm_tmas_frames_cnt<= 32'b0;
            tlm_tmar_frames_cnt<= 32'b0;
            tlm_rx_drop_cnt    <= 16'b0;
        end else begin
            case (state)
            // ---------------------------------------------------------
            S_IDLE: begin
                m_axis_tvalid <= 1'b0;
                m_axis_tlast  <= 1'b0;
                m_axis_tkeep  <= 4'b0000;
                // TMAR has priority over TMAS to honour pulse semantics.
                if (tmar_emit_pulse) begin
                    req_handle_q  <= tmar_req_handle;
                    report_code_q <= tmar_report_code;
                    state         <= S_TMAR_W0;
                end else if (umac_to_tmasap_rx_valid) begin
                    pdu_q             <= umac_to_tmasap_rx_pdu;
                    pdu_len_bits_q    <= umac_to_tmasap_rx_pdu_len;
                    ssi_q             <= umac_to_tmasap_rx_ssi;
                    ssi_type_q        <= umac_to_tmasap_rx_ssi_type;
                    endpoint_id_q     <= umac_to_tmasap_rx_endpoint_id;
                    scrambling_code_q <= umac_to_tmasap_rx_scrambling_code;
                    pay_words_total   <= ceil_bytes_to_words(
                                            ceil_bits_to_bytes(umac_to_tmasap_rx_pdu_len));
                    pay_word_idx      <= 5'b0;
                    frame_len_bytes   <= 16'd36 +
                                         {11'b0, ceil_bits_to_bytes(umac_to_tmasap_rx_pdu_len)};
                    state             <= S_TMAS_W0;
                end
            end
            // ---------------------------------------------------------
            // ---- TMAS emission ---------------------------------------
            S_TMAS_W0: begin
                m_axis_tdata  <= MAGIC_TMAS;
                m_axis_tvalid <= 1'b1;
                m_axis_tkeep  <= 4'b1111;
                m_axis_tlast  <= 1'b0;
                if (m_axis_tready) state <= S_TMAS_W1;
            end
            S_TMAS_W1: begin
                // [31:16] frame_len, [15:0] pdu_len_bits
                m_axis_tdata  <= { frame_len_bytes, 5'b0, pdu_len_bits_q[10:0] };
                m_axis_tvalid <= 1'b1;
                m_axis_tkeep  <= 4'b1111;
                m_axis_tlast  <= 1'b0;
                if (m_axis_tready) state <= S_TMAS_W2;
            end
            S_TMAS_W2: begin
                // ssi MSB-aligned in 4 bytes: { 8'h00, ssi[23:0] }
                m_axis_tdata  <= { 8'h00, ssi_q };
                m_axis_tvalid <= 1'b1;
                m_axis_tkeep  <= 4'b1111;
                m_axis_tlast  <= 1'b0;
                if (m_axis_tready) state <= S_TMAS_W3;
            end
            S_TMAS_W3: begin
                // [31:24] ssi_type, [23:16] flags, [15:0] reserved
                m_axis_tdata  <= { 5'b0, ssi_type_q, 8'h00, 16'h0000 };
                m_axis_tvalid <= 1'b1;
                m_axis_tkeep  <= 4'b1111;
                m_axis_tlast  <= 1'b0;
                if (m_axis_tready) state <= S_TMAS_W4;
            end
            S_TMAS_W4: begin
                m_axis_tdata  <= endpoint_id_q;
                m_axis_tvalid <= 1'b1;
                m_axis_tkeep  <= 4'b1111;
                m_axis_tlast  <= 1'b0;
                if (m_axis_tready) state <= S_TMAS_W5;
            end
            S_TMAS_W5: begin
                m_axis_tdata  <= 32'h0;        // new_endpoint_id (BS doesn't emit)
                m_axis_tvalid <= 1'b1;
                m_axis_tkeep  <= 4'b1111;
                m_axis_tlast  <= 1'b0;
                if (m_axis_tready) state <= S_TMAS_W6;
            end
            S_TMAS_W6: begin
                m_axis_tdata  <= 32'h0;        // css_endpoint_id
                m_axis_tvalid <= 1'b1;
                m_axis_tkeep  <= 4'b1111;
                m_axis_tlast  <= 1'b0;
                if (m_axis_tready) state <= S_TMAS_W7;
            end
            S_TMAS_W7: begin
                m_axis_tdata  <= scrambling_code_q;
                m_axis_tvalid <= 1'b1;
                m_axis_tkeep  <= 4'b1111;
                m_axis_tlast  <= 1'b0;
                if (m_axis_tready) state <= S_TMAS_W8;
            end
            S_TMAS_W8: begin
                m_axis_tdata  <= 32'h0;        // reserved
                m_axis_tvalid <= 1'b1;
                m_axis_tkeep  <= 4'b1111;
                m_axis_tlast  <= (pay_words_total == 5'b0); // pathological: pdu_len_bits=0
                if (m_axis_tready) begin
                    if (pay_words_total == 5'b0) begin
                        if (tlm_tmas_frames_cnt != 32'hFFFF_FFFF)
                            tlm_tmas_frames_cnt <= tlm_tmas_frames_cnt + 32'd1;
                        state <= S_IDLE;
                    end else begin
                        state <= S_TMAS_PAY;
                    end
                end
            end
            S_TMAS_PAY: begin
                m_axis_tdata  <= pay_word_at(pay_word_idx);
                m_axis_tvalid <= 1'b1;
                m_axis_tkeep  <= 4'b1111;
                m_axis_tlast  <= (pay_word_idx == (pay_words_total - 5'd1));
                if (m_axis_tready) begin
                    if (pay_word_idx == (pay_words_total - 5'd1)) begin
                        if (tlm_tmas_frames_cnt != 32'hFFFF_FFFF)
                            tlm_tmas_frames_cnt <= tlm_tmas_frames_cnt + 32'd1;
                        state <= S_IDLE;
                    end else begin
                        pay_word_idx <= pay_word_idx + 5'd1;
                    end
                end
            end
            // ---------------------------------------------------------
            // ---- TMAR emission ---------------------------------------
            S_TMAR_W0: begin
                m_axis_tdata  <= MAGIC_TMAR;
                m_axis_tvalid <= 1'b1;
                m_axis_tkeep  <= 4'b1111;
                m_axis_tlast  <= 1'b0;
                if (m_axis_tready) state <= S_TMAR_W1;
            end
            S_TMAR_W1: begin
                // [31:16] frame_len = 12, [15:0] reserved = 0
                m_axis_tdata  <= { 16'd12, 16'h0000 };
                m_axis_tvalid <= 1'b1;
                m_axis_tkeep  <= 4'b1111;
                m_axis_tlast  <= 1'b0;
                if (m_axis_tready) state <= S_TMAR_W2;
            end
            S_TMAR_W2: begin
                m_axis_tdata  <= req_handle_q;
                m_axis_tvalid <= 1'b1;
                m_axis_tkeep  <= 4'b1111;
                m_axis_tlast  <= 1'b0;
                if (m_axis_tready) state <= S_TMAR_W3;
            end
            S_TMAR_W3: begin
                // [31:24] report_code, [23:0] pad
                m_axis_tdata  <= { report_code_q, 24'h00_0000 };
                m_axis_tvalid <= 1'b1;
                m_axis_tkeep  <= 4'b1111;
                m_axis_tlast  <= 1'b1;
                if (m_axis_tready) begin
                    if (tlm_tmar_frames_cnt != 32'hFFFF_FFFF)
                        tlm_tmar_frames_cnt <= tlm_tmar_frames_cnt + 32'd1;
                    state <= S_IDLE;
                end
            end
            // ---------------------------------------------------------
            default: state <= S_IDLE;
            endcase
        end
    end

endmodule

`default_nettype wire
