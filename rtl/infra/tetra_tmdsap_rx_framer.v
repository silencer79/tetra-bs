// rtl/infra/tetra_tmdsap_rx_framer.v
//
// Owned by Agent A3 (A3-fpga-tmdsap-framer).
//
// TmdSap-RX framer: bit-transparent voice-relay path
// (LMAC channel-decode output → DMA-S2MM AXIS = FPGA→PS).
//
// Per CLAUDE.md "TX path validated, hands-off" memory and Decision #4
// (TMDC magic = 0x544D_4443), the FPGA does NOT perform any ACELP encode/
// decode on TCH/S voice — it shovels half-blocks (NUB = 432-bit) bit-
// transparent between PS DDR and the LMAC TX/RX chain. SW (CMCE) is the
// only owner of the 274-bit ACELP frames; carrying the post-channel-
// decode raw 432-bit half-block keeps the encoder/decoder symmetry on
// air and avoids re-implementing ACELP in two places.
//
// Frame format on AXIS-out (network byte order, big-endian; matches the
// `tetra_dma_frame_packer` 8-byte self-describing header convention from
// A1):
//
//     beat[0]  bytes 0..3  = 0x544D_4443  ("TMDC")
//     beat[1]  bytes 4..7  = 0x0000_003E  (uint32 BE: total frame length)
//     beat[2..15] bytes 8..61 = 54-byte voice payload (= 432 bits MSB-first,
//                                with the lower 4 bits of the LAST payload
//                                byte zero-padded — see "Payload encoding"
//                                below).
//     tlast asserted on the final beat (beat[15]).
//     tkeep on beat[15] = 4'b1100 (only top 2 bytes valid since 62 mod 4 = 2).
//
// Payload encoding (decision: BYTE-ALIGNED) ------------------------------
//   - The NUB carries 432 bits.  432 / 8 = 54 bytes exactly, so byte
//     alignment is perfect and we incur ZERO pad bits at the byte
//     boundary.  The TOTAL frame length is therefore 4 (magic) + 4 (len) +
//     54 (payload) = 62 bytes (not a multiple of 4 → final beat partial).
//   - Bit ordering inside the payload bytes is MSB-first: bit `payload[431]`
//     (= LMAC port `in_nub_bits[431]`, the first on-air bit, matching the
//     existing SCH/F encoder convention `coded_bits[431]` = first on-air
//     bit per `rtl/lmac/tetra_sch_f_encoder.v`) lands in bit 7 of payload
//     byte 0; bit `[424]` lands in bit 0 of payload byte 0; etc.  Bit `[0]`
//     (last on-air bit) lands in bit 0 of payload byte 53.
//   - This is the simplest LMAC interface because every existing LMAC RTL
//     block already uses MSB-first parallel buses (`coded_bits[431:0]`,
//     `pdu_bits[267:0]`, `info_bits[123:0]`).  A bit-aligned (serial)
//     LMAC interface would need a 432-stage serializer whose semantics
//     duplicate what the burst muxer already does internally.
//
// LMAC-side handshake (in_nub_bits, in_valid, in_ready):
//   - `in_valid` HIGH for ≥1 cycle when the LMAC channel-decoder has a
//     full 432-bit half-block available.
//   - `in_ready` HIGH when the framer is idle and able to accept.
//   - Single-beat handshake: data is captured on the first cycle where
//     valid AND ready are both HIGH; the framer then drains the AXIS side
//     before re-asserting in_ready.
//   - Bit ordering on `in_nub_bits[431:0]`: MSB-first (bit `[431]` = first
//     on-air bit).  Matches `coded_bits[431:0]` from the SCH/F encoder.
//
// LMAC port shape — TODO MARKER ------------------------------------------
//   The carry-over `rtl/lmac/` does NOT (yet) expose a TCH/S half-block
//   port.  `docs/PROTOCOL.md` says: "TCH/S … RTL TBD when CMCE-Voice-Path
//   implemented".  We therefore define a CLEAN PLACEHOLDER interface here
//   (in_nub_bits[431:0] + in_valid/in_ready), document it in
//   `docs/references/tmdsap_port_contract.md`, and leave the marker:
//
//     <-- TODO: confirm LMAC TCH/S port shape with A5 when tetra_top.v
//                wires it -->
//
//   When A5 plumbs `tetra_top.v` and the TCH/S decoder lands, the connection
//   is a single-cycle parallel bus + valid/ready — the simplest possible
//   shape and the one already used by SCH/F.
//
// Telemetry counters (driven up to A5 register window; see
// `docs/ARCHITECTURE.md` §"AXI-Lite Live-Config Register Window" addendum):
//
//     REG_TMDSAP_TX_FRAMES_CNT  @ 0x170  (companion TX framer)
//     REG_TMDSAP_RX_FRAMES_CNT  @ 0x174  (this module)
//     REG_TMDSAP_ERR_CNT        @ 0x178  (TX framer; bad-magic / len-mismatch)
//
// Verilog-2001 only (CLAUDE.md §Languages).

`timescale 1ns / 1ps
`default_nettype none

