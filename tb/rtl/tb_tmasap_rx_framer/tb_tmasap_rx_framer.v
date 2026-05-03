// tb/rtl/tb_tmasap_rx_framer/tb_tmasap_rx_framer.v
//
// Owned by Agent A2 (A2-fpga-tmasap-framer).
//
// Bit-exact RX-framer TB.
//
// Test 1 (TMAS): drive a Gold-Reference reassembled MM body of 129 bits
// derived from `docs/references/reference_demand_reassembly_bitexact.md`
// (UL#0 hex `01 41 7F A7 01 12 66 34 20 C1 22 60` ++ UL#1 hex
// `D4 1C 3C 02 40 50 2F 4D 61 20 00 00`, taking UL#0[48..91] ++ UL#1[7..91]).
// Resulting 129-bit MM body MSB-aligned to 17 payload bytes:
//   0x66 0x34 0x20 0xC1 0x22 0x60 0xE1 0xE0 0x12 0x02 0x81 0x7A 0x6B 0x09 0x00 0x00 0x00
// (last byte's low 7 bits are zero-pad - body is 129 bits, MSB-aligned).
//
// Expected TMAS frame on AXIS, byte-by-byte (big-endian on the wire):
//   bytes 0..3   : 54 4D 41 53                ("TMAS")
//   bytes 4..5   : 00 35                      (frame_len = 53 = 0x35)
//   bytes 6..7   : 00 81                      (pdu_len_bits = 129 = 0x81)
//   bytes 8..11  : 00 28 2F F4                (ssi=0x282FF4 MSB-aligned in 4 B)
//   bytes 12..15 : 00 00 00 00                (ssi_type=0(Unknown), flags=0, reserved=0)
//   bytes 16..19 : DE AD BE EF                (endpoint_id - test-driven)
//   bytes 20..23 : 00 00 00 00                (new_endpoint_id)
//   bytes 24..27 : 00 00 00 00                (css_endpoint_id)
//   bytes 28..31 : 12 34 56 78                (scrambling_code - test-driven)
//   bytes 32..35 : 00 00 00 00                (reserved)
//   bytes 36..52 : 66 34 20 C1 22 60 E1 E0 12 02 81 7A 6B 09 00 00 00
//   total declared frame_len = 53 bytes (bytes 0..52)
//   on the wire, total = ceil(53/4)*4 = 56 bytes (14 beats), tlast on
//   beat 14, tkeep=4'b1111 throughout (trailing zero pad).
//
// Test 2 (TMAR): pulse `tmar_emit_pulse` with `req_handle = 0x12345678`,
// `report_code = 4` (SuccessRandomAccess) and verify:
//   bytes 0..3 : 54 4D 41 52   ("TMAR")
//   bytes 4..5 : 00 0C         (frame_len = 12)
//   bytes 6..7 : 00 00         (reserved)
//   bytes 8..11: 12 34 56 78   (req_handle)
//   bytes 12   : 04            (report_code = SuccessRandomAccess)
//   bytes 13..15: 00 00 00     (pad)
//   total = 16 bytes = 4 beats, tlast on beat 4.
//
// Pass/fail contract: print exactly one line containing "PASS" on success.
//
// Verilog-2001 only.

`timescale 1ns / 1ps
`default_nettype none

