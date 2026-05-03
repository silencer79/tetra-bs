// tb_smoke.v — minimal RTL TB to prove the iverilog pipeline.
//
// Owned by T0 build-skeleton. Phase-2 RTL agents add real per-block TBs
// alongside this one; the _smoke TB stays as the canary that the build
// system itself works.
//
// Contract (see tb/rtl/Makefile.inc): print a single line containing "PASS"
// on success. Any other outcome is treated as failure.

`timescale 1ns / 1ps

module tb_smoke;

    initial begin
        // Trivial sanity check; if the simulator can't evaluate this,
        // the toolchain is broken and the whole pipeline is wrong.
        if (1 == 1) begin
            $display("PASS tb_smoke");
        end else begin
            $display("FAIL tb_smoke");
            $fatal;
        end
        $finish;
    end

endmodule
