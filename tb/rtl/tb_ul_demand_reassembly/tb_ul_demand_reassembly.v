// =============================================================================
// tb_ul_demand_reassembly.v — Bit-exact testbench against Gold-Ref vectors.
// =============================================================================
// Verilog-2001 only. Drives the carry-over UL Frag1+Frag2 bit slices from
// docs/references/reference_demand_reassembly_bitexact.md (M2 Gold-Ref hex
// `01 41 7F A7 01 12 66 34 20 C1 22 60` + `D4 1C 3C 02 40 50 2F 4D 61 20 00 00`)
// and the MTP3550 variant, asserts the reassembled 129-bit MM body matches
// bit-for-bit.
//
// Per A6 contract: 0/129 bit diff against Gold-Ref. >0 bits → FAIL exit non-0.
// =============================================================================

`timescale 1ns / 1ps
`default_nettype none

module tb_ul_demand_reassembly;
    // -------- DUT clock / reset --------
    reg clk_sys;
    reg rst_n_sys;

    // -------- DUT inputs --------
    reg [3:0]  t0_frames_sys;
    reg        frame_tick_sys;
    reg        frag1_pulse_sys;
    reg [23:0] frag1_ssi_sys;
    reg [43:0] frag1_bits_sys;
    reg        end_hu_pulse_sys;
    reg [23:0] end_hu_ssi_sys;
    reg [84:0] end_hu_bits_sys;

    // -------- DUT outputs --------
    wire        reassembled_valid_sys;
    wire [128:0] reassembled_body_sys;
    wire [23:0] reassembled_ssi_sys;
    wire [15:0] reassembled_cnt_sys;
    wire [15:0] drop_cnt_sys;
    wire [1:0]  busy_slots_sys;

    // -------- Clock --------
    initial clk_sys = 1'b0;
    always #5 clk_sys = ~clk_sys;   // 100 MHz

    // -------- DUT instance --------
    tetra_ul_demand_reassembly #(.T0_FRAMES_DEFAULT(2)) dut (
        .clk_sys(clk_sys),
        .rst_n_sys(rst_n_sys),
        .t0_frames_sys(t0_frames_sys),
        .frame_tick_sys(frame_tick_sys),
        .frag1_pulse_sys(frag1_pulse_sys),
        .frag1_ssi_sys(frag1_ssi_sys),
        .frag1_bits_sys(frag1_bits_sys),
        .end_hu_pulse_sys(end_hu_pulse_sys),
        .end_hu_ssi_sys(end_hu_ssi_sys),
        .end_hu_bits_sys(end_hu_bits_sys),
        .reassembled_valid_sys(reassembled_valid_sys),
        .reassembled_body_sys(reassembled_body_sys),
        .reassembled_ssi_sys(reassembled_ssi_sys),
        .reassembled_cnt_sys(reassembled_cnt_sys),
        .drop_cnt_sys(drop_cnt_sys),
        .busy_slots_sys(busy_slots_sys)
    );

    // -------- Gold-Ref vectors (computed from hex via Python helper) --------
    // M2 Gold-Ref capture:
    //   UL#0 hex: 01 41 7F A7 01 12 66 34 20 C1 22 60
    //     bits[48..91] = 44 bit fragment 1 = 44'h663420c1226
    //     SSI = 0x282FF4
    //   UL#1 hex: D4 1C 3C 02 40 50 2F 4D 61 20 00 00
    //     bits[7..91] = 85 bit continuation = 85'h01c3c0240502f4d6120000
    //   reassembled body[128..0] = 129'h0cc68418244c1c3c0240502f4d6120000
    localparam [23:0]  GOLD_SSI       = 24'h282FF4;
    localparam [43:0]  GOLD_FRAG1     = 44'h663420c1226;
    localparam [84:0]  GOLD_CONT      = 85'h01c3c0240502f4d6120000;
    localparam [128:0] GOLD_BODY      = 129'h0cc68418244c1c3c0240502f4d6120000;

    // MTP3550 capture (UL#0 = 01 41 7C 8F …, GSSI=0x000001):
    localparam [23:0]  MTP_SSI        = 24'h282F91;
    localparam [43:0]  MTP_FRAG1      = 44'h663420c1226;
    localparam [84:0]  MTP_CONT       = 85'h01c3c02405000000120000;
    localparam [128:0] MTP_BODY       = 129'h0cc68418244c1c3c02405000000120000;

    integer fail_count;
    integer total_diff_bits;

    // ---- helper: count differing bits between two 129-bit words ----
    function integer popcount129;
        input [128:0] x;
        integer i;
        begin
            popcount129 = 0;
            for (i = 0; i < 129; i = i + 1)
                popcount129 = popcount129 + x[i];
        end
    endfunction

    task drive_frag1;
        input [23:0] ssi;
        input [43:0] bits;
        begin
            @(posedge clk_sys);
            frag1_pulse_sys <= 1'b1;
            frag1_ssi_sys   <= ssi;
            frag1_bits_sys  <= bits;
            @(posedge clk_sys);
            frag1_pulse_sys <= 1'b0;
        end
    endtask

    task drive_end_hu;
        input [23:0] ssi;
        input [84:0] bits;
        begin
            @(posedge clk_sys);
            end_hu_pulse_sys <= 1'b1;
            end_hu_ssi_sys   <= ssi;
            end_hu_bits_sys  <= bits;
            @(posedge clk_sys);
            end_hu_pulse_sys <= 1'b0;
        end
    endtask

    task check_reassembly;
        input [255:0] tag;
        input [23:0]  expect_ssi;
        input [128:0] expect_body;
        integer       waits;
        integer       diff;
        begin
            // Wait up to a few cycles for reassembled_valid_sys
            waits = 0;
            while (!reassembled_valid_sys && waits < 8) begin
                @(posedge clk_sys);
                waits = waits + 1;
            end
            if (!reassembled_valid_sys) begin
                $display("FAIL [%0s]: reassembled_valid_sys never asserted", tag);
                fail_count = fail_count + 1;
            end else begin
                if (reassembled_ssi_sys !== expect_ssi) begin
                    $display("FAIL [%0s]: ssi mismatch got=0x%06h want=0x%06h",
                             tag, reassembled_ssi_sys, expect_ssi);
                    fail_count = fail_count + 1;
                end
                diff = popcount129(reassembled_body_sys ^ expect_body);
                total_diff_bits = total_diff_bits + diff;
                if (diff != 0) begin
                    $display("FAIL [%0s]: body bit-diff = %0d", tag, diff);
                    $display("        got = 129'h%033h", reassembled_body_sys);
                    $display("        want= 129'h%033h", expect_body);
                    fail_count = fail_count + 1;
                end else begin
                    $display("PASS [%0s]: reassembled_body bit-exact (129/129)",
                             tag);
                end
            end
        end
    endtask

    initial begin
        fail_count       = 0;
        total_diff_bits  = 0;
        rst_n_sys        = 1'b0;
        t0_frames_sys    = 4'd0;   // use default
        frame_tick_sys   = 1'b0;
        frag1_pulse_sys  = 1'b0;
        frag1_ssi_sys    = 24'd0;
        frag1_bits_sys   = 44'd0;
        end_hu_pulse_sys = 1'b0;
        end_hu_ssi_sys   = 24'd0;
        end_hu_bits_sys  = 85'd0;

        repeat (4) @(posedge clk_sys);
        rst_n_sys = 1'b1;
        repeat (2) @(posedge clk_sys);

        // ---- Scenario 1: Gold-Ref M2 attach reassembly ----
        drive_frag1(GOLD_SSI, GOLD_FRAG1);
        repeat (2) @(posedge clk_sys);
        drive_end_hu(GOLD_SSI, GOLD_CONT);
        check_reassembly("GOLD M2", GOLD_SSI, GOLD_BODY);

        repeat (4) @(posedge clk_sys);

        // ---- Scenario 2: MTP3550 reassembly ----
        drive_frag1(MTP_SSI, MTP_FRAG1);
        repeat (2) @(posedge clk_sys);
        drive_end_hu(MTP_SSI, MTP_CONT);
        check_reassembly("MTP3550", MTP_SSI, MTP_BODY);

        repeat (4) @(posedge clk_sys);

        // ---- Summary ----
        if (fail_count == 0 && total_diff_bits == 0) begin
            $display("==================================================");
            $display("PASS: tb_ul_demand_reassembly all checks bit-exact");
            $display("==================================================");
            $finish;
        end else begin
            $display("==================================================");
            $display("FAIL: tb_ul_demand_reassembly %0d failures, %0d diff bits",
                     fail_count, total_diff_bits);
            $display("==================================================");
            $stop;
        end
    end

    // Watchdog
    initial begin
        #100000;
        $display("FAIL: tb_ul_demand_reassembly watchdog");
        $stop;
    end

    // ---- T1: optional VCD dump (compile with -DVCDDUMP to enable) ---------
`ifdef VCDDUMP
    initial begin
`ifdef VCD_FILE
        $dumpfile(`VCD_FILE);
`else
        $dumpfile("dump.vcd");
`endif
        $dumpvars(0, tb_ul_demand_reassembly);
    end
`endif

endmodule

`default_nettype wire
