// tb/rtl/tb_tmasap_tx_framer/tb_tmasap_tx_framer.v
//
// Owned by Agent A2 (A2-fpga-tmasap-framer).
//
// AXIS-in -> per-byte stream TX-framer TB.
//
// Test 1 (Gold-Ref D-LOC-UPDATE-ACCEPT shape):
//   Driver injects an AXIS stream:
//     beat 0   : TMAS magic (0x544D4153)
//     beat 1   : { frame_len=50 (0x32, BE), pdu_len_bits=112 (0x70 BE) }
//     beats 2..8 : meta header words (ssi=0x282FF4, scrambling_code=...)
//     beats 9..12: 14-octet D-LOC-UPDATE-ACCEPT MM-body bytes
//                  (test pattern: 0xA0 0xA1 0xA2 ... 0xAD)
//   Total declared frame_len = 36 + 14 = 50 bytes; on-the-wire =
//   ceil(50/4)*4 = 52 bytes = 13 beats with the last beat half-padded.
//   Assert:
//     - 14 mb_byte_* outputs match A0..AD byte-for-byte.
//     - mb_pdu_len_bits = 112, mb_ssi = 0x282FF4, mb_req_handle preserved.
//     - mb_frame_start_pulse / mb_frame_end_pulse fire exactly once each.
//     - tlm_tmasap_tx_frames_cnt == 1, tlm_tmasap_tx_err_cnt == 0.
//
// Test 2 (bad magic): inject a single-beat frame whose first word is NOT
//   0x544D4153 (e.g. 0x12345678).  Assert mb_frame_error_pulse fires,
//   tlm_tmasap_tx_err_cnt increments, no mb_byte_valid was driven.
//
// Test 3 (length mismatch): inject a frame whose declared frame_len in
//   beat 1 disagrees with `36 + ceil(pdu_len_bits/8)`.  Assert
//   mb_frame_error_pulse and tlm_tmasap_tx_err_cnt += 1.
//
// Pass/fail contract: print exactly one line containing "PASS" on success.
//
// Verilog-2001 only.

`timescale 1ns / 1ps
`default_nettype none