module tetra_tmdsap_rx_framer (
    input  wire                 clk,
    input  wire                 rst_n,

    // ---- LMAC-side input: bit-transparent 432-bit voice half-block.
    //      MSB-first, single-cycle latch when valid & ready.
    input  wire [431:0]         in_nub_bits,
    input  wire                 in_valid,
    output wire                 in_ready,

    // ---- AXIS master to A1's `s_axis_tmd_rx_*` slave port (FPGA→PS).
    output reg  [31:0]          m_axis_tdata,
    output reg                  m_axis_tvalid,
    input  wire                 m_axis_tready,
    output reg                  m_axis_tlast,
    output reg  [3:0]           m_axis_tkeep,

    // ---- Telemetry to A5 register window.
    output wire [31:0]          tlm_rx_frames
);

    // ---- locked frame constants ------------------------------------------
    localparam [31:0] MAGIC_TMDC      = 32'h544D_4443;
    localparam [15:0] PAYLOAD_BYTES   = 16'd54;          // 432 / 8
    localparam [31:0] TOTAL_LEN_BYTES = 32'd62;          // 4 + 4 + 54
    // Total wire layout = 16 beats:
    //   beat 0    = magic
    //   beat 1    = length
    //   beats 2..14 = payload bytes 0..51   (13 full beats × 4 bytes = 52 bytes)
    //   beat 15   = payload bytes 52..53   (partial, tkeep = 4'b1100)
    localparam integer NUM_BEATS_FULL    = 15;  // beats 0..14 are full (tkeep=1111)
    localparam integer NUM_BEATS_TOTAL   = 16;  // 0..15 inclusive, tlast on 15

    // ---- FSM -------------------------------------------------------------
    localparam [1:0] S_IDLE = 2'd0;
    localparam [1:0] S_EMIT = 2'd1;

    reg [1:0]   state;
    reg [4:0]   beat_idx;          // 0..15
    reg [431:0] payload_q;         // latched 432-bit NUB

    // Frame counter (saturating at 32'hFFFF_FFFF — 32 bits is plenty).
    reg [31:0]  frames_cnt;

    assign in_ready     = (state == S_IDLE);
    assign tlm_rx_frames = frames_cnt;

    // ---- payload byte indexing helper -----------------------------------
    // payload_byte(i) = bits [432-8*i-1 : 432-8*(i+1)] from MSB-first vector
    //                 = payload_q[431 - 8*i  -:  8]  (Verilog -:select)
    // We use 4-byte beats for beats 2..14 and a 2-byte partial in beat 15.
    function [31:0] beat_word;
        input integer beat_no;          // 2..15
        integer base_byte;              // 0..52
        begin
            // base_byte = (beat_no - 2) * 4
            base_byte = (beat_no - 2) * 4;
            // For beats 2..14 all 4 bytes are valid.
            // For beat 15 only bytes 52,53 are valid; the lower 2 lanes are
            // driven 0 (and gated by tkeep).
            if (beat_no <= 14) begin
                beat_word = {
                    payload_q[431 - 8*base_byte       -: 8],   // byte 0 (MSB lane)
                    payload_q[431 - 8*(base_byte + 1) -: 8],
                    payload_q[431 - 8*(base_byte + 2) -: 8],
                    payload_q[431 - 8*(base_byte + 3) -: 8]
                };
            end else begin
                // beat_no == 15: payload bytes 52, 53 in upper two lanes.
                beat_word = {
                    payload_q[431 - 8*52 -: 8],
                    payload_q[431 - 8*53 -: 8],
                    8'h00,
                    8'h00
                };
            end
        end
    endfunction

    // ---- main FSM --------------------------------------------------------
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state         <= S_IDLE;
            beat_idx      <= 5'd0;
            payload_q     <= 432'h0;
            m_axis_tdata  <= 32'h0;
            m_axis_tvalid <= 1'b0;
            m_axis_tlast  <= 1'b0;
            m_axis_tkeep  <= 4'b0000;
            frames_cnt    <= 32'h0;
        end else begin
            case (state)
            // -------------------------------------------------------------
            S_IDLE: begin
                m_axis_tvalid <= 1'b0;
                m_axis_tlast  <= 1'b0;
                m_axis_tkeep  <= 4'b0000;
                if (in_valid) begin
                    payload_q <= in_nub_bits;
                    beat_idx  <= 5'd0;
                    state     <= S_EMIT;
                end
            end
            // -------------------------------------------------------------
            S_EMIT: begin
                // Drive current beat.
                m_axis_tvalid <= 1'b1;
                if (beat_idx == 5'd0) begin
                    m_axis_tdata <= MAGIC_TMDC;
                    m_axis_tkeep <= 4'b1111;
                    m_axis_tlast <= 1'b0;
                end else if (beat_idx == 5'd1) begin
                    m_axis_tdata <= TOTAL_LEN_BYTES;
                    m_axis_tkeep <= 4'b1111;
                    m_axis_tlast <= 1'b0;
                end else if (beat_idx <= 5'd14) begin
                    m_axis_tdata <= beat_word(beat_idx);
                    m_axis_tkeep <= 4'b1111;
                    m_axis_tlast <= 1'b0;
                end else begin // beat_idx == 15 → final partial beat
                    m_axis_tdata <= beat_word(5'd15);
                    m_axis_tkeep <= 4'b1100;   // upper 2 lanes only
                    m_axis_tlast <= 1'b1;
                end

                // Advance only when the slave latches the current beat.
                // Note: do NOT override the AXIS-output registers here —
                // the NB writes earlier in this case-branch already drive
                // the final-beat values, and the next cycle's S_IDLE
                // branch zeros them.  Overriding tvalid/tlast/tkeep
                // here would race with the final-beat NB and the slave
                // would never see the final beat.
                if (m_axis_tready) begin
                    if (beat_idx == 5'd15) begin
                        // Frame complete.
                        if (frames_cnt != 32'hFFFF_FFFF)
                            frames_cnt <= frames_cnt + 32'd1;
                        state    <= S_IDLE;
                        beat_idx <= 5'd0;
                    end else begin
                        beat_idx <= beat_idx + 5'd1;
                    end
                end
            end
            // -------------------------------------------------------------
            default: state <= S_IDLE;
            endcase
        end
    end

endmodule

`default_nettype wire
