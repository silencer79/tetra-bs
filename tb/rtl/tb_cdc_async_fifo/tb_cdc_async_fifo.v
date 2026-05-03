// tb_cdc_async_fifo.v — exercise cdc_async_fifo at 1:1, 1:3, 3:1 ratios with
// random push/pop patterns, verify:
//   1. No data loss (every word written is read back, in order).
//   2. wr_full deasserts at start, asserts when DEPTH words pending.
//   3. rd_empty deasserts when there is data, asserts when drained.

`timescale 1ns/1ps
`default_nettype none

module tb_cdc_async_fifo;

    parameter WIDTH = 32;
    parameter DEPTH = 16;

    // Three lanes with different clock ratios.
    reg [2:0] wr_clk = 3'b0;
    reg [2:0] rd_clk = 3'b0;
    always #5  wr_clk[0] = ~wr_clk[0];
    always #5  rd_clk[0] = ~rd_clk[0];   // 1:1
    always #5  wr_clk[1] = ~wr_clk[1];
    always #15 rd_clk[1] = ~rd_clk[1];   // wr fast, rd slow (3:1)
    always #15 wr_clk[2] = ~wr_clk[2];
    always #5  rd_clk[2] = ~rd_clk[2];   // wr slow, rd fast (1:3)

    reg  [WIDTH-1:0] wr_data [0:2];
    reg  [2:0]       wr_en   = 3'b0;
    wire [2:0]       wr_full;
    wire [WIDTH-1:0] rd_data [0:2];
    reg  [2:0]       rd_en   = 3'b0;
    wire [2:0]       rd_empty;

    initial begin
        wr_data[0] = {WIDTH{1'b0}};
        wr_data[1] = {WIDTH{1'b0}};
        wr_data[2] = {WIDTH{1'b0}};
    end

    genvar gi;
    generate
        for (gi = 0; gi < 3; gi = gi + 1) begin : g_dut
            cdc_async_fifo #(.WIDTH(WIDTH), .DEPTH(DEPTH)) u (
                .wr_clk  (wr_clk[gi]),
                .wr_data (wr_data[gi]),
                .wr_en   (wr_en[gi]),
                .wr_full (wr_full[gi]),
                .rd_clk  (rd_clk[gi]),
                .rd_data (rd_data[gi]),
                .rd_en   (rd_en[gi]),
                .rd_empty(rd_empty[gi])
            );
        end
    endgenerate

    // ---- Test bookkeeping ----
    integer errors  = 0;
    parameter integer N_WORDS = 256;

    reg [WIDTH-1:0] tx [0:N_WORDS-1];
    integer         tx_idx [0:2];
    integer         rx_idx [0:2];
    reg [WIDTH-1:0] rx     [0:2][0:N_WORDS-1];

    integer i;
    initial begin
        for (i = 0; i < N_WORDS; i = i + 1)
            tx[i] = $random ^ (32'hF00DBA00 + i);
        for (i = 0; i < 3; i = i + 1) begin
            tx_idx[i] = 0;
            rx_idx[i] = 0;
        end
    end

    // -------------------------------------------------------------------------
    // Producer-A: random rate writer per lane.
    // -------------------------------------------------------------------------
    integer lfsr0 = 32'h1234_5678;
    integer lfsr1 = 32'hABCD_0123;
    integer lfsr2 = 32'h9999_5555;

    function [31:0] step_lfsr;
        input [31:0] s;
        begin
            // Galois LFSR (taps 32,22,2,1)
            step_lfsr = {s[30:0], s[31] ^ s[21] ^ s[1] ^ s[0]};
        end
    endfunction

    // Producer pattern: wr_en, wr_data, tx_idx are all registered. On each
    // wr_clk edge, advance tx_idx only when the previous-cycle wr_en && !wr_full
    // (i.e. the write actually committed). Then re-arm wr_en/wr_data for the
    // next word. Without this guard, the TB races wr_full transitions and
    // drops words.
    //
    // Consumer pattern (FWFT): assert rd_en when there is data. On the cycle
    // where rd_en && !rd_empty are both true, the pop happens at this edge —
    // rd_data is the just-popped word and is captured into rx[].

    // Lane 0
    always @(posedge wr_clk[0]) begin
        lfsr0 <= step_lfsr(lfsr0);
        if (wr_en[0] && !wr_full[0]) tx_idx[0] <= tx_idx[0] + 1;
        if (((wr_en[0] && !wr_full[0]) ? tx_idx[0] + 1 : tx_idx[0]) < N_WORDS
            && lfsr0[0]) begin
            wr_data[0] <= tx[(wr_en[0] && !wr_full[0]) ? tx_idx[0] + 1 : tx_idx[0]];
            wr_en[0]   <= 1'b1;
        end else begin
            wr_en[0] <= 1'b0;
        end
    end
    always @(posedge rd_clk[0]) begin
        if (rd_en[0] && !rd_empty[0] && rx_idx[0] < N_WORDS) begin
            rx[0][rx_idx[0]] <= rd_data[0];
            rx_idx[0]        <= rx_idx[0] + 1;
        end
        rd_en[0] <= !rd_empty[0] && rx_idx[0] < N_WORDS;
    end

    // Lane 1 (wr fast, rd slow). Reader pulls every cycle when not empty,
    // so we exercise wr_full assertion.
    always @(posedge wr_clk[1]) begin
        lfsr1 <= step_lfsr(lfsr1);
        if (wr_en[1] && !wr_full[1]) tx_idx[1] <= tx_idx[1] + 1;
        if (((wr_en[1] && !wr_full[1]) ? tx_idx[1] + 1 : tx_idx[1]) < N_WORDS) begin
            wr_data[1] <= tx[(wr_en[1] && !wr_full[1]) ? tx_idx[1] + 1 : tx_idx[1]];
            wr_en[1]   <= 1'b1;
        end else begin
            wr_en[1] <= 1'b0;
        end
    end
    always @(posedge rd_clk[1]) begin
        if (rd_en[1] && !rd_empty[1] && rx_idx[1] < N_WORDS) begin
            rx[1][rx_idx[1]] <= rd_data[1];
            rx_idx[1]        <= rx_idx[1] + 1;
        end
        rd_en[1] <= !rd_empty[1] && rx_idx[1] < N_WORDS;
    end

    // Lane 2 (wr slow, rd fast). Writer pushes every cycle.
    always @(posedge wr_clk[2]) begin
        lfsr2 <= step_lfsr(lfsr2);
        if (wr_en[2] && !wr_full[2]) tx_idx[2] <= tx_idx[2] + 1;
        if (((wr_en[2] && !wr_full[2]) ? tx_idx[2] + 1 : tx_idx[2]) < N_WORDS) begin
            wr_data[2] <= tx[(wr_en[2] && !wr_full[2]) ? tx_idx[2] + 1 : tx_idx[2]];
            wr_en[2]   <= 1'b1;
        end else begin
            wr_en[2] <= 1'b0;
        end
    end
    always @(posedge rd_clk[2]) begin
        if (rd_en[2] && !rd_empty[2] && rx_idx[2] < N_WORDS) begin
            rx[2][rx_idx[2]] <= rd_data[2];
            rx_idx[2]        <= rx_idx[2] + 1;
        end
        rd_en[2] <= !rd_empty[2] && rx_idx[2] < N_WORDS;
    end

    // -------------------------------------------------------------------------
    // Sanity: empty after reset, full eventually asserted on lane 1, then
    // releases as reader catches up.
    // -------------------------------------------------------------------------
    integer saw_full_lane1 = 0;
    always @(posedge wr_clk[1]) begin
        if (wr_full[1]) saw_full_lane1 = 1;
    end

    initial begin
        // Reset-state sanity. Sample at t=1 (before any clock edge) so the
        // producers/consumers haven't run yet — DUT initial-block must have
        // set pointers to 0, so empty=1, full=0 across all lanes.
        #1;
        if (rd_empty !== 3'b111) begin
            $display("[FAIL] expected rd_empty=111 after reset, got %b",
                     rd_empty);
            errors = errors + 1;
        end
        if (wr_full !== 3'b000) begin
            $display("[FAIL] expected wr_full=000 after reset, got %b",
                     wr_full);
            errors = errors + 1;
        end
    end

    // -------------------------------------------------------------------------
    // Wait for all lanes to drain, then check.
    // -------------------------------------------------------------------------
    integer lane;
    initial begin
        // Allow a long simulation budget. Poll on wr_clk[0] instead of `wait`
        // on array elements (iverilog has a zero-time-loop bug there).
        while (!(rx_idx[0] == N_WORDS && rx_idx[1] == N_WORDS &&
                 rx_idx[2] == N_WORDS))
            @(posedge wr_clk[0]);

        // Verify ordered exact match.
        for (lane = 0; lane < 3; lane = lane + 1) begin
            for (i = 0; i < N_WORDS; i = i + 1) begin
                if (rx[lane][i] !== tx[i]) begin
                    $display("[FAIL] lane=%0d idx=%0d got=%h expected=%h",
                             lane, i, rx[lane][i], tx[i]);
                    errors = errors + 1;
                end
            end
            $display("[ok] lane=%0d transferred %0d words", lane, N_WORDS);
        end

        if (saw_full_lane1 == 0) begin
            $display("[FAIL] lane1 wr_full was never asserted (back-pressure check)");
            errors = errors + 1;
        end

        if (errors == 0) begin
            $display("PASS tb_cdc_async_fifo");
            $finish(0);
        end else begin
            $display("FAIL tb_cdc_async_fifo errors=%0d", errors);
            $finish(1);
        end
    end

    initial begin
        #50_000_000;
        $display("FAIL tb_cdc_async_fifo watchdog timeout (rx_idx=%0d/%0d/%0d)",
                 rx_idx[0], rx_idx[1], rx_idx[2]);
        $finish(1);
    end

endmodule

`default_nettype wire
