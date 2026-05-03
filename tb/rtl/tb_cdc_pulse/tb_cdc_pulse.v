// tb_cdc_pulse.v — verifies cdc_pulse:
//   1. A single src pulse produces exactly 1 dst_pulse cycle.
//   2. Multiple spaced src pulses produce the same number of dst pulses
//      (no merging).
//   3. Works at multiple src/dst clock ratios (1:1, 1:4, 4:1).
//
// We instantiate four DUT copies, each driven by a fixed-period pair of
// clocks (the iverilog forever-with-variable-delay pattern is fragile under
// parameter changes — better to instantiate per scenario).

`timescale 1ns/1ps
`default_nettype none

module tb_cdc_pulse;

    // ---- four pairs of clocks ----
    reg [3:0] src_clk = 4'b0;
    reg [3:0] dst_clk = 4'b0;

    // s1: ~1:3   (src 100 MHz / dst 38.5 MHz) — half-periods 5/13
    // s2: 1:1    (50 MHz/50 MHz) — 10/10
    // s3: 1:4    (src 25 MHz / dst 100 MHz) — 20/5
    // s4: 4:1    (src 100 MHz / dst 25 MHz) — 5/20
    always #5  src_clk[0] = ~src_clk[0];
    always #13 dst_clk[0] = ~dst_clk[0];
    always #10 src_clk[1] = ~src_clk[1];
    always #10 dst_clk[1] = ~dst_clk[1];
    always #20 src_clk[2] = ~src_clk[2];
    always #5  dst_clk[2] = ~dst_clk[2];
    always #5  src_clk[3] = ~src_clk[3];
    always #20 dst_clk[3] = ~dst_clk[3];

    // ---- four DUTs ----
    reg  [3:0] src_pulse = 4'b0;
    wire [3:0] dst_pulse;

    genvar gi;
    generate
        for (gi = 0; gi < 4; gi = gi + 1) begin : g_dut
            cdc_pulse u (
                .src_clk  (src_clk[gi]),
                .dst_clk  (dst_clk[gi]),
                .src_pulse(src_pulse[gi]),
                .dst_pulse(dst_pulse[gi])
            );
        end
    endgenerate

    integer errors = 0;
    integer dst_pulses [0:3];
    integer s;
    initial begin
        for (s = 0; s < 4; s = s + 1) dst_pulses[s] = 0;
    end

    always @(posedge dst_clk[0]) if (dst_pulse[0]) dst_pulses[0] = dst_pulses[0] + 1;
    always @(posedge dst_clk[1]) if (dst_pulse[1]) dst_pulses[1] = dst_pulses[1] + 1;
    always @(posedge dst_clk[2]) if (dst_pulse[2]) dst_pulses[2] = dst_pulses[2] + 1;
    always @(posedge dst_clk[3]) if (dst_pulse[3]) dst_pulses[3] = dst_pulses[3] + 1;

    // Issue ONE pulse on lane `lane` (1 src-clk-cycle wide).
    task issue_pulse;
        input integer lane;
        begin
            @(posedge src_clk[lane]);
            #1 src_pulse[lane] = 1'b1;
            @(posedge src_clk[lane]);
            #1 src_pulse[lane] = 1'b0;
        end
    endtask

    // Settle: at least 8 of each clock so the toggle is fully crossed and
    // the dst-side edge-detector has fired.
    task settle_lane;
        input integer lane;
        begin
            repeat (12) @(posedge dst_clk[lane]);
            repeat (12) @(posedge src_clk[lane]);
        end
    endtask

    task run_lane;
        input integer lane;
        input integer n_pulses;
        integer i;
        begin
            // quiet
            repeat (20) @(posedge dst_clk[lane]);
            dst_pulses[lane] = 0;
            for (i = 0; i < n_pulses; i = i + 1) begin
                issue_pulse(lane);
                settle_lane(lane);
            end
            if (dst_pulses[lane] !== n_pulses) begin
                $display("[FAIL] lane=%0d dst_pulses=%0d expected=%0d",
                         lane, dst_pulses[lane], n_pulses);
                errors = errors + 1;
            end else begin
                $display("[ok] lane=%0d dst_pulses=%0d", lane,
                         dst_pulses[lane]);
            end
        end
    endtask

    initial begin
        // Power-on settle so the synchronizer chain holds 0.
        repeat (20) @(posedge dst_clk[0]);

        run_lane(0, 5);
        run_lane(1, 5);
        run_lane(2, 4);
        run_lane(3, 4);

        if (errors == 0) begin
            $display("PASS tb_cdc_pulse");
            $finish(0);
        end else begin
            $display("FAIL tb_cdc_pulse errors=%0d", errors);
            $finish(1);
        end
    end

    initial begin
        #5_000_000;
        $display("FAIL tb_cdc_pulse watchdog timeout");
        $finish(1);
    end

    // ---- T1: optional VCD dump (compile with -DVCDDUMP to enable) ---------
`ifdef VCDDUMP
    initial begin
`ifdef VCD_FILE
        $dumpfile(`VCD_FILE);
`else
        $dumpfile("dump.vcd");
`endif
        $dumpvars(0, tb_cdc_pulse);
    end
`endif

endmodule

`default_nettype wire
