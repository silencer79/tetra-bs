// tb/rtl/tb_tmdsap_tx_framer/tb_tmdsap_tx_framer.v
//
// Owned by Agent A3 (A3-fpga-tmdsap-framer).
//
// Bit-level unpack TB for `tetra_tmdsap_tx_framer`.  Drives a TMDC
// frame on the AXIS-slave port:
//
//     [0x544D_4443][0x0000_003E][54-byte payload (54 = len 62 - 8 hdr)]
//
// (16 beats: 15 full @ tkeep=4'b1111 + 1 tail @ tkeep=4'b1100 with tlast.)
//
// Asserts:
//   - DUT emits exactly the original 432-bit half-block on out_nub_bits
//     (MSB-first bit-ordering, byte-by-byte equality with the input
//     payload).
//   - tlm_tx_frames increments to 1.
//
// Then drives an error-frame:
//   - bad-magic frame → tlm_err_count increments, no out_valid pulse.
// Then drives a length-mismatch frame:
//   - magic OK but length=0xDEAD → tlm_err_count further increments,
//     no out_valid pulse.
// Then drives a final good frame to confirm post-error recovery.
//
// Pass/fail contract: print one line containing "PASS" on success;
// any FAIL → $fatal.
//
// Verilog-2001 only.

`timescale 1ns / 1ps
`default_nettype none

