// tb_cdc_handshake.v — drive N words across cdc_handshake at three different
// src/dst clock ratios (1:1, 1:3, 3:1) and check N words received intact.

`timescale 1ns/1ps
`default_nettype none

module tb_cdc_handshake;

    parameter WIDTH = 32;

    // Three lanes with different clock ratios.
    reg [2:0] src_clk = 3'b0;
    reg [2:0] dst_clk = 3'b0;
    always #5  src_clk[0] = ~src_clk[0];
    always #5  dst_clk[0] = ~dst_clk[0];   // 1:1
    always #5  src_clk[1] = ~src_clk[1];
    always #15 dst_clk[1] = ~dst_clk[1];   // 1:3 (dst slower)
    always #15 src_clk[2] = ~src_clk[2];
    always #5  dst_clk[2] = ~dst_clk[2];   // 3:1 (src slower)

    reg  [WIDTH-1:0] src_data  [0:2];
    reg  [2:0]       src_valid = 3'b0;
    wire [2:0]       src_ready;
    wire [WIDTH-1:0] dst_data  [0:2];
    wire [2:0]       dst_valid;
    reg  [2:0]       dst_ack   = 3'b0;

    genvar gi;
    generate
        for (gi = 0; gi < 3; gi = gi + 1) begin : g_dut
            cdc_handshake #(.WIDTH(WIDTH)) u (
                .src_clk  (src_clk[gi]),
                .src_data (src_data[gi]),
                .src_valid(src_valid[gi]),
                .src_ready(src_ready[gi]),
                .dst_clk  (dst_clk[gi]),
                .dst_data (dst_data[gi]),
                .dst_valid(dst_valid[gi]),
                .dst_ack  (dst_ack[gi])
            );
        end
    endgenerate

    initial begin
        src_data[0] = {WIDTH{1'b0}};
        src_data[1] = {WIDTH{1'b0}};
        src_data[2] = {WIDTH{1'b0}};
    end

    // Capture received words per lane.
    integer rx_count [0:2];
    reg [WIDTH-1:0] rx_words [0:2][0:127];
    integer i;
    initial begin
        for (i = 0; i < 3; i = i + 1) rx_count[i] = 0;
    end

    // DST consumer: 1-cycle ack on each valid pulse.
    integer L;
    always @(posedge dst_clk[0]) begin
        dst_ack[0] <= 1'b0;
        if (dst_valid[0]) begin
            rx_words[0][rx_count[0]] = dst_data[0];
            rx_count[0]              = rx_count[0] + 1;
            dst_ack[0] <= 1'b1;
        end
    end
    always @(posedge dst_clk[1]) begin
        dst_ack[1] <= 1'b0;
        if (dst_valid[1]) begin
            rx_words[1][rx_count[1]] = dst_data[1];
            rx_count[1]              = rx_count[1] + 1;
            dst_ack[1] <= 1'b1;
        end
    end
    always @(posedge dst_clk[2]) begin
        dst_ack[2] <= 1'b0;
        if (dst_valid[2]) begin
            rx_words[2][rx_count[2]] = dst_data[2];
            rx_count[2]              = rx_count[2] + 1;
            dst_ack[2] <= 1'b1;
        end
    end

    integer errors = 0;
    parameter integer N_WORDS = 16;

    reg [WIDTH-1:0] tx_words [0:N_WORDS-1];

    task send_one;
        input integer lane;
        input [WIDTH-1:0] word;
        begin
            // Wait until ready, sampled after a clock edge.
            @(posedge src_clk[lane]);
            while (!src_ready[lane])
                @(posedge src_clk[lane]);
            // Drive data + valid just after the edge (#1 settle) so they
            // are stable before the NEXT edge where the DUT samples.
            #1;
            src_data[lane]  = word;
            src_valid[lane] = 1'b1;
            @(posedge src_clk[lane]);
            // DUT consumed the request on this edge; deassert valid.
            #1;
            src_valid[lane] = 1'b0;
        end
    endtask

    task run_lane;
        input integer lane;
        integer i;
        begin
            rx_count[lane] = 0;
            for (i = 0; i < N_WORDS; i = i + 1) begin
                send_one(lane, tx_words[i]);
            end
            // Wait for all words to land in DST.
            while (rx_count[lane] < N_WORDS) begin
                @(posedge dst_clk[lane]);
            end
            // Verify intact + ordered.
            for (i = 0; i < N_WORDS; i = i + 1) begin
                if (rx_words[lane][i] !== tx_words[i]) begin
                    $display("[FAIL] lane=%0d idx=%0d got=%h expected=%h",
                             lane, i, rx_words[lane][i], tx_words[i]);
                    errors = errors + 1;
                end
            end
            $display("[ok] lane=%0d transferred %0d words", lane, N_WORDS);
        end
    endtask

    initial begin
        // Init test vector.
        for (i = 0; i < N_WORDS; i = i + 1)
            tx_words[i] = $random ^ (32'hC0FFEE00 + i);

        // Settle.
        repeat (10) @(posedge src_clk[0]);

        run_lane(0);
        run_lane(1);
        run_lane(2);

        if (errors == 0) begin
            $display("PASS tb_cdc_handshake");
            $finish(0);
        end else begin
            $display("FAIL tb_cdc_handshake errors=%0d", errors);
            $finish(1);
        end
    end

    initial begin
        #5_000_000;
        $display("FAIL tb_cdc_handshake watchdog timeout");
        $finish(1);
    end

endmodule

`default_nettype wire
