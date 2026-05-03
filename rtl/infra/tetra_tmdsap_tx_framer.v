// rtl/infra/tetra_tmdsap_tx_framer.v
//
// Owned by Agent A3 (A3-fpga-tmdsap-framer).
//
// TmdSap-TX framer: bit-transparent voice-relay path
// (DMA-MM2S AXIS = PS→FPGA → LMAC channel-encode input).
//
// Mirror of `tetra_tmdsap_rx_framer.v`.  Receives a TMDC frame on AXIS-in
// (A1's `m_axis_tmd_tx_*` master port = PS→FPGA), validates the magic
// (0x544D_4443, Decision #4) and the length-prefix (must equal the
// fixed 0x0000_003E = 62 bytes), and emits the embedded 432-bit voice
// half-block as a single-cycle parallel bus to the LMAC TX channel-
// encode chain.
//
// Frame format on AXIS-in (network byte order, big-endian; matches the
// `tetra_dma_frame_unpacker` 8-byte header convention from A1):
//
//     beat[0]  bytes 0..3  = 0x544D_4443  ("TMDC")
//     beat[1]  bytes 4..7  = 0x0000_003E  (uint32 BE: total frame length)
//     beat[2..14] bytes 8..59 = payload bytes 0..51   (13 full beats)
//     beat[15] bytes 60..61 = payload bytes 52..53    (partial; tkeep=1100)
//     tlast must be HIGH on beat[15] only.
//
// Bit ordering on `out_nub_bits[431:0]`: MSB-first, identical to the RX
// framer.  Bit `[431]` (= bit 7 of payload byte 0) is the first on-air
// bit, matching the carry-over LMAC convention.
//
// Error handling:
//   - bad_magic: beat[0] does not equal 0x544D_4443.  Increments
//     `tlm_err_count` and discards bytes until tlast.
//   - bad_length: beat[1] is not 0x0000_003E.  Same recovery (drop until
//     tlast), increments `tlm_err_count`.
//   - short/long frame (tlast arrives before beat 15 / not by beat 15):
//     same recovery, increments `tlm_err_count`.
//
// LMAC-side handshake (out_nub_bits, out_valid, out_ready):
//   - On a valid frame, the framer asserts `out_valid` for one cycle with
//     the assembled 432-bit half-block.  The LMAC must sample on this
//     cycle (or hold `out_ready` LOW to back-pressure; the framer waits
//     in the OUT-DRIVE state until ready & valid both HIGH).
//   - Single-beat handshake; symmetric to the RX framer.
//
// LMAC port shape — TODO MARKER ------------------------------------------
//   Same as the RX framer: the TCH/S TX path in carry-over LMAC is TBD
//   (see `docs/PROTOCOL.md`).  We expose a clean placeholder bus.
//
//     <-- TODO: confirm LMAC TCH/S port shape with A5 when tetra_top.v
//                wires it -->
//
// Verilog-2001 only (CLAUDE.md §Languages).

`timescale 1ns / 1ps
`default_nettype none