module tb_tmdsap_tx_framer;

    // -----------------------------------------------------------------
    // Clock / reset
    // -----------------------------------------------------------------
    reg clk = 1'b0;
    reg rst_n = 1'b0;
    always #5 clk = ~clk;        // 100 MHz nominal

    // -----------------------------------------------------------------
    // DUT signals
    // -----------------------------------------------------------------
    reg  [31:0]   s_axis_tdata;
    reg           s_axis_tvalid;
    wire          s_axis_tready;
    reg           s_axis_tlast;
    reg  [3:0]    s_axis_tkeep;

    wire [431:0]  out_nub_bits;
    wire          out_valid;
    reg           out_ready;

    wire [31:0]   tlm_tx_frames;
    wire [31:0]   tlm_err_count;

    tetra_tmdsap_tx_framer dut (
        .clk            (clk),
        .rst_n          (rst_n),
        .s_axis_tdata   (s_axis_tdata),
        .s_axis_tvalid  (s_axis_tvalid),
        .s_axis_tready  (s_axis_tready),
        .s_axis_tlast   (s_axis_tlast),
        .s_axis_tkeep   (s_axis_tkeep),
        .out_nub_bits   (out_nub_bits),
        .out_valid      (out_valid),
        .out_ready      (out_ready),
        .tlm_tx_frames  (tlm_tx_frames),
        .tlm_err_count  (tlm_err_count)
    );

    // -----------------------------------------------------------------
    // Reference NUB (54 bytes, alternating 0xAA/0x55, packed MSB-first
    // into a 432-bit vector).
    // -----------------------------------------------------------------
    reg [7:0]   ref_payload [0:53];
    reg [431:0] ref_nub;
    integer     k;

    task build_ref_payload_aa55;
        integer kk;
        begin
            for (kk = 0; kk < 54; kk = kk + 1) begin
                ref_payload[kk] = (kk[0] == 1'b0) ? 8'hAA : 8'h55;
            end
        end
    endtask

    task pack_ref_into_nub;
        integer kk;
        begin
            for (kk = 0; kk < 54; kk = kk + 1) begin
                ref_nub[431 - 8*kk -: 8] = ref_payload[kk];
            end
        end
    endtask

    // -----------------------------------------------------------------
    // AXIS master driver: send one 32-bit beat with tlast/tkeep.
    // -----------------------------------------------------------------
    task axis_send_beat;
        input [31:0] data;
        input        last;
        input [3:0]  keep;
        begin
            @(negedge clk);
            s_axis_tdata  = data;
            s_axis_tvalid = 1'b1;
            s_axis_tlast  = last;
            s_axis_tkeep  = keep;
            // Wait until DUT asserts ready (level-sensitive).
            wait (s_axis_tready);
            @(posedge clk);
            // Beat is captured at this posedge.
        end
    endtask

    task axis_idle;
        begin
            @(negedge clk);
            s_axis_tvalid = 1'b0;
            s_axis_tlast  = 1'b0;
            s_axis_tkeep  = 4'b0000;
            s_axis_tdata  = 32'h0;
        end
    endtask

    // -----------------------------------------------------------------
    // Build a single TMDC frame and send it.
    // -----------------------------------------------------------------
    task drive_good_frame;
        // Uses ref_payload[] for the payload bytes.
        integer beat;
        reg [31:0] beat_data;
        integer base;
        begin
            // beat 0 = magic
            axis_send_beat(32'h544D_4443, 1'b0, 4'b1111);
            // beat 1 = total length (62)
            axis_send_beat(32'h0000_003E, 1'b0, 4'b1111);
            // beats 2..14 (13 beats) = full payload bytes 0..51
            for (beat = 0; beat < 13; beat = beat + 1) begin
                base = beat * 4;
                beat_data = { ref_payload[base + 0],
                              ref_payload[base + 1],
                              ref_payload[base + 2],
                              ref_payload[base + 3] };
                axis_send_beat(beat_data, 1'b0, 4'b1111);
            end
            // beat 15 = tail (bytes 52, 53 in upper 2 lanes), tlast=1, tkeep=1100
            beat_data = { ref_payload[52], ref_payload[53], 8'h00, 8'h00 };
            axis_send_beat(beat_data, 1'b1, 4'b1100);
            axis_idle();
        end
    endtask

    task drive_bad_magic_frame;
        integer beat;
        reg [31:0] beat_data;
        integer base;
        begin
            // beat 0 = WRONG magic ("TMAS" instead of "TMDC")
            axis_send_beat(32'h544D_4153, 1'b0, 4'b1111);
            // The DUT should now drain.  Send one more beat with tlast=1
            // to terminate the (rejected) frame in DRAIN.
            axis_send_beat(32'hDEAD_BEEF, 1'b1, 4'b1111);
            axis_idle();
        end
    endtask

    task drive_bad_length_frame;
        begin
            // beat 0 = correct magic
            axis_send_beat(32'h544D_4443, 1'b0, 4'b1111);
            // beat 1 = WRONG length 0x0000_DEAD instead of 0x0000_003E
            axis_send_beat(32'h0000_DEAD, 1'b0, 4'b1111);
            // Drain with a tlast beat.
            axis_send_beat(32'hCAFE_BABE, 1'b1, 4'b1111);
            axis_idle();
        end
    endtask

    // -----------------------------------------------------------------
    // Verification helpers
    // -----------------------------------------------------------------
    task check_nub;
        // Compares out_nub_bits to ref_nub byte-by-byte.
        integer kk;
        reg [7:0] got;
        reg [7:0] exp;
        begin
            for (kk = 0; kk < 54; kk = kk + 1) begin
                got = out_nub_bits[431 - 8*kk -: 8];
                exp = ref_payload[kk];
                if (got !== exp) begin
                    $display("FAIL out_nub_bits byte[%0d] = 0x%02h, expected 0x%02h",
                             kk, got, exp);
                    $fatal;
                end
            end
        end
    endtask

    // -----------------------------------------------------------------
    // out_valid observer: latches the most recent NUB on a valid handshake.
    // -----------------------------------------------------------------
    reg          saw_out_valid;
    reg [431:0]  saw_out_nub;

    always @(posedge clk) begin
        if (rst_n && out_valid && out_ready) begin
            saw_out_valid <= 1'b1;
            saw_out_nub   <= out_nub_bits;
        end
    end

    // -----------------------------------------------------------------
    // Test sequence
    // -----------------------------------------------------------------
    initial begin
        // Init
        s_axis_tdata  = 32'h0;
        s_axis_tvalid = 1'b0;
        s_axis_tlast  = 1'b0;
        s_axis_tkeep  = 4'b0000;
        out_ready     = 1'b1;        // always-ready LMAC
        saw_out_valid = 1'b0;
        saw_out_nub   = 432'h0;

        // Reset
        rst_n = 1'b0;
        repeat (4) @(posedge clk);
        rst_n = 1'b1;
        @(posedge clk);

        // ---------- Test 1: byte-identical good frame -------------------
        build_ref_payload_aa55();
        pack_ref_into_nub();
        saw_out_valid = 1'b0;
        drive_good_frame();
        // Wait for DUT to emit out_valid handshake.
        wait (saw_out_valid);
        @(posedge clk);

        if (saw_out_nub !== ref_nub) begin
            $display("FAIL T1: saw_out_nub != ref_nub (top=0x%h ref=0x%h)",
                     saw_out_nub[431:416], ref_nub[431:416]);
            $fatal;
        end
        check_nub();
        if (tlm_tx_frames !== 32'd1) begin
            $display("FAIL T1: tlm_tx_frames=%0d expected 1", tlm_tx_frames);
            $fatal;
        end
        if (tlm_err_count !== 32'd0) begin
            $display("FAIL T1: tlm_err_count=%0d expected 0", tlm_err_count);
            $fatal;
        end
        $display("[T1] good TMDC frame → 432-bit NUB byte-identical: PASS");

        // ---------- Test 2: bad-magic frame increments err_count --------
        saw_out_valid = 1'b0;
        drive_bad_magic_frame();
        repeat (8) @(posedge clk);

        if (saw_out_valid) begin
            $display("FAIL T2: out_valid pulsed on a bad-magic frame");
            $fatal;
        end
        if (tlm_err_count === 32'd0) begin
            $display("FAIL T2: tlm_err_count did not increment on bad magic (=%0d)",
                     tlm_err_count);
            $fatal;
        end
        if (tlm_tx_frames !== 32'd1) begin
            $display("FAIL T2: tlm_tx_frames changed (=%0d, expected 1)",
                     tlm_tx_frames);
            $fatal;
        end
        $display("[T2] bad-magic frame: err_count=%0d frames=%0d: PASS",
                 tlm_err_count, tlm_tx_frames);

        // ---------- Test 3: length-mismatch frame -----------------------
        saw_out_valid = 1'b0;
        drive_bad_length_frame();
        repeat (8) @(posedge clk);

        if (saw_out_valid) begin
            $display("FAIL T3: out_valid pulsed on a bad-length frame");
            $fatal;
        end
        if (tlm_err_count !== 32'd2) begin
            $display("FAIL T3: tlm_err_count=%0d expected 2", tlm_err_count);
            $fatal;
        end
        if (tlm_tx_frames !== 32'd1) begin
            $display("FAIL T3: tlm_tx_frames=%0d expected 1", tlm_tx_frames);
            $fatal;
        end
        $display("[T3] length-mismatch frame: err_count=%0d frames=%0d: PASS",
                 tlm_err_count, tlm_tx_frames);

        // ---------- Test 4: post-error recovery — second good frame ----
        // Use a different payload to ensure the latch is fresh.
        for (k = 0; k < 54; k = k + 1) begin
            ref_payload[k] = (k[0] == 1'b0) ? 8'hC3 : 8'h3C;
        end
        pack_ref_into_nub();
        saw_out_valid = 1'b0;
        drive_good_frame();
        wait (saw_out_valid);
        @(posedge clk);

        if (saw_out_nub !== ref_nub) begin
            $display("FAIL T4: saw_out_nub != ref_nub on recovery frame");
            $fatal;
        end
        check_nub();
        if (tlm_tx_frames !== 32'd2) begin
            $display("FAIL T4: tlm_tx_frames=%0d expected 2", tlm_tx_frames);
            $fatal;
        end
        if (tlm_err_count !== 32'd2) begin
            $display("FAIL T4: tlm_err_count=%0d expected 2 (unchanged)",
                     tlm_err_count);
            $fatal;
        end
        $display("[T4] post-error recovery good frame: PASS");

        $display("PASS tb_tmdsap_tx_framer (4/4 sub-tests)");
        $finish;
    end

    // Watchdog — bail out if a test hangs
    initial begin
        #200000;
        $display("FAIL tb_tmdsap_tx_framer: watchdog timeout");
        $fatal;
    end

endmodule

`default_nettype wire
