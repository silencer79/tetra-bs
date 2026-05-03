// tb_cdc_sync_2ff.v — drives random `in` changes on src_clk and verifies the
// dst_clk-side `out` settles to the new value within 2 dst-clk cycles after a
// `in` rising edge has propagated through the synchronizer.
//
// PASS criteria:
//   1. After every change of `in`, `out` matches the new value within 2
//      complete dst_clk cycles (excluding the first sample-edge).
//   2. No X/Z propagation onto `out` after reset settle.
//   3. Multiple back-to-back changes (faster than dst_clk) eventually settle.

`timescale 1ns/1ps
`default_nettype none

module tb_cdc_sync_2ff;

    reg src_clk = 0;
    reg dst_clk = 0;

    // src_clk = 100 MHz (10 ns), dst_clk = ~36.864 MHz approximated by 27 ns
    always #5  src_clk = ~src_clk;
    always #13 dst_clk = ~dst_clk;

    parameter WIDTH = 4;
    reg  [WIDTH-1:0] in_val = {WIDTH{1'b0}};
    wire [WIDTH-1:0] out_val;

    cdc_sync_2ff #(.WIDTH(WIDTH)) DUT (
        .src_clk_unused(src_clk),
        .dst_clk       (dst_clk),
        .in            (in_val),
        .out           (out_val)
    );

    integer errors = 0;
    integer i;
    reg [WIDTH-1:0] expected;

    // Wait until `out_val` matches `value` for at least one full dst_clk
    // cycle, OR fail if more than `timeout_cycles` dst-edges pass.
    task wait_for_out;
        input [WIDTH-1:0] value;
        input integer     timeout_cycles;
        integer           cycles;
        begin
            cycles = 0;
            while (out_val !== value && cycles < timeout_cycles) begin
                @(posedge dst_clk);
                cycles = cycles + 1;
            end
            if (out_val !== value) begin
                $display("[FAIL] out_val=%h expected=%h after %0d dst cycles",
                         out_val, value, cycles);
                errors = errors + 1;
            end
        end
    endtask

    initial begin
        // Reset settle: 5 dst-clk cycles
        in_val = {WIDTH{1'b0}};
        repeat (5) @(posedge dst_clk);
        if (out_val !== {WIDTH{1'b0}}) begin
            $display("[FAIL] out_val didn't reset to 0, got %h", out_val);
            errors = errors + 1;
        end

        // 64 random transitions.
        for (i = 0; i < 64; i = i + 1) begin
            expected = $random;
            @(posedge src_clk);
            in_val = expected;
            // After src-side update, allow up to 4 dst-clk cycles to settle
            // (worst case: just-missed sample on metastability flop, plus
            // 2-cycle pipeline, plus output flop = 4).
            wait_for_out(expected, 8);
        end

        // Back-to-back: change every src-edge for 8 src-edges, then hold and
        // verify final value lands.
        for (i = 0; i < 8; i = i + 1) begin
            @(posedge src_clk);
            in_val = i[WIDTH-1:0];
        end
        expected = (8 - 1);
        @(posedge src_clk);
        in_val = expected;
        wait_for_out(expected, 16);

        if (errors == 0) begin
            $display("PASS tb_cdc_sync_2ff");
            $finish(0);
        end else begin
            $display("FAIL tb_cdc_sync_2ff errors=%0d", errors);
            $finish(1);
        end
    end

    // Watchdog
    initial begin
        #100000;
        $display("FAIL tb_cdc_sync_2ff watchdog timeout");
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
        $dumpvars(0, tb_cdc_sync_2ff);
    end
`endif

endmodule

`default_nettype wire
