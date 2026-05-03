// tb/rtl/tb_dma_frame_unpacker/tb_dma_frame_unpacker.v
//
// Owned by Agent A1 (A1-fpga-axi-dma).
//
// Bit-level unpack round-trip TB for tetra_dma_frame_unpacker.
//
// Test coverage:
//   1. Feed [0x544D4153 magic][0x00000028 len][32 payload bytes] AXIS,
//      assert byte stream out == 32 bytes byte-identical, m_magic_sel == 0.
//   2. Feed TMDC frame (magic 0x544D4443) → m_magic_sel == 2.
//   3. Feed bad-magic frame → drop_count increments, no payload bytes emitted,
//      m_frame_error_pulse asserts.
//   4. Feed length-mismatch frame (declared 0x40 but actual 0x28) → drop_count
//      increments, m_frame_error_pulse asserts.
//
// Pass/fail contract: print exactly one line containing "PASS" on success.
//
// Verilog-2001 only.

`timescale 1ns / 1ps
`default_nettype none

module tb_dma_frame_unpacker;

    // -----------------------------------------------------------------
    // Clock / reset
    // -----------------------------------------------------------------
    reg clk = 1'b0;
    reg rst_n = 1'b0;
    always #5 clk = ~clk;

    // -----------------------------------------------------------------
    // DUT signals
    // -----------------------------------------------------------------
    reg  [31:0] s_axis_tdata;
    reg         s_axis_tvalid;
    wire        s_axis_tready;
    reg         s_axis_tlast;
    reg  [3:0]  s_axis_tkeep;

    wire [7:0]  m_byte_data;
    wire        m_byte_valid;
    reg         m_byte_ready;
    wire        m_frame_start_pulse;
    wire [1:0]  m_magic_sel;
    wire        m_frame_end_pulse;
    wire        m_frame_error_pulse;
    wire [15:0] drop_count;

    tetra_dma_frame_unpacker #(.LEN_WIDTH(16)) dut (
        .clk                  (clk),
        .rst_n                (rst_n),
        .s_axis_tdata         (s_axis_tdata),
        .s_axis_tvalid        (s_axis_tvalid),
        .s_axis_tready        (s_axis_tready),
        .s_axis_tlast         (s_axis_tlast),
        .s_axis_tkeep         (s_axis_tkeep),
        .m_byte_data          (m_byte_data),
        .m_byte_valid         (m_byte_valid),
        .m_byte_ready         (m_byte_ready),
        .m_frame_start_pulse  (m_frame_start_pulse),
        .m_magic_sel          (m_magic_sel),
        .m_frame_end_pulse    (m_frame_end_pulse),
        .m_frame_error_pulse  (m_frame_error_pulse),
        .drop_count           (drop_count)
    );

    // -----------------------------------------------------------------
    // Capture buffer for byte stream output.
    // -----------------------------------------------------------------
    reg [7:0]  cap_buf [0:255];
    integer    cap_len;
    integer    n_frame_start;
    integer    n_frame_end;
    integer    n_frame_error;
    reg [1:0]  last_magic_sel;

    task capture_reset;
        begin
            cap_len        = 0;
            n_frame_start  = 0;
            n_frame_end    = 0;
            n_frame_error  = 0;
            last_magic_sel = 2'd0;
        end
    endtask

    always @(posedge clk) begin
        if (rst_n) begin
            if (m_frame_start_pulse) begin
                n_frame_start  <= n_frame_start + 1;
                last_magic_sel <= m_magic_sel;
            end
            if (m_frame_end_pulse)  n_frame_end   <= n_frame_end + 1;
            if (m_frame_error_pulse) n_frame_error <= n_frame_error + 1;
            if (m_byte_valid && m_byte_ready) begin
                cap_buf[cap_len] <= m_byte_data;
                cap_len <= cap_len + 1;
            end
        end
    end

    // -----------------------------------------------------------------
    // AXIS-driver helpers.
    // -----------------------------------------------------------------
    integer i;
    reg [7:0] frame_buf [0:255];

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
        end
    endtask

    task axis_idle;
        begin
            @(negedge clk);
            s_axis_tdata  = 32'h0;
            s_axis_tkeep  = 4'b0000;
            s_axis_tlast  = 1'b0;
            s_axis_tvalid = 1'b0;
        end
    endtask

    // Drive a frame of frame_len bytes (including 8-byte header) by
    // packing them MSB-first into 32-bit beats, asserting tlast on the
    // final beat with appropriate tkeep.
    task drive_axis_frame;
        input integer frame_len;
        integer j, beats, last_keep;
        reg [31:0] beat;
        begin
            beats = (frame_len + 3) / 4;
            for (j = 0; j < beats; j = j + 1) begin
                if (j < beats - 1) begin
                    beat = {frame_buf[j*4+0], frame_buf[j*4+1],
                            frame_buf[j*4+2], frame_buf[j*4+3]};
                    axis_drive_beat(beat, 4'b1111, 1'b0);
                end else begin
                    // last beat
                    if (frame_len % 4 == 0) begin
                        beat = {frame_buf[j*4+0], frame_buf[j*4+1],
                                frame_buf[j*4+2], frame_buf[j*4+3]};
                        axis_drive_beat(beat, 4'b1111, 1'b1);
                    end else if (frame_len % 4 == 1) begin
                        beat = {frame_buf[j*4+0], 24'h0};
                        axis_drive_beat(beat, 4'b1000, 1'b1);
                    end else if (frame_len % 4 == 2) begin
                        beat = {frame_buf[j*4+0], frame_buf[j*4+1], 16'h0};
                        axis_drive_beat(beat, 4'b1100, 1'b1);
                    end else begin // == 3
                        beat = {frame_buf[j*4+0], frame_buf[j*4+1],
                                frame_buf[j*4+2], 8'h0};
                        axis_drive_beat(beat, 4'b1110, 1'b1);
                    end
                end
            end
            axis_idle();
        end
    endtask

    // Build the frame_buf[] array with magic + len + payload.
    task build_frame;
        input [31:0] magic;
        input integer payload_len;
        integer j;
        begin
            // header: 4-byte magic + 4-byte length (BE)
            frame_buf[0] = magic[31:24];
            frame_buf[1] = magic[23:16];
            frame_buf[2] = magic[15:8];
            frame_buf[3] = magic[ 7:0];
            // total length = payload + 8
            frame_buf[4] = 8'h00;
            frame_buf[5] = 8'h00;
            frame_buf[6] = (payload_len + 8) >> 8;
            frame_buf[7] = (payload_len + 8) & 8'hFF;
            // payload bytes — caller pre-fills frame_buf[8..]
            for (j = 0; j < 0; j = j + 1) ; // no-op
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
        s_axis_tdata  = 32'h0;
        s_axis_tvalid = 1'b0;
        s_axis_tlast  = 1'b0;
        s_axis_tkeep  = 4'b0000;
        m_byte_ready  = 1'b1; // always-ready consumer
        capture_reset();

        rst_n = 1'b0;
        repeat (4) @(posedge clk);
        rst_n = 1'b1;
        @(posedge clk);

        // ---------- Test 1: 32-byte TMAS payload ---------------------
        capture_reset();
        build_frame(32'h544D_4153, 32);
        for (i = 0; i < 32; i = i + 1) frame_buf[8 + i] = i[7:0] ^ 8'hA5;
        drive_axis_frame(40); // 8 hdr + 32 payload

        // Wait a few cycles for the unpacker to flush the bytes.
        repeat (40) @(posedge clk);

        if (cap_len !== 32) begin
            $display("FAIL T1: cap_len=%0d expected 32", cap_len);
            $fatal;
        end
        if (n_frame_start !== 1) begin
            $display("FAIL T1: n_frame_start=%0d expected 1", n_frame_start);
            $fatal;
        end
        if (n_frame_end !== 1) begin
            $display("FAIL T1: n_frame_end=%0d expected 1", n_frame_end);
            $fatal;
        end
        if (n_frame_error !== 0) begin
            $display("FAIL T1: n_frame_error=%0d expected 0", n_frame_error);
            $fatal;
        end
        if (last_magic_sel !== 2'd0) begin
            $display("FAIL T1: magic_sel=%0d expected 0 (TMAS)", last_magic_sel);
            $fatal;
        end
        for (i = 0; i < 32; i = i + 1) check_byte(i, i[7:0] ^ 8'hA5);
        $display("[T1] TMAS 32-byte unpack: PASS");

        // ---------- Test 2: 44-byte TMDC payload ---------------------
        capture_reset();
        build_frame(32'h544D_4443, 44);
        for (i = 0; i < 44; i = i + 1) frame_buf[8 + i] = i[7:0] + 8'h10;
        drive_axis_frame(52);
        repeat (60) @(posedge clk);

        if (cap_len !== 44) begin
            $display("FAIL T2: cap_len=%0d expected 44", cap_len);
            $fatal;
        end
        if (last_magic_sel !== 2'd2) begin
            $display("FAIL T2: magic_sel=%0d expected 2 (TMDC)", last_magic_sel);
            $fatal;
        end
        if (n_frame_error !== 0) begin
            $display("FAIL T2: n_frame_error=%0d expected 0", n_frame_error);
            $fatal;
        end
        for (i = 0; i < 44; i = i + 1) check_byte(i, i[7:0] + 8'h10);
        $display("[T2] TMDC 44-byte unpack: PASS");

        // ---------- Test 3: bad magic → drop counter ----------------
        capture_reset();
        build_frame(32'hDEAD_BEEF, 8);  // bad magic
        for (i = 0; i < 8; i = i + 1) frame_buf[8 + i] = 8'hFF;
        drive_axis_frame(16);
        repeat (20) @(posedge clk);

        if (cap_len !== 0) begin
            $display("FAIL T3: cap_len=%0d expected 0 (bad magic dropped)", cap_len);
            $fatal;
        end
        if (drop_count !== 16'd1) begin
            $display("FAIL T3: drop_count=%0d expected 1", drop_count);
            $fatal;
        end
        if (n_frame_error !== 1) begin
            $display("FAIL T3: n_frame_error=%0d expected 1", n_frame_error);
            $fatal;
        end
        $display("[T3] bad-magic dropped: PASS");

        // ---------- Test 4: length mismatch → drop counter ----------
        // Declared total_len = 64 (0x40) but we only feed 16 bytes total
        // (8 hdr + 8 payload) and assert tlast → length mismatch on
        // final beat, increments drop_count.
        capture_reset();
        frame_buf[0] = 8'h54;  // magic = TMAS
        frame_buf[1] = 8'h4D;
        frame_buf[2] = 8'h41;
        frame_buf[3] = 8'h53;
        frame_buf[4] = 8'h00;  // declared total len = 0x40
        frame_buf[5] = 8'h00;
        frame_buf[6] = 8'h00;
        frame_buf[7] = 8'h40;
        for (i = 0; i < 8; i = i + 1) frame_buf[8 + i] = 8'hAA;
        drive_axis_frame(16);   // physical only sends 16 bytes; declared was 64
        repeat (20) @(posedge clk);

        // drop_count is sticky across the run (T3 left it at 1; T4 adds 1).
        if (drop_count !== 16'd2) begin
            $display("FAIL T4: drop_count=%0d expected 2", drop_count);
            $fatal;
        end
        // n_frame_error was reset by capture_reset(); only THIS scenario's
        // error pulse should be visible.
        if (n_frame_error !== 1) begin
            $display("FAIL T4: n_frame_error=%0d expected 1", n_frame_error);
            $fatal;
        end
        // n_frame_end should NOT have incremented for this scenario.
        if (n_frame_end !== 0) begin
            $display("FAIL T4: n_frame_end=%0d expected 0 (this scenario only)", n_frame_end);
            $fatal;
        end
        $display("[T4] length-mismatch dropped: PASS");

        // ---------- Test 5: TMAR (12-byte report) -------------------
        capture_reset();
        build_frame(32'h544D_4152, 12);
        for (i = 0; i < 12; i = i + 1) frame_buf[8 + i] = 8'hC0 | i[7:0];
        drive_axis_frame(20);
        repeat (30) @(posedge clk);

        if (cap_len !== 12) begin
            $display("FAIL T5: cap_len=%0d expected 12", cap_len);
            $fatal;
        end
        if (last_magic_sel !== 2'd1) begin
            $display("FAIL T5: magic_sel=%0d expected 1 (TMAR)", last_magic_sel);
            $fatal;
        end
        for (i = 0; i < 12; i = i + 1) check_byte(i, 8'hC0 | i[7:0]);
        $display("[T5] TMAR 12-byte unpack: PASS");

        $display("PASS tb_dma_frame_unpacker (5/5 sub-tests)");
        $finish;
    end

    // Watchdog
    initial begin
        #200000;
        $display("FAIL tb_dma_frame_unpacker: watchdog timeout");
        $fatal;
    end

    // ---- T1: optional VCD dump (compile with -DVCDDUMP to enable) ---------
`ifdef VCDDUMP
    initial begin
`ifdef VCD_FILE
        $dumpfile(`VCD_FILE);
`else
        $dumpfile("dump.vcd");
`endif
        $dumpvars(0, tb_dma_frame_unpacker);
    end
`endif

endmodule

`default_nettype wire