module tb_tmasap_rx_framer;

    // -----------------------------------------------------------------
    reg clk = 1'b0;
    reg rst_n = 1'b0;
    always #5 clk = ~clk;

    // -----------------------------------------------------------------
    // DUT signals
    // -----------------------------------------------------------------
    reg          umac_valid;
    wire         umac_ready;
    reg  [128:0] umac_pdu;
    reg  [10:0]  umac_pdu_len;
    reg  [23:0]  umac_ssi;
    reg  [2:0]   umac_ssi_type;
    reg  [31:0]  umac_endpoint_id;
    reg  [31:0]  umac_scrambling_code;

    reg          tmar_emit;
    reg  [31:0]  tmar_handle;
    reg  [7:0]   tmar_code;

    wire [31:0]  m_axis_tdata;
    wire         m_axis_tvalid;
    reg          m_axis_tready;
    wire         m_axis_tlast;
    wire [3:0]   m_axis_tkeep;

    wire [31:0]  tlm_tmas_cnt;
    wire [31:0]  tlm_tmar_cnt;
    wire [15:0]  tlm_drop_cnt;

    tetra_tmasap_rx_framer dut (
        .clk                                 (clk),
        .rst_n                               (rst_n),
        .umac_to_tmasap_rx_valid             (umac_valid),
        .umac_to_tmasap_rx_ready             (umac_ready),
        .umac_to_tmasap_rx_pdu               (umac_pdu),
        .umac_to_tmasap_rx_pdu_len           (umac_pdu_len),
        .umac_to_tmasap_rx_ssi               (umac_ssi),
        .umac_to_tmasap_rx_ssi_type          (umac_ssi_type),
        .umac_to_tmasap_rx_endpoint_id       (umac_endpoint_id),
        .umac_to_tmasap_rx_scrambling_code   (umac_scrambling_code),
        .tmar_emit_pulse                     (tmar_emit),
        .tmar_req_handle                     (tmar_handle),
        .tmar_report_code                    (tmar_code),
        .m_axis_tdata                        (m_axis_tdata),
        .m_axis_tvalid                       (m_axis_tvalid),
        .m_axis_tready                       (m_axis_tready),
        .m_axis_tlast                        (m_axis_tlast),
        .m_axis_tkeep                        (m_axis_tkeep),
        .tlm_tmas_frames_cnt                 (tlm_tmas_cnt),
        .tlm_tmar_frames_cnt                 (tlm_tmar_cnt),
        .tlm_rx_drop_cnt                     (tlm_drop_cnt)
    );

    // -----------------------------------------------------------------
    // AXIS sink: capture every fired beat into a byte buffer.
    // -----------------------------------------------------------------
    reg [7:0] cap_buf [0:255];
    integer   cap_len;
    integer   cap_beats;
    reg       last_beat_seen_tlast;
    reg [3:0] last_beat_tkeep;

    task capture_reset;
        begin
            cap_len   = 0;
            cap_beats = 0;
            last_beat_seen_tlast = 1'b0;
            last_beat_tkeep      = 4'b0000;
        end
    endtask

    always @(posedge clk) begin
        if (rst_n && m_axis_tvalid && m_axis_tready) begin
            cap_beats <= cap_beats + 1;
            last_beat_seen_tlast <= m_axis_tlast;
            last_beat_tkeep      <= m_axis_tkeep;
            // capture all 4 lanes (we always drive tkeep=4'b1111)
            cap_buf[cap_len    ] <= m_axis_tdata[31:24];
            cap_buf[cap_len + 1] <= m_axis_tdata[23:16];
            cap_buf[cap_len + 2] <= m_axis_tdata[15: 8];
            cap_buf[cap_len + 3] <= m_axis_tdata[ 7: 0];
            cap_len <= cap_len + 4;
        end
    end

    // -----------------------------------------------------------------
    // Verification helpers
    // -----------------------------------------------------------------
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

    integer i;

    // -----------------------------------------------------------------
    // Test sequence
    // -----------------------------------------------------------------
    initial begin
        // Init
        umac_valid           = 1'b0;
        umac_pdu             = 129'b0;
        umac_pdu_len         = 11'd0;
        umac_ssi             = 24'h0;
        umac_ssi_type        = 3'b0;
        umac_endpoint_id     = 32'h0;
        umac_scrambling_code = 32'h0;
        tmar_emit            = 1'b0;
        tmar_handle          = 32'h0;
        tmar_code            = 8'h0;
        m_axis_tready        = 1'b1; // always-ready sink for now
        capture_reset();

        rst_n = 1'b0;
        repeat (4) @(posedge clk);
        rst_n = 1'b1;
        @(posedge clk);

        // ============== Test 1: Gold-Ref TMAS frame ==================
        capture_reset();

        // 129-bit Gold-Ref MM body, MSB-first (bit[128] = first on-air).
        // Derived from UL#0[48..91] ++ UL#1[7..91] per
        // reference_demand_reassembly_bitexact.md.
        umac_pdu             = 129'h0CC68418244C1C3C0240502F4D6120000;
        umac_pdu_len         = 11'd129;
        umac_ssi             = 24'h28_2FF4;
        umac_ssi_type        = 3'd1; // Ssi
        umac_endpoint_id     = 32'hDEAD_BEEF;
        umac_scrambling_code = 32'h1234_5678;

        @(negedge clk);
        umac_valid = 1'b1;
        wait (umac_ready);
        @(posedge clk);
        @(negedge clk);
        umac_valid = 1'b0;

        // Wait for tlast.
        wait (m_axis_tlast && m_axis_tvalid && m_axis_tready);
        @(posedge clk);
        @(posedge clk);

        // Verify byte-for-byte.
        if (cap_beats !== 14) begin
            $display("FAIL T1: cap_beats=%0d expected 14", cap_beats);
            $fatal;
        end
        // Magic "TMAS"
        check_byte(0, 8'h54);
        check_byte(1, 8'h4D);
        check_byte(2, 8'h41);
        check_byte(3, 8'h53);
        // frame_len = 53 = 0x0035 (bytes 4..5)
        check_byte(4, 8'h00);
        check_byte(5, 8'h35);
        // pdu_len_bits = 129 = 0x0081 (bytes 6..7)
        check_byte(6, 8'h00);
        check_byte(7, 8'h81);
        // ssi byte 8..11 = 00 28 2F F4
        check_byte(8,  8'h00);
        check_byte(9,  8'h28);
        check_byte(10, 8'h2F);
        check_byte(11, 8'hF4);
        // ssi_type=1 (Ssi) at byte 12 [bits 26:24 of W3]; flags=0; reserved=0
        // The framer encodes W3 = { 5'b0, ssi_type_q, 8'h00, 16'h0000 }, so
        // byte 12 = {5'b0, ssi_type_q[2:0]} = 0x01, byte 13..15 = 0.
        check_byte(12, 8'h01);
        check_byte(13, 8'h00);
        check_byte(14, 8'h00);
        check_byte(15, 8'h00);
        // endpoint_id 0xDEADBEEF in bytes 16..19
        check_byte(16, 8'hDE);
        check_byte(17, 8'hAD);
        check_byte(18, 8'hBE);
        check_byte(19, 8'hEF);
        // new_endpoint_id 0
        check_byte(20, 8'h00);
        check_byte(21, 8'h00);
        check_byte(22, 8'h00);
        check_byte(23, 8'h00);
        // css_endpoint_id 0
        check_byte(24, 8'h00);
        check_byte(25, 8'h00);
        check_byte(26, 8'h00);
        check_byte(27, 8'h00);
        // scrambling_code 0x12345678
        check_byte(28, 8'h12);
        check_byte(29, 8'h34);
        check_byte(30, 8'h56);
        check_byte(31, 8'h78);
        // reserved
        check_byte(32, 8'h00);
        check_byte(33, 8'h00);
        check_byte(34, 8'h00);
        check_byte(35, 8'h00);
        // MM body MSB-aligned bytes 36..52
        check_byte(36, 8'h66);
        check_byte(37, 8'h34);
        check_byte(38, 8'h20);
        check_byte(39, 8'hC1);
        check_byte(40, 8'h22);
        check_byte(41, 8'h60);
        check_byte(42, 8'hE1);
        check_byte(43, 8'hE0);
        check_byte(44, 8'h12);
        check_byte(45, 8'h02);
        check_byte(46, 8'h81);
        check_byte(47, 8'h7A);
        check_byte(48, 8'h6B);
        check_byte(49, 8'h09);
        check_byte(50, 8'h00); // body[120..127] = 8 trailing pad bits
        check_byte(51, 8'h00); // body[128] = 0 + 7 LSB pad bits = 0
        check_byte(52, 8'h00); // (within frame_len) - tail of MSB-aligned 129-bit body
        // bytes 53..55 are wire-pad zeros (frame_len excludes them; declared len=53)
        check_byte(53, 8'h00);
        check_byte(54, 8'h00);
        check_byte(55, 8'h00);
        if (!last_beat_seen_tlast) begin
            $display("FAIL T1: tlast not asserted on last beat");
            $fatal;
        end
        if (last_beat_tkeep !== 4'b1111) begin
            $display("FAIL T1: last beat tkeep=0x%h expected 0xF", last_beat_tkeep);
            $fatal;
        end
        if (tlm_tmas_cnt !== 32'd1) begin
            $display("FAIL T1: tlm_tmas_cnt=%0d expected 1", tlm_tmas_cnt);
            $fatal;
        end
        $display("[T1] Gold-Ref TMAS frame (129-bit MM body): PASS");

        // ============== Test 2: TMAR SuccessRandomAccess =============
        capture_reset();
        @(negedge clk);
        tmar_handle = 32'h12345678;
        tmar_code   = 8'h04;          // SuccessRandomAccess
        tmar_emit   = 1'b1;
        @(posedge clk);
        @(negedge clk);
        tmar_emit   = 1'b0;

        wait (m_axis_tlast && m_axis_tvalid && m_axis_tready);
        @(posedge clk);
        @(posedge clk);

        if (cap_beats !== 4) begin
            $display("FAIL T2: cap_beats=%0d expected 4", cap_beats);
            $fatal;
        end
        // Magic "TMAR"
        check_byte(0, 8'h54);
        check_byte(1, 8'h4D);
        check_byte(2, 8'h41);
        check_byte(3, 8'h52);
        // frame_len = 12 = 0x000C
        check_byte(4, 8'h00);
        check_byte(5, 8'h0C);
        // reserved = 0
        check_byte(6, 8'h00);
        check_byte(7, 8'h00);
        // req_handle = 0x12345678
        check_byte(8,  8'h12);
        check_byte(9,  8'h34);
        check_byte(10, 8'h56);
        check_byte(11, 8'h78);
        // report_code = 0x04 (SuccessRandomAccess)
        check_byte(12, 8'h04);
        // pad
        check_byte(13, 8'h00);
        check_byte(14, 8'h00);
        check_byte(15, 8'h00);
        if (!last_beat_seen_tlast) begin
            $display("FAIL T2: tlast not asserted on last beat");
            $fatal;
        end
        if (tlm_tmar_cnt !== 32'd1) begin
            $display("FAIL T2: tlm_tmar_cnt=%0d expected 1", tlm_tmar_cnt);
            $fatal;
        end
        $display("[T2] TMAR SuccessRandomAccess report: PASS");

        // ============== Test 3: tready back-pressure stall ===========
        // Drive a small TMAR while gating tready off for a few cycles.
        capture_reset();
        m_axis_tready = 1'b0;
        @(negedge clk);
        tmar_handle = 32'hCAFE_BABE;
        tmar_code   = 8'h00;          // ConfirmHandle
        tmar_emit   = 1'b1;
        @(posedge clk);
        @(negedge clk);
        tmar_emit   = 1'b0;
        // Stall for a while.
        repeat (8) @(posedge clk);
        m_axis_tready = 1'b1;

        wait (m_axis_tlast && m_axis_tvalid && m_axis_tready);
        @(posedge clk);
        @(posedge clk);

        if (cap_beats !== 4) begin
            $display("FAIL T3: cap_beats=%0d expected 4", cap_beats);
            $fatal;
        end
        check_byte(8,  8'hCA);
        check_byte(9,  8'hFE);
        check_byte(10, 8'hBA);
        check_byte(11, 8'hBE);
        check_byte(12, 8'h00);   // ConfirmHandle
        if (tlm_tmar_cnt !== 32'd2) begin
            $display("FAIL T3: tlm_tmar_cnt=%0d expected 2", tlm_tmar_cnt);
            $fatal;
        end
        $display("[T3] TMAR with tready back-pressure: PASS");

        $display("PASS tb_tmasap_rx_framer (3/3 sub-tests)");
        $finish;
    end

    // Watchdog
    initial begin
        #200000;
        $display("FAIL tb_tmasap_rx_framer: watchdog timeout");
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
        $dumpvars(0, tb_tmasap_rx_framer);
    end
`endif

endmodule

`default_nettype wire