module tb_tmasap_tx_framer;

    reg clk = 1'b0;
    reg rst_n = 1'b0;
    always #5 clk = ~clk;

    // ---------- DUT signals ------------------------------------------
    reg  [31:0] s_axis_tdata;
    reg         s_axis_tvalid;
    wire        s_axis_tready;
    reg         s_axis_tlast;
    reg  [3:0]  s_axis_tkeep;

    wire [10:0] mb_pdu_len_bits;
    wire [23:0] mb_ssi;
    wire [2:0]  mb_ssi_type;
    wire [7:0]  mb_flags;
    wire [11:0] mb_chan_alloc;
    wire [31:0] mb_endpoint_id;
    wire [31:0] mb_new_endpoint_id;
    wire [31:0] mb_css_endpoint_id;
    wire [31:0] mb_scrambling_code;
    wire [31:0] mb_req_handle;
    wire        mb_frame_start_pulse;
    wire [7:0]  mb_byte_data;
    wire        mb_byte_valid;
    reg         mb_byte_ready;
    wire        mb_frame_end_pulse;
    wire        mb_frame_error_pulse;
    wire [31:0] tlm_frames;
    wire [15:0] tlm_err;

    tetra_tmasap_tx_framer dut (
        .clk                       (clk),
        .rst_n                     (rst_n),
        .s_axis_tdata              (s_axis_tdata),
        .s_axis_tvalid             (s_axis_tvalid),
        .s_axis_tready             (s_axis_tready),
        .s_axis_tlast              (s_axis_tlast),
        .s_axis_tkeep              (s_axis_tkeep),
        .mb_pdu_len_bits           (mb_pdu_len_bits),
        .mb_ssi                    (mb_ssi),
        .mb_ssi_type               (mb_ssi_type),
        .mb_flags                  (mb_flags),
        .mb_chan_alloc             (mb_chan_alloc),
        .mb_endpoint_id            (mb_endpoint_id),
        .mb_new_endpoint_id        (mb_new_endpoint_id),
        .mb_css_endpoint_id        (mb_css_endpoint_id),
        .mb_scrambling_code        (mb_scrambling_code),
        .mb_req_handle             (mb_req_handle),
        .mb_frame_start_pulse      (mb_frame_start_pulse),
        .mb_byte_data              (mb_byte_data),
        .mb_byte_valid             (mb_byte_valid),
        .mb_byte_ready             (mb_byte_ready),
        .mb_frame_end_pulse        (mb_frame_end_pulse),
        .mb_frame_error_pulse      (mb_frame_error_pulse),
        .tlm_tmasap_tx_frames_cnt  (tlm_frames),
        .tlm_tmasap_tx_err_cnt     (tlm_err)
    );

    // ---------- Capture buffer for mb_byte_* stream ------------------
    reg [7:0] cap_buf [0:255];
    integer   cap_len;
    integer   n_start;
    integer   n_end;
    integer   n_err;

    task capture_reset;
        begin
            cap_len = 0;
            n_start = 0;
            n_end   = 0;
            n_err   = 0;
        end
    endtask

    always @(posedge clk) begin
        if (rst_n) begin
            if (mb_frame_start_pulse) n_start <= n_start + 1;
            if (mb_frame_end_pulse)   n_end   <= n_end + 1;
            if (mb_frame_error_pulse) n_err   <= n_err + 1;
            if (mb_byte_valid && mb_byte_ready) begin
                cap_buf[cap_len] <= mb_byte_data;
                cap_len <= cap_len + 1;
            end
        end
    end

    // ---------- AXIS driver helper -----------------------------------
    task axis_drive_beat;
        input [31:0] data;
        input [3:0]  keep;
        input        last;
        begin
            @(negedge clk);
            s_axis_tdata  = data;
            s_axis_tkeep  = keep;
            s_axis_tlast  = last;
            s_axis_tvalid = 1'b1;
            wait (s_axis_tready);
            @(posedge clk);
            @(negedge clk);
            s_axis_tvalid = 1'b0;
            s_axis_tlast  = 1'b0;
        end
    endtask

    integer i;

    initial begin
        s_axis_tdata  = 32'h0;
        s_axis_tvalid = 1'b0;
        s_axis_tlast  = 1'b0;
        s_axis_tkeep  = 4'b0000;
        mb_byte_ready = 1'b1;     // always-ready sink

        capture_reset();
        rst_n = 1'b0;
        repeat (4) @(posedge clk);
        rst_n = 1'b1;
        @(posedge clk);

        // ============== Test 1: Gold-Ref TMAS frame ==================
        // 14 MM-body bytes, pattern A0..AD.
        capture_reset();

        // beat 0: magic
        axis_drive_beat(32'h544D_4153, 4'b1111, 1'b0);
        // beat 1: frame_len=50 (=0x32), pdu_len_bits=112 (=0x70)
        axis_drive_beat({16'd50, 16'd112}, 4'b1111, 1'b0);
        // beat 2: ssi (24-bit MSB-aligned in 4 B): 00 28 2F F4
        axis_drive_beat({8'h00, 24'h28_2FF4}, 4'b1111, 1'b0);
        // beat 3: { 5'b0, ssi_type=1, flags=0x42, chan_alloc=0xABC, 4'b0 }
        axis_drive_beat({5'b0, 3'd1, 8'h42, 12'hABC, 4'b0}, 4'b1111, 1'b0);
        // beat 4: endpoint_id = 0x11223344
        axis_drive_beat(32'h1122_3344, 4'b1111, 1'b0);
        // beat 5: new_endpoint_id = 0x55667788
        axis_drive_beat(32'h5566_7788, 4'b1111, 1'b0);
        // beat 6: css_endpoint_id = 0x99AA_BBCC
        axis_drive_beat(32'h99AA_BBCC, 4'b1111, 1'b0);
        // beat 7: scrambling_code = 0x1234_5678
        axis_drive_beat(32'h1234_5678, 4'b1111, 1'b0);
        // beat 8: req_handle = 0xFEED_F00D
        axis_drive_beat(32'hFEED_F00D, 4'b1111, 1'b0);
        // beats 9..12: 14 MM-body bytes A0..AD.
        // beat 9 :  A0 A1 A2 A3
        axis_drive_beat({8'hA0, 8'hA1, 8'hA2, 8'hA3}, 4'b1111, 1'b0);
        // beat 10:  A4 A5 A6 A7
        axis_drive_beat({8'hA4, 8'hA5, 8'hA6, 8'hA7}, 4'b1111, 1'b0);
        // beat 11:  A8 A9 AA AB
        axis_drive_beat({8'hA8, 8'hA9, 8'hAA, 8'hAB}, 4'b1111, 1'b0);
        // beat 12:  AC AD 00 00 (last 2 bytes are wire-pad, declared len=50 excludes them) - tlast
        axis_drive_beat({8'hAC, 8'hAD, 8'h00, 8'h00}, 4'b1111, 1'b1);

        // wait for end-pulse
        wait (mb_frame_end_pulse);
        @(posedge clk);
        @(posedge clk);

        if (cap_len !== 14) begin
            $display("FAIL T1: cap_len=%0d expected 14", cap_len);
            $fatal;
        end
        for (i = 0; i < 14; i = i + 1) begin
            if (cap_buf[i] !== (8'hA0 + i[7:0])) begin
                $display("FAIL T1 byte[%0d]=0x%02h expected 0x%02h",
                         i, cap_buf[i], (8'hA0 + i[7:0]));
                $fatal;
            end
        end
        if (n_start !== 1) begin
            $display("FAIL T1: n_start=%0d expected 1", n_start);
            $fatal;
        end
        if (n_end !== 1) begin
            $display("FAIL T1: n_end=%0d expected 1", n_end);
            $fatal;
        end
        if (n_err !== 0) begin
            $display("FAIL T1: n_err=%0d expected 0", n_err);
            $fatal;
        end
        if (mb_pdu_len_bits !== 11'd112) begin
            $display("FAIL T1: mb_pdu_len_bits=%0d expected 112", mb_pdu_len_bits);
            $fatal;
        end
        if (mb_ssi !== 24'h28_2FF4) begin
            $display("FAIL T1: mb_ssi=0x%h expected 0x282FF4", mb_ssi);
            $fatal;
        end
        if (mb_ssi_type !== 3'd1) begin
            $display("FAIL T1: mb_ssi_type=%0d expected 1", mb_ssi_type);
            $fatal;
        end
        if (mb_flags !== 8'h42) begin
            $display("FAIL T1: mb_flags=0x%h expected 0x42", mb_flags);
            $fatal;
        end
        if (mb_chan_alloc !== 12'hABC) begin
            $display("FAIL T1: mb_chan_alloc=0x%h expected 0xABC", mb_chan_alloc);
            $fatal;
        end
        if (mb_endpoint_id !== 32'h1122_3344) begin
            $display("FAIL T1: mb_endpoint_id=0x%h", mb_endpoint_id);
            $fatal;
        end
        if (mb_new_endpoint_id !== 32'h5566_7788) begin
            $display("FAIL T1: mb_new_endpoint_id=0x%h", mb_new_endpoint_id);
            $fatal;
        end
        if (mb_css_endpoint_id !== 32'h99AA_BBCC) begin
            $display("FAIL T1: mb_css_endpoint_id=0x%h", mb_css_endpoint_id);
            $fatal;
        end
        if (mb_scrambling_code !== 32'h1234_5678) begin
            $display("FAIL T1: mb_scrambling_code=0x%h", mb_scrambling_code);
            $fatal;
        end
        if (mb_req_handle !== 32'hFEED_F00D) begin
            $display("FAIL T1: mb_req_handle=0x%h", mb_req_handle);
            $fatal;
        end
        if (tlm_frames !== 32'd1) begin
            $display("FAIL T1: tlm_frames=%0d expected 1", tlm_frames);
            $fatal;
        end
        if (tlm_err !== 16'd0) begin
            $display("FAIL T1: tlm_err=%0d expected 0", tlm_err);
            $fatal;
        end
        $display("[T1] Gold-Ref TMAS TX frame (14 MM-body octets): PASS");

        // ============== Test 2: bad magic =============================
        capture_reset();
        // first beat with bad magic, tlast asserted -> single-beat junk frame
        axis_drive_beat(32'h1234_5678, 4'b1111, 1'b1);
        @(posedge clk);
        @(posedge clk);
        @(posedge clk);

        if (cap_len !== 0) begin
            $display("FAIL T2: cap_len=%0d expected 0 (bad magic should not commit bytes)", cap_len);
            $fatal;
        end
        if (n_err !== 1) begin
            $display("FAIL T2: n_err=%0d expected 1", n_err);
            $fatal;
        end
        if (tlm_err !== 16'd1) begin
            $display("FAIL T2: tlm_err=%0d expected 1", tlm_err);
            $fatal;
        end
        if (tlm_frames !== 32'd1) begin
            $display("FAIL T2: tlm_frames=%0d expected still 1", tlm_frames);
            $fatal;
        end
        $display("[T2] Bad-magic frame dropped: PASS");

        // ============== Test 3: length mismatch =======================
        // Send a frame with frame_len=50 but pdu_len_bits=64 (mismatch).
        capture_reset();
        axis_drive_beat(32'h544D_4153, 4'b1111, 1'b0); // magic OK
        // declared frame_len=50, pdu_len_bits=64 -> expected frame_len=36+8=44, mismatch.
        axis_drive_beat({16'd50, 16'd64}, 4'b1111, 1'b0);
        // beat 2: ssi
        axis_drive_beat(32'h0028_2FF4, 4'b1111, 1'b0);
        // beat 3..8: dummy header words
        axis_drive_beat(32'h0, 4'b1111, 1'b0);
        axis_drive_beat(32'h0, 4'b1111, 1'b0);
        axis_drive_beat(32'h0, 4'b1111, 1'b0);
        axis_drive_beat(32'h0, 4'b1111, 1'b0);
        axis_drive_beat(32'h0, 4'b1111, 1'b0);
        axis_drive_beat(32'h0, 4'b1111, 1'b0); // beat 8 (req_handle) - mismatch caught here
        // payload pretend - the framer should be in S_DRAIN now
        axis_drive_beat(32'hDEAD_BEEF, 4'b1111, 1'b0);
        axis_drive_beat(32'hCAFE_BABE, 4'b1111, 1'b1); // tlast - terminate drain
        @(posedge clk);
        @(posedge clk);
        @(posedge clk);

        if (cap_len !== 0) begin
            $display("FAIL T3: cap_len=%0d expected 0 (length mismatch should not commit bytes)", cap_len);
            $fatal;
        end
        if (n_err !== 1) begin
            $display("FAIL T3: n_err=%0d expected 1", n_err);
            $fatal;
        end
        if (tlm_err !== 16'd2) begin
            $display("FAIL T3: tlm_err=%0d expected 2", tlm_err);
            $fatal;
        end
        if (tlm_frames !== 32'd1) begin
            $display("FAIL T3: tlm_frames=%0d expected still 1", tlm_frames);
            $fatal;
        end
        $display("[T3] Length-mismatch frame dropped: PASS");

        $display("PASS tb_tmasap_tx_framer (3/3 sub-tests)");
        $finish;
    end

    initial begin
        #200000;
        $display("FAIL tb_tmasap_tx_framer: watchdog timeout");
        $fatal;
    end

endmodule

`default_nettype wire
