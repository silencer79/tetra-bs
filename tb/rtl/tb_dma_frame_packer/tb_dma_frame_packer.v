// tb/rtl/tb_dma_frame_packer/tb_dma_frame_packer.v
//
// Owned by Agent A1 (A1-fpga-axi-dma).
//
// Bit-level pack round-trip TB for tetra_dma_frame_packer.
//
// Test coverage:
//   1. Feed a 32-byte TmaSap (TMAS) payload, assert output AXIS bytes
//      exactly match  [0x544D4153][len=0x00000028][32 payload bytes]
//      → 40 bytes total = 10× 32-bit beats with tlast on the last.
//   2. Feed a 44-byte TmdSap (TMDC) payload, assert output exactly
//      [0x544D4443][len=0x00000034][44 payload bytes] → 13× 32-bit beats.
//   3. Feed a 12-byte TmaSap report (TMAR), assert output exactly
//      [0x544D4152][len=0x00000014][12 payload bytes] → 5× beats.
//   4. Feed a 33-byte payload (= 41 bytes total, NOT a multiple of 4):
//      verify final beat has tkeep=4'b1000 and only the byte 0 lane valid.
//
// Pass/fail contract: print exactly one line containing "PASS" on success;
// any FAIL → $fatal.
//
// Verilog-2001 only.

`timescale 1ns / 1ps
`default_nettype none