module tetra_tmdsap_tx_framer (
    input  wire                 clk,
    input  wire                 rst_n,

    // ---- AXIS slave from A1's `m_axis_tmd_tx_*` (PS→FPGA).
    input  wire [31:0]          s_axis_tdata,
    input  wire                 s_axis_tvalid,
    output wire                 s_axis_tready,
    input  wire                 s_axis_tlast,
    input  wire [3:0]           s_axis_tkeep,

    // ---- LMAC-side output: bit-transparent 432-bit voice half-block.
    output reg  [431:0]         out_nub_bits,
    output reg                  out_valid,
    input  wire                 out_ready,

    // ---- Telemetry to A5 register window.
    output wire [31:0]          tlm_tx_frames,
    output wire [31:0]          tlm_err_count
);

    // ---- locked frame constants ------------------------------------------
    localparam [31:0] MAGIC_TMDC      = 32'h544D_4443;
    localparam [31:0] EXPECTED_LEN_B  = 32'd62;          // 4+4+54

    // Beats:
    //   0    = magic
    //   1    = length
    //   2..14 = payload bytes 0..51
    //   15   = payload bytes 52..53 (partial)

    // ---- FSM -------------------------------------------------------------
    localparam [2:0] S_HDR_M  = 3'd0;
    localparam [2:0] S_HDR_L  = 3'd1;
    localparam [2:0] S_PAY    = 3'd2;
    localparam [2:0] S_TAIL   = 3'd3;
    localparam [2:0] S_DRIVE  = 3'd4;   // present out_nub_bits to LMAC
    localparam [2:0] S_DRAIN  = 3'd5;   // discard remainder after error
    localparam [2:0] S_DONE_W = 3'd6;   // post-frame wait/cleanup

    reg [2:0]   state;
    reg [3:0]   pay_beat_idx;       // 0..12: tracks beats 2..14 in S_PAY
    reg [431:0] payload_acc;        // accumulator for the 432-bit NUB

    reg [31:0]  frames_cnt;
    reg [31:0]  err_cnt;

    assign tlm_tx_frames = frames_cnt;
    assign tlm_err_count = err_cnt;

    // tready: accept beats while parsing.  When DRIVE-ing the LMAC bus we
    // are not consuming AXIS, so tready=0; same for DRAIN we keep tready=1
    // (consume + discard).  In DONE_W (handshake-finalize) we hold off.
    assign s_axis_tready = (state == S_HDR_M) || (state == S_HDR_L) ||
                           (state == S_PAY)   || (state == S_TAIL)  ||
                           (state == S_DRAIN);

    // Helper: latch 4 payload bytes from a beat into the accumulator.
    // `pay_beat_idx` ∈ 0..12 ; full-beat covers payload bytes
    //     [pay_beat_idx*4 .. pay_beat_idx*4 + 3]
    // bits (MSB-first):  payload_acc[431 - 8*base -: 32]
    task absorb_full_beat;
        input integer base_byte;        // 0..48
        begin
            payload_acc[431 - 8*base_byte       -: 8] <= s_axis_tdata[31:24];
            payload_acc[431 - 8*(base_byte + 1) -: 8] <= s_axis_tdata[23:16];
            payload_acc[431 - 8*(base_byte + 2) -: 8] <= s_axis_tdata[15: 8];
            payload_acc[431 - 8*(base_byte + 3) -: 8] <= s_axis_tdata[ 7: 0];
        end
    endtask

    task absorb_tail_beat;
        // Final beat: 2 bytes valid, in upper 2 lanes (tkeep checked by caller).
        // base_byte = 52
        begin
            payload_acc[431 - 8*52 -: 8] <= s_axis_tdata[31:24];
            payload_acc[431 - 8*53 -: 8] <= s_axis_tdata[23:16];
            // lower 2 bytes ignored (tkeep should be 4'b1100)
        end
    endtask

    // ---- main FSM --------------------------------------------------------
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state         <= S_HDR_M;
            pay_beat_idx  <= 4'd0;
            payload_acc   <= 432'h0;
            out_nub_bits  <= 432'h0;
            out_valid     <= 1'b0;
            frames_cnt    <= 32'h0;
            err_cnt       <= 32'h0;
        end else begin
            case (state)
            // -------------------------------------------------------------
            S_HDR_M: begin
                out_valid <= 1'b0;
                if (s_axis_tvalid && s_axis_tready) begin
                    if (s_axis_tdata == MAGIC_TMDC && !s_axis_tlast) begin
                        state <= S_HDR_L;
                    end else begin
                        // Bad magic — or unexpected tlast on header word.
                        if (err_cnt != 32'hFFFF_FFFF)
                            err_cnt <= err_cnt + 32'd1;
                        if (s_axis_tlast) begin
                            // Single-beat garbage frame; back to fresh start.
                            state <= S_HDR_M;
                        end else begin
                            state <= S_DRAIN;
                        end
                    end
                end
            end
            // -------------------------------------------------------------
            S_HDR_L: begin
                if (s_axis_tvalid && s_axis_tready) begin
                    if (s_axis_tdata == EXPECTED_LEN_B && !s_axis_tlast) begin
                        pay_beat_idx <= 4'd0;
                        state        <= S_PAY;
                    end else begin
                        if (err_cnt != 32'hFFFF_FFFF)
                            err_cnt <= err_cnt + 32'd1;
                        if (s_axis_tlast) begin
                            state <= S_HDR_M;
                        end else begin
                            state <= S_DRAIN;
                        end
                    end
                end
            end
            // -------------------------------------------------------------
            S_PAY: begin
                if (s_axis_tvalid && s_axis_tready) begin
                    // Expect FULL beats (tkeep=1111) for indices 0..12.
                    if (s_axis_tkeep != 4'b1111) begin
                        // Premature partial / sparse beat — error.
                        if (err_cnt != 32'hFFFF_FFFF)
                            err_cnt <= err_cnt + 32'd1;
                        if (s_axis_tlast) begin
                            state <= S_HDR_M;
                        end else begin
                            state <= S_DRAIN;
                        end
                    end else begin
                        absorb_full_beat(pay_beat_idx * 4);
                        if (pay_beat_idx == 4'd12) begin
                            // 13th full payload beat consumed → expect tail next.
                            // Must NOT have tlast on this beat (tail still owed).
                            if (s_axis_tlast) begin
                                if (err_cnt != 32'hFFFF_FFFF)
                                    err_cnt <= err_cnt + 32'd1;
                                state <= S_HDR_M;
                            end else begin
                                state <= S_TAIL;
                            end
                        end else begin
                            // tlast must be LOW for non-final beats.
                            if (s_axis_tlast) begin
                                if (err_cnt != 32'hFFFF_FFFF)
                                    err_cnt <= err_cnt + 32'd1;
                                state <= S_HDR_M;
                            end else begin
                                pay_beat_idx <= pay_beat_idx + 4'd1;
                            end
                        end
                    end
                end
            end
            // -------------------------------------------------------------
            S_TAIL: begin
                if (s_axis_tvalid && s_axis_tready) begin
                    // Expect tlast=1 with tkeep=4'b1100.
                    if (s_axis_tlast && s_axis_tkeep == 4'b1100) begin
                        absorb_tail_beat();
                        // hand off to LMAC
                        state <= S_DRIVE;
                    end else begin
                        if (err_cnt != 32'hFFFF_FFFF)
                            err_cnt <= err_cnt + 32'd1;
                        if (s_axis_tlast) begin
                            state <= S_HDR_M;
                        end else begin
                            state <= S_DRAIN;
                        end
                    end
                end
            end
            // -------------------------------------------------------------
            S_DRIVE: begin
                // Present the assembled 432-bit half-block to LMAC.
                // Two-step pattern (avoids same-cycle NB-race on out_valid):
                //   1) on entry to S_DRIVE: out_valid registered HIGH while
                //      we wait for `out_ready & out_valid` to coincide.
                //   2) on the cycle the slave latches (out_valid==1 already
                //      AND out_ready==1), we deassert out_valid<=0 and
                //      advance to S_HDR_M.  Because out_valid is checked
                //      BEFORE we possibly clear it, the NB-write order does
                //      not race.
                out_nub_bits <= payload_acc;
                if (out_valid && out_ready) begin
                    // Handshake completes THIS cycle (slave latches on this
                    // posedge).  Tear down for next cycle.
                    out_valid <= 1'b0;
                    if (frames_cnt != 32'hFFFF_FFFF)
                        frames_cnt <= frames_cnt + 32'd1;
                    state <= S_HDR_M;
                end else begin
                    // Either entry cycle (out_valid still registered 0) or
                    // slave back-pressure: keep asserting until ready.
                    out_valid <= 1'b1;
                end
            end
            // -------------------------------------------------------------
            S_DRAIN: begin
                // Discard remaining beats until tlast.
                if (s_axis_tvalid && s_axis_tready && s_axis_tlast) begin
                    state <= S_HDR_M;
                end
            end
            // -------------------------------------------------------------
            default: state <= S_HDR_M;
            endcase
        end
    end

endmodule

`default_nettype wire
