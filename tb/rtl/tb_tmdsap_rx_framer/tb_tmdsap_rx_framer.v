// tb/rtl/tb_tmdsap_rx_framer/tb_tmdsap_rx_framer.v
//
// Owned by Agent A3 (A3-fpga-tmdsap-framer).
//
// Bit-level pack TB for `tetra_tmdsap_rx_framer`.  Drives a 432-bit
// pseudo-NUB (alternating-byte 0xAA/0x55 pattern, repeating) into the
// LMAC-side input port and asserts that the AXIS output is exactly:
//
//     [0x544D_4443][0x0000_003E][54-byte 0xAA55... payload]
//
// totalling 62 bytes across 16 beats (15 full + 1 partial tkeep=4'b1100,
// tlast on the final beat).
//
// Pass/fail contract: print one line containing "PASS" on success;
// any FAIL → $fatal.
//
// Verilog-2001 only.

`timescale 1ns / 1ps
`default_nettype none

module tb_tmdsap_rx_framer;

    // -----------------------------------------------------------------
    // Clock / reset
    // -----------------------------------------------------------------
    reg clk = 1'b0;
    reg rst_n = 1'b0;
    always #5 clk = ~clk;        // 100 MHz nominal

    // -----------------------------------------------------------------
    // DUT signals
    // -----------------------------------------------------------------
    reg  [431:0]  in_nub_bits;
    reg           in_valid;
    wire          in_ready;

    wire [31:0]   m_axis_tdata;
    wire          m_axis_tvalid;
    reg           m_axis_tready;
    wire          m_axis_tlast;
    wire [3:0]    m_axis_tkeep;

    wire [31:0]   tlm_rx_frames;

    tetra_tmdsap_rx_framer dut (
        .clk            (clk),
        .rst_n          (rst_n),
        .in_nub_bits    (in_nub_bits),
        .in_valid       (in_valid),
        .in_ready       (in_ready),
        .m_axis_tdata   (m_axis_tdata),
        .m_axis_tvalid  (m_axis_tvalid),
        .m_axis_tready  (m_axis_tready),
        .m_axis_tlast   (m_axis_tlast),
        .m_axis_tkeep   (m_axis_tkeep),
        .tlm_rx_frames  (tlm_rx_frames)
    );

    // -----------------------------------------------------------------
    // Capture buffer for AXIS output bytes (per scenario).
    // -----------------------------------------------------------------
    reg  [7:0]   cap_buf [0:127];
    integer      cap_len;
    integer      cap_beats;
    reg          last_beat_seen_tlast;
    reg  [3:0]   last_beat_tkeep;
    integer      i;

    task capture_reset;
        integer k;
        begin
            cap_len   = 0;
            cap_beats = 0;
            last_beat_seen_tlast = 1'b0;
            last_beat_tkeep      = 4'b0000;
            for (k = 0; k < 128; k = k + 1) cap_buf[k] = 8'h00;
        end
    endtask

    // Sample AXIS handshake and store bytes (MSB lane first per wire-byte map).
    always @(posedge clk) begin
        if (rst_n && m_axis_tvalid && m_axis_tready) begin
            cap_beats <= cap_beats + 1;
            last_beat_seen_tlast <= m_axis_tlast;
            last_beat_tkeep      <= m_axis_tkeep;
            if (m_axis_tkeep[3]) cap_buf[cap_len    ] <= m_axis_tdata[31:24];
            if (m_axis_tkeep[2]) cap_buf[cap_len + (m_axis_tkeep[3]?1:0)
                                        ] <= m_axis_tdata[23:16];
            if (m_axis_tkeep[1]) cap_buf[cap_len + (m_axis_tkeep[3]?1:0)
                                                + (m_axis_tkeep[2]?1:0)
                                        ] <= m_axis_tdata[15: 8];
            if (m_axis_tkeep[0]) cap_buf[cap_len + (m_axis_tkeep[3]?1:0)
                                                + (m_axis_tkeep[2]?1:0)
                                                + (m_axis_tkeep[1]?1:0)
                                        ] <= m_axis_tdata[ 7: 0];
            cap_len <= cap_len +
                       (m_axis_tkeep[3]?1:0) +
                       (m_axis_tkeep[2]?1:0) +
                       (m_axis_tkeep[1]?1:0) +
                       (m_axis_tkeep[0]?1:0);
        end
    end

    // -----------------------------------------------------------------
    // Reference NUB: alternating-byte 0xAA/0x55 pattern, repeating.
    // 54 bytes: byte i = (i even) ? 0xAA : 0x55.
    // Mapped MSB-first into 432 bits: bit [431:424]=0xAA, [423:416]=0x55, ...
    // -----------------------------------------------------------------
    reg [7:0]  ref_payload [0:53];

    task build_ref_payload;
        integer k;
        begin
            for (k = 0; k < 54; k = k + 1) begin
                ref_payload[k] = (k[0] == 1'b0) ? 8'hAA : 8'h55;
            end
        end
    endtask

    task pack_ref_into_nub;
        // Builds in_nub_bits MSB-first from ref_payload.
        integer k;
        begin
            for (k = 0; k < 54; k = k + 1) begin
                in_nub_bits[431 - 8*k -: 8] = ref_payload[k];
            end
        end
    endtask

    // -----------------------------------------------------------------
    // Driver
    // -----------------------------------------------------------------
    task drive_one_frame;
        begin
            // Wait for in_ready high, then pulse in_valid for 1 cycle.
            @(negedge clk);
            wait (in_ready);
            @(negedge clk);
            in_valid = 1'b1;
            @(negedge clk);
            in_valid = 1'b0;
        end
    endtask

    // -----------------------------------------------------------------
    // Verification helpers
    // -----------------------------------------------------------------
    task check_byte;
        input integer idx;
        input [7:0]   expected;
        begin
            if (cap_buf[idx] !== expected) begin
                $display("FAIL byte[%0d] = 0x%02h, expected 0x%02h",
                         idx, cap_buf[idx], expected);
                $fatal;
            end
        end
    endtask

    // -----------------------------------------------------------------
    // Test sequence
    // -----------------------------------------------------------------
    initial begin
        // Init
        in_nub_bits   = 432'h0;
        in_valid      = 1'b0;
        m_axis_tready = 1'b1;     // always-ready slave for primary scenarios
        capture_reset();

        // Reset
        rst_n = 1'b0;
        repeat (4) @(posedge clk);
        rst_n = 1'b1;
        @(posedge clk);

        // ---------- Test 1: 0xAA/0x55 NUB → byte-identical TMDC frame ----
        build_ref_payload();
        pack_ref_into_nub();
        capture_reset();

        drive_one_frame();

        // Wait for tlast (frame complete).
        wait (last_beat_seen_tlast);
        @(posedge clk);
        @(posedge clk);

        // Expect 62 bytes total = 4 (magic) + 4 (len) + 54 (payload).
        if (cap_len !== 62) begin
            $display("FAIL T1: cap_len=%0d expected 62", cap_len);
            $fatal;
        end
        // Expect 16 beats (15 full + 1 partial).
        if (cap_beats !== 16) begin
            $display("FAIL T1: cap_beats=%0d expected 16", cap_beats);
            $fatal;
        end

        // magic = 0x544D4443 (TMDC)
        check_byte(0, 8'h54);
        check_byte(1, 8'h4D);
        check_byte(2, 8'h44);
        check_byte(3, 8'h43);
        // total len = 62 = 0x0000_003E
        check_byte(4, 8'h00);
        check_byte(5, 8'h00);
        check_byte(6, 8'h00);
        check_byte(7, 8'h3E);
        // payload bytes 0..53 = 0xAA/0x55 alternating
        for (i = 0; i < 54; i = i + 1) begin
            if (cap_buf[8 + i] !== ref_payload[i]) begin
                $display("FAIL T1 payload[%0d] = 0x%02h, expected 0x%02h",
                         i, cap_buf[8 + i], ref_payload[i]);
                $fatal;
            end
        end
        if (!last_beat_seen_tlast) begin
            $display("FAIL T1: tlast not asserted on last beat");
            $fatal;
        end
        if (last_beat_tkeep !== 4'b1100) begin
            $display("FAIL T1: last beat tkeep=0x%h expected 0xC (62 mod 4 = 2)",
                     last_beat_tkeep);
            $fatal;
        end
        if (tlm_rx_frames !== 32'd1) begin
            $display("FAIL T1: tlm_rx_frames=%0d expected 1", tlm_rx_frames);
            $fatal;
        end
        $display("[T1] 0xAA/0x55 NUB → byte-identical TMDC frame: PASS");

        // ---------- Test 2: second frame increments counter --------------
        // Use a different MSB-first pattern: 0xC3 / 0x3C alternating.
        for (i = 0; i < 54; i = i + 1) begin
            ref_payload[i] = (i[0] == 1'b0) ? 8'hC3 : 8'h3C;
        end
        pack_ref_into_nub();
        capture_reset();

        drive_one_frame();
        wait (last_beat_seen_tlast);
        @(posedge clk);
        @(posedge clk);

        if (cap_len !== 62) begin
            $display("FAIL T2: cap_len=%0d expected 62", cap_len);
            $fatal;
        end
        for (i = 0; i < 54; i = i + 1) begin
            if (cap_buf[8 + i] !== ref_payload[i]) begin
                $display("FAIL T2 payload[%0d] = 0x%02h, expected 0x%02h",
                         i, cap_buf[8 + i], ref_payload[i]);
                $fatal;
            end
        end
        if (tlm_rx_frames !== 32'd2) begin
            $display("FAIL T2: tlm_rx_frames=%0d expected 2", tlm_rx_frames);
            $fatal;
        end
        $display("[T2] second frame, counter==2: PASS");

        // ---------- Test 3: AXIS back-pressure (stalling slave) ----------
        // Drop tready mid-frame for a few cycles, verify byte-identity.
        for (i = 0; i < 54; i = i + 1) begin
            ref_payload[i] = i[7:0];     // arbitrary distinct values
        end
        pack_ref_into_nub();
        capture_reset();

        m_axis_tready = 1'b0;       // start with slave NOT ready
        drive_one_frame();
        repeat (8) @(posedge clk);  // hold tready low for 8 cycles
        m_axis_tready = 1'b1;
        wait (last_beat_seen_tlast);
        @(posedge clk);
        @(posedge clk);

        if (cap_len !== 62) begin
            $display("FAIL T3: cap_len=%0d expected 62 (back-pressure case)",
                     cap_len);
            $fatal;
        end
        for (i = 0; i < 54; i = i + 1) begin
            if (cap_buf[8 + i] !== ref_payload[i]) begin
                $display("FAIL T3 payload[%0d] = 0x%02h, expected 0x%02h",
                         i, cap_buf[8 + i], ref_payload[i]);
                $fatal;
            end
        end
        if (tlm_rx_frames !== 32'd3) begin
            $display("FAIL T3: tlm_rx_frames=%0d expected 3", tlm_rx_frames);
            $fatal;
        end
        $display("[T3] stalling slave (back-pressure) byte-identical: PASS");

        $display("PASS tb_tmdsap_rx_framer (3/3 sub-tests)");
        $finish;
    end

    // Watchdog — bail out if a test hangs
    initial begin
        #200000;
        $display("FAIL tb_tmdsap_rx_framer: watchdog timeout");
        $fatal;
    end

endmodule

`default_nettype wire