module tb_dma_frame_packer;

    // -----------------------------------------------------------------
    // Clock / reset
    // -----------------------------------------------------------------
    reg clk = 1'b0;
    reg rst_n = 1'b0;
    always #5 clk = ~clk; // 100 MHz nominal

    // -----------------------------------------------------------------
    // DUT signals
    // -----------------------------------------------------------------
    reg          s_start;
    reg [1:0]    s_magic_sel;
    reg [15:0]   s_payload_len_bytes;
    wire         s_busy;
    wire         frame_done_pulse;
    reg [7:0]    s_byte_data;
    reg          s_byte_valid;
    wire         s_byte_ready;

    wire [31:0]  m_axis_tdata;
    wire         m_axis_tvalid;
    reg          m_axis_tready;
    wire         m_axis_tlast;
    wire [3:0]   m_axis_tkeep;

    tetra_dma_frame_packer #(.LEN_WIDTH(16)) dut (
        .clk                  (clk),
        .rst_n                (rst_n),
        .s_start              (s_start),
        .s_magic_sel          (s_magic_sel),
        .s_payload_len_bytes  (s_payload_len_bytes),
        .s_busy               (s_busy),
        .frame_done_pulse     (frame_done_pulse),
        .s_byte_data          (s_byte_data),
        .s_byte_valid         (s_byte_valid),
        .s_byte_ready         (s_byte_ready),
        .m_axis_tdata         (m_axis_tdata),
        .m_axis_tvalid        (m_axis_tvalid),
        .m_axis_tready        (m_axis_tready),
        .m_axis_tlast         (m_axis_tlast),
        .m_axis_tkeep         (m_axis_tkeep)
    );

    // -----------------------------------------------------------------
    // Capture buffer for AXIS output bytes (per scenario).
    // -----------------------------------------------------------------
    reg [7:0]  cap_buf [0:255];
    integer    cap_len;
    integer    cap_beats;
    reg        last_beat_seen_tlast;
    reg [3:0]  last_beat_tkeep;

    task capture_reset;
        begin
            cap_len   = 0;
            cap_beats = 0;
            last_beat_seen_tlast = 1'b0;
            last_beat_tkeep      = 4'b0000;
        end
    endtask

    // Sample AXIS handshake and store bytes (MSB lane first per wire-byte map).
    always @(posedge clk) begin
        if (rst_n && m_axis_tvalid && m_axis_tready) begin
            cap_beats <= cap_beats + 1;
            last_beat_seen_tlast <= m_axis_tlast;
            last_beat_tkeep      <= m_axis_tkeep;
            if (m_axis_tkeep[3]) begin cap_buf[cap_len    ] <= m_axis_tdata[31:24]; end
            if (m_axis_tkeep[2]) begin cap_buf[cap_len + (m_axis_tkeep[3]?1:0)                                            ] <= m_axis_tdata[23:16]; end
            if (m_axis_tkeep[1]) begin cap_buf[cap_len + (m_axis_tkeep[3]?1:0) + (m_axis_tkeep[2]?1:0)                     ] <= m_axis_tdata[15: 8]; end
            if (m_axis_tkeep[0]) begin cap_buf[cap_len + (m_axis_tkeep[3]?1:0) + (m_axis_tkeep[2]?1:0) + (m_axis_tkeep[1]?1:0)] <= m_axis_tdata[ 7: 0]; end
            cap_len <= cap_len +
                       (m_axis_tkeep[3]?1:0) +
                       (m_axis_tkeep[2]?1:0) +
                       (m_axis_tkeep[1]?1:0) +
                       (m_axis_tkeep[0]?1:0);
        end
    end

    // -----------------------------------------------------------------
    // Driver tasks
    // -----------------------------------------------------------------
    integer i;
    reg [7:0] payload_buf [0:255];

    task drive_frame;
        input [1:0]  magic_sel;
        input integer payload_len;
        begin
            // 1) Pulse start
            @(negedge clk);
            s_magic_sel         = magic_sel;
            s_payload_len_bytes = payload_len[15:0];
            s_start             = 1'b1;
            @(negedge clk);
            s_start             = 1'b0;

            // 2) Stream bytes — canonical AXI-Stream master pattern.
            // Drive data + valid at negedge, wait for ready level-sensitive
            // (which indicates the FSM will accept on the upcoming posedge),
            // then advance after the posedge.
            for (i = 0; i < payload_len; i = i + 1) begin
                @(negedge clk);
                s_byte_data  = payload_buf[i];
                s_byte_valid = 1'b1;
                // wait until slave is ready (level-sensitive); this means
                // at the upcoming posedge the byte will be accepted.
                wait (s_byte_ready);
                @(posedge clk);
                // byte is accepted at this posedge.
            end
            @(negedge clk);
            s_byte_valid = 1'b0;
            s_byte_data  = 8'h00;

            // 3) Wait for frame_done_pulse
            while (!frame_done_pulse) @(posedge clk);
        end
    endtask

    // -----------------------------------------------------------------
    // Verification helpers
    // -----------------------------------------------------------------
    task fail;
        input integer code;
        begin
            $display("FAIL tb_dma_frame_packer code=%0d cap_len=%0d cap_beats=%0d", code, cap_len, cap_beats);
            $fatal;
        end
    endtask

    task check_byte;
        input integer idx;
        input [7:0]   expected;
        begin
            if (cap_buf[idx] !== expected) begin
                $display("FAIL byte[%0d] = 0x%02h, expected 0x%02h", idx, cap_buf[idx], expected);
                $fatal;
            end
        end
    endtask

    // -----------------------------------------------------------------
    // Test sequence
    // -----------------------------------------------------------------
    initial begin
        // Init
        s_start             = 1'b0;
        s_magic_sel         = 2'd0;
        s_payload_len_bytes = 16'd0;
        s_byte_data         = 8'h00;
        s_byte_valid        = 1'b0;
        m_axis_tready       = 1'b1; // always-ready slave for now
        capture_reset();

        // Reset
        rst_n = 1'b0;
        repeat (4) @(posedge clk);
        rst_n = 1'b1;
        @(posedge clk);

        // ---------- Test 1: 32-byte TMAS payload ---------------------
        capture_reset();
        for (i = 0; i < 32; i = i + 1) payload_buf[i] = i[7:0] ^ 8'hA5;
        drive_frame(2'd0, 32);
        @(posedge clk);
        @(posedge clk);

        // Expect 40 bytes total: 4 (magic) + 4 (len) + 32 (payload).
        if (cap_len !== 40) begin
            $display("FAIL T1: cap_len=%0d expected 40", cap_len);
            $fatal;
        end
        if (cap_beats !== 10) begin
            $display("FAIL T1: cap_beats=%0d expected 10", cap_beats);
            $fatal;
        end
        // magic = 0x544D4153 (TMAS)
        check_byte(0, 8'h54);
        check_byte(1, 8'h4D);
        check_byte(2, 8'h41);
        check_byte(3, 8'h53);
        // total len = 40 = 0x00000028
        check_byte(4, 8'h00);
        check_byte(5, 8'h00);
        check_byte(6, 8'h00);
        check_byte(7, 8'h28);
        // payload
        for (i = 0; i < 32; i = i + 1) begin
            if (cap_buf[8 + i] !== (i[7:0] ^ 8'hA5)) begin
                $display("FAIL T1 payload[%0d] = 0x%02h, expected 0x%02h",
                         i, cap_buf[8+i], i[7:0] ^ 8'hA5);
                $fatal;
            end
        end
        if (!last_beat_seen_tlast) begin
            $display("FAIL T1: tlast not asserted on last beat");
            $fatal;
        end
        if (last_beat_tkeep !== 4'b1111) begin
            $display("FAIL T1: last beat tkeep=0x%h expected 0xF", last_beat_tkeep);
            $fatal;
        end
        $display("[T1] 32-byte TMAS frame: PASS");

        // ---------- Test 2: 44-byte TMDC payload ---------------------
        capture_reset();
        for (i = 0; i < 44; i = i + 1) payload_buf[i] = i[7:0] + 8'h10;
        drive_frame(2'd2, 44);
        @(posedge clk);
        @(posedge clk);

        if (cap_len !== 52) begin
            $display("FAIL T2: cap_len=%0d expected 52", cap_len);
            $fatal;
        end
        if (cap_beats !== 13) begin
            $display("FAIL T2: cap_beats=%0d expected 13", cap_beats);
            $fatal;
        end
        // magic = 0x544D4443 (TMDC)
        check_byte(0, 8'h54);
        check_byte(1, 8'h4D);
        check_byte(2, 8'h44);
        check_byte(3, 8'h43);
        // total len = 52 = 0x00000034
        check_byte(4, 8'h00);
        check_byte(5, 8'h00);
        check_byte(6, 8'h00);
        check_byte(7, 8'h34);
        for (i = 0; i < 44; i = i + 1) begin
            if (cap_buf[8 + i] !== (i[7:0] + 8'h10)) begin
                $display("FAIL T2 payload[%0d] = 0x%02h, expected 0x%02h",
                         i, cap_buf[8+i], i[7:0] + 8'h10);
                $fatal;
            end
        end
        $display("[T2] 44-byte TMDC frame: PASS");

        // ---------- Test 3: 12-byte TMAR report -----------------------
        capture_reset();
        for (i = 0; i < 12; i = i + 1) payload_buf[i] = 8'hC0 | i[7:0];
        drive_frame(2'd1, 12);
        @(posedge clk);
        @(posedge clk);

        if (cap_len !== 20) begin
            $display("FAIL T3: cap_len=%0d expected 20", cap_len);
            $fatal;
        end
        if (cap_beats !== 5) begin
            $display("FAIL T3: cap_beats=%0d expected 5", cap_beats);
            $fatal;
        end
        // magic = 0x544D4152 (TMAR)
        check_byte(0, 8'h54);
        check_byte(1, 8'h4D);
        check_byte(2, 8'h41);
        check_byte(3, 8'h52);
        // total len = 20 = 0x00000014
        check_byte(4, 8'h00);
        check_byte(5, 8'h00);
        check_byte(6, 8'h00);
        check_byte(7, 8'h14);
        $display("[T3] 12-byte TMAR frame: PASS");

        // ---------- Test 4: 33-byte payload (partial last beat) -------
        // Total = 41 bytes. Beats = ceil(41/4) = 11 (10 full + 1 partial).
        // Last beat tkeep = 4'b1000 (only byte0 lane), since 41 mod 4 = 1.
        capture_reset();
        for (i = 0; i < 33; i = i + 1) payload_buf[i] = i[7:0];
        drive_frame(2'd0, 33);
        @(posedge clk);
        @(posedge clk);

        if (cap_len !== 41) begin
            $display("FAIL T4: cap_len=%0d expected 41", cap_len);
            $fatal;
        end
        if (cap_beats !== 11) begin
            $display("FAIL T4: cap_beats=%0d expected 11", cap_beats);
            $fatal;
        end
        if (!last_beat_seen_tlast) begin
            $display("FAIL T4: tlast not asserted on last beat");
            $fatal;
        end
        if (last_beat_tkeep !== 4'b1000) begin
            $display("FAIL T4: last beat tkeep=0x%h expected 0x8 (partial)", last_beat_tkeep);
            $fatal;
        end
        // Spot check byte 40 (last byte of payload, == payload[32]).
        check_byte(40, 8'h20); // payload[32] = i[7:0]=32
        $display("[T4] 33-byte (partial last beat) frame: PASS");

        // ---------- Test 5: small frame round-trip ----------------------
        // Verifies the AXIS handshake at minimum size (1-byte payload).
        // Total = 9 bytes = 3 beats, last beat tkeep = 4'b1000.
        capture_reset();
        payload_buf[0] = 8'hDE;
        drive_frame(2'd0, 1);
        @(posedge clk);
        @(posedge clk);

        if (cap_len !== 9) begin
            $display("FAIL T5: cap_len=%0d expected 9", cap_len);
            $fatal;
        end
        if (cap_beats !== 3) begin
            $display("FAIL T5: cap_beats=%0d expected 3", cap_beats);
            $fatal;
        end
        check_byte(0, 8'h54);
        check_byte(7, 8'h09); // total = 9
        check_byte(8, 8'hDE);
        if (last_beat_tkeep !== 4'b1000) begin
            $display("FAIL T5: last beat tkeep=0x%h expected 0x8", last_beat_tkeep);
            $fatal;
        end
        $display("[T5] 1-byte payload (minimum): PASS");

        $display("PASS tb_dma_frame_packer (5/5 sub-tests)");
        $finish;
    end

    // Watchdog — bail out if a test hangs
    initial begin
        #100000;
        $display("FAIL tb_dma_frame_packer: watchdog timeout");
        $fatal;
    end

endmodule

`default_nettype wire
