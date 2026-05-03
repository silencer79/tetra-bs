// =============================================================================
// tb_sch_f_encoder.v — Bit-exact testbench for tetra_sch_f_encoder.
// =============================================================================
// Verilog-2001 only.  Drives a deterministic 268-bit info pattern (1010...)
// plus the Gold-Cell scramble init `0x4183F207` (per `gen_d_nwrk_broadcast.py`
// in gold_field_values.md), and asserts the 432-bit coded output matches the
// reference vector produced by the Python pipeline at
// `scripts/ref_sch_f_encode.py` — the same algorithmic chain (CRC-16-CCITT
// + RCPC R1/4 + R2/3 puncture + a=103 interleave + Fibonacci scramble) the
// RTL implements.
//
// Bit-exact gate: 0/432 bits diff.  >0 ⇒ FAIL.
// =============================================================================

`timescale 1ns / 1ps
`default_nettype none

module tb_sch_f_encoder;
    reg         clk;
    reg         rst_n;
    reg         encode_start;
    reg [267:0] info_bits;
    reg [31:0]  scramble_init;

    wire [431:0] coded_bits;
    wire         coded_valid;

    initial clk = 1'b0;
    always #5 clk = ~clk;

    tetra_sch_f_encoder dut (
        .clk(clk),
        .rst_n(rst_n),
        .encode_start(encode_start),
        .info_bits(info_bits),
        .scramble_init(scramble_init),
        .coded_bits(coded_bits),
        .coded_valid(coded_valid)
    );

    // ---- Reference vectors (computed by scripts/ref_sch_f_encode.py) ----
    // info pattern = 1010...1010 (268 bits, info[0]=1, info[1]=0, …),
    //   packed so info_bits[267] = first on-air bit = 1.
    // scramble_init = Gold-Cell value 0x4183F207.
    localparam [267:0] INFO_TEST =
        268'haaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa;
    localparam [31:0]  SCRAMBLE_INIT = 32'h4183F207;
    localparam [431:0] CODED_EXPECT  =
        432'h37fb4be7f0c645d526952b5c841315e2f552f9b3cea72db219b6a6d0e6111b896147fdd95849dc550e0c4e7c2f12cea8da303de7c478;

    integer fail_count;
    integer diff_bits;
    integer i;

    initial begin
        fail_count = 0;
        diff_bits  = 0;
        rst_n      = 1'b0;
        encode_start  = 1'b0;
        info_bits     = 268'd0;
        scramble_init = 32'd0;

        repeat (4) @(posedge clk);
        rst_n = 1'b1;
        repeat (2) @(posedge clk);

        // Pulse encode_start with the test pattern.
        @(posedge clk);
        info_bits     <= INFO_TEST;
        scramble_init <= SCRAMBLE_INIT;
        encode_start  <= 1'b1;
        @(posedge clk);
        encode_start  <= 1'b0;

        // Wait for coded_valid (latency ~1 + 268 + 1 + 288 + 432 + 1 ≈ 991).
        i = 0;
        while (!coded_valid && i < 2000) begin
            @(posedge clk);
            i = i + 1;
        end
        if (!coded_valid) begin
            $display("FAIL [tb_sch_f_encoder]: coded_valid never asserted");
            fail_count = fail_count + 1;
        end else begin
            // Bit-diff against expected.
            for (i = 0; i < 432; i = i + 1) begin
                if (coded_bits[i] !== CODED_EXPECT[i])
                    diff_bits = diff_bits + 1;
            end
            if (diff_bits == 0) begin
                $display("PASS [tb_sch_f_encoder]: 432/432 bits match");
            end else begin
                $display("FAIL [tb_sch_f_encoder]: %0d/432 bit diff", diff_bits);
                $display("  got = 432'h%0108h", coded_bits);
                $display("  want= 432'h%0108h", CODED_EXPECT);
                fail_count = fail_count + 1;
            end
        end

        repeat (4) @(posedge clk);

        if (fail_count == 0 && diff_bits == 0) begin
            $display("==================================================");
            $display("PASS: tb_sch_f_encoder bit-exact");
            $display("==================================================");
            $finish;
        end else begin
            $display("==================================================");
            $display("FAIL: tb_sch_f_encoder %0d failures, %0d diff bits",
                     fail_count, diff_bits);
            $display("==================================================");
            $stop;
        end
    end

    initial begin
        #1000000;
        $display("FAIL: tb_sch_f_encoder watchdog");
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
        $dumpvars(0, tb_sch_f_encoder);
    end
`endif

endmodule

`default_nettype wire
