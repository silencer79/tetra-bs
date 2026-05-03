// =============================================================================
// tb_dl_signal_queue.v — Sanity TB for tetra_dl_signal_queue.
// =============================================================================
// Verilog-2001 only. No Gold-Ref bit-vector available for queue ordering;
// instead asserts the documented FIFO+priority spec from the source header:
//   - prio 0 (MLE) < prio 1 (CMCE) < prio 2 (SDS)
//   - tie within prio → lower slot index
//   - drop-newest on full
//   - producer-collision MLE > CMCE > SDS, losers count as drops
// =============================================================================

`timescale 1ns / 1ps
`default_nettype none

module tb_dl_signal_queue;
    reg clk;
    reg rst_n;

    reg          wr_mle_valid;
    reg  [431:0] wr_mle_coded;
    reg  [1:0]   wr_mle_pdu_type;
    reg  [1:0]   wr_mle_target_tn;
    reg          wr_mle_second_pdu_present;
    reg          wr_mle_second_pdu_nr;

    reg          wr_cmce_valid;
    reg  [431:0] wr_cmce_coded;
    reg  [1:0]   wr_cmce_pdu_type;
    reg  [1:0]   wr_cmce_target_tn;

    reg          wr_sds_valid;
    reg  [431:0] wr_sds_coded;
    reg  [1:0]   wr_sds_pdu_type;
    reg  [1:0]   wr_sds_target_tn;

    reg          pop;
    wire         head_valid;
    wire [431:0] head_coded;
    wire [1:0]   head_pdu_type;
    wire [1:0]   head_target_tn;
    wire [1:0]   head_prio;
    wire         head_second_pdu_present;
    wire         head_second_pdu_nr;

    wire [3:0]   depth_valid_mask;
    wire [2:0]   depth_count;
    wire [15:0]  drop_cnt;

    initial clk = 1'b0;
    always #5 clk = ~clk;

    tetra_dl_signal_queue #(.DEPTH(4)) dut (
        .clk(clk),
        .rst_n(rst_n),
        .wr_mle_valid(wr_mle_valid),
        .wr_mle_coded(wr_mle_coded),
        .wr_mle_pdu_type(wr_mle_pdu_type),
        .wr_mle_target_tn(wr_mle_target_tn),
        .wr_mle_second_pdu_present(wr_mle_second_pdu_present),
        .wr_mle_second_pdu_nr(wr_mle_second_pdu_nr),
        .wr_cmce_valid(wr_cmce_valid),
        .wr_cmce_coded(wr_cmce_coded),
        .wr_cmce_pdu_type(wr_cmce_pdu_type),
        .wr_cmce_target_tn(wr_cmce_target_tn),
        .wr_sds_valid(wr_sds_valid),
        .wr_sds_coded(wr_sds_coded),
        .wr_sds_pdu_type(wr_sds_pdu_type),
        .wr_sds_target_tn(wr_sds_target_tn),
        .pop(pop),
        .head_valid(head_valid),
        .head_coded(head_coded),
        .head_pdu_type(head_pdu_type),
        .head_target_tn(head_target_tn),
        .head_prio(head_prio),
        .head_second_pdu_present(head_second_pdu_present),
        .head_second_pdu_nr(head_second_pdu_nr),
        .depth_valid_mask(depth_valid_mask),
        .depth_count(depth_count),
        .drop_cnt(drop_cnt)
    );

    integer fail_count;

    // Pre-built tag payloads — bit positions encode their identity so we can
    // verify pop ordering by inspecting head_coded[7:0].
    function [431:0] mk_payload;
        input [7:0] tag;
        begin
            mk_payload = {{(424){1'b0}}, tag};
        end
    endfunction

    task tick;
        begin @(posedge clk); end
    endtask

    task clear_writes;
        begin
            wr_mle_valid <= 1'b0;
            wr_cmce_valid<= 1'b0;
            wr_sds_valid <= 1'b0;
            wr_mle_second_pdu_present <= 1'b0;
            wr_mle_second_pdu_nr      <= 1'b0;
        end
    endtask

    task push_mle;
        input [7:0] tag;
        input [1:0] tn;
        begin
            @(posedge clk);
            wr_mle_valid     <= 1'b1;
            wr_mle_coded     <= mk_payload(tag);
            wr_mle_pdu_type  <= 2'd0;
            wr_mle_target_tn <= tn;
            wr_mle_second_pdu_present <= 1'b0;
            wr_mle_second_pdu_nr      <= 1'b0;
            @(posedge clk);
            clear_writes;
        end
    endtask

    task push_cmce;
        input [7:0] tag;
        input [1:0] tn;
        begin
            @(posedge clk);
            wr_cmce_valid     <= 1'b1;
            wr_cmce_coded     <= mk_payload(tag);
            wr_cmce_pdu_type  <= 2'd0;
            wr_cmce_target_tn <= tn;
            @(posedge clk);
            clear_writes;
        end
    endtask

    task push_sds;
        input [7:0] tag;
        input [1:0] tn;
        begin
            @(posedge clk);
            wr_sds_valid     <= 1'b1;
            wr_sds_coded     <= mk_payload(tag);
            wr_sds_pdu_type  <= 2'd0;
            wr_sds_target_tn <= tn;
            @(posedge clk);
            clear_writes;
        end
    endtask

    task pop_one;
        input [255:0] tag_str;
        input [7:0]   want_tag;
        input [1:0]   want_prio;
        begin
            // Combinational head view should already match expectation
            if (!head_valid) begin
                $display("FAIL [%0s]: head_valid=0 (queue should not be empty)", tag_str);
                fail_count = fail_count + 1;
            end else if (head_coded[7:0] !== want_tag) begin
                $display("FAIL [%0s]: head_tag=0x%02h want=0x%02h",
                         tag_str, head_coded[7:0], want_tag);
                fail_count = fail_count + 1;
            end else if (head_prio !== want_prio) begin
                $display("FAIL [%0s]: head_prio=%0d want=%0d",
                         tag_str, head_prio, want_prio);
                fail_count = fail_count + 1;
            end else begin
                $display("PASS [%0s]: head_tag=0x%02h prio=%0d",
                         tag_str, head_coded[7:0], head_prio);
            end
            // Strobe pop for one cycle, then wait one more for head to settle.
            @(posedge clk);
            pop <= 1'b1;
            @(posedge clk);
            pop <= 1'b0;
            @(posedge clk);  // entry_valid clears here, combinational head re-evals
        end
    endtask

    initial begin
        fail_count = 0;
        rst_n = 1'b0;
        wr_mle_valid = 1'b0; wr_mle_coded = 432'd0;
        wr_mle_pdu_type = 2'd0; wr_mle_target_tn = 2'd0;
        wr_mle_second_pdu_present = 1'b0; wr_mle_second_pdu_nr = 1'b0;
        wr_cmce_valid = 1'b0; wr_cmce_coded = 432'd0;
        wr_cmce_pdu_type = 2'd0; wr_cmce_target_tn = 2'd0;
        wr_sds_valid = 1'b0; wr_sds_coded = 432'd0;
        wr_sds_pdu_type = 2'd0; wr_sds_target_tn = 2'd0;
        pop = 1'b0;

        repeat (4) @(posedge clk);
        rst_n = 1'b1;
        repeat (2) @(posedge clk);

        // ==== Scenario 1: priority order — push SDS, CMCE, MLE in that order
        push_sds (8'h11, 2'd0);   // slot 0, prio 2
        push_cmce(8'h22, 2'd0);   // slot 1, prio 1
        push_mle (8'h33, 2'd0);   // slot 2, prio 0
        @(posedge clk);

        // Expect MLE first (prio 0), then CMCE (prio 1), then SDS (prio 2)
        pop_one("priority MLE first",  8'h33, 2'd0);
        pop_one("priority CMCE second",8'h22, 2'd1);
        pop_one("priority SDS last",   8'h11, 2'd2);

        repeat (2) @(posedge clk);
        if (depth_count !== 3'd0) begin
            $display("FAIL [empty after 3 pops]: depth_count=%0d", depth_count);
            fail_count = fail_count + 1;
        end else $display("PASS [empty after 3 pops]");

        // ==== Scenario 2: FIFO within same priority (lower slot index wins)
        push_mle(8'hA0, 2'd0);    // slot 0
        push_mle(8'hA1, 2'd1);    // slot 1
        push_mle(8'hA2, 2'd2);    // slot 2
        @(posedge clk);
        pop_one("same-prio slot0 wins", 8'hA0, 2'd0);
        pop_one("same-prio slot1 next", 8'hA1, 2'd0);
        pop_one("same-prio slot2 last", 8'hA2, 2'd0);

        repeat (2) @(posedge clk);

        // ==== Scenario 3: drop-newest on full
        push_mle(8'hB0, 2'd0);
        push_mle(8'hB1, 2'd1);
        push_mle(8'hB2, 2'd2);
        push_mle(8'hB3, 2'd3);
        // queue full now; drop_cnt should be 0
        if (drop_cnt !== 16'd0) begin
            $display("FAIL [drop_cnt before full]: %0d", drop_cnt);
            fail_count = fail_count + 1;
        end else $display("PASS [drop_cnt=0 before full]");
        push_mle(8'hB4, 2'd0);    // should be dropped
        @(posedge clk);
        if (drop_cnt !== 16'd1) begin
            $display("FAIL [drop_cnt after full+1]: %0d (want 1)", drop_cnt);
            fail_count = fail_count + 1;
        end else $display("PASS [drop_cnt=1 after overflow]");

        // Pop everything to clean up
        pop_one("after-full pop B0", 8'hB0, 2'd0);
        pop_one("after-full pop B1", 8'hB1, 2'd0);
        pop_one("after-full pop B2", 8'hB2, 2'd0);
        pop_one("after-full pop B3", 8'hB3, 2'd0);

        repeat (2) @(posedge clk);

        // ==== Scenario 4: producer collision — all three fire same cycle
        // MLE wins, CMCE+SDS counted as 2 drops.
        @(posedge clk);
        wr_mle_valid     <= 1'b1; wr_mle_coded <= mk_payload(8'hC0);
        wr_mle_pdu_type  <= 2'd0; wr_mle_target_tn <= 2'd0;
        wr_cmce_valid    <= 1'b1; wr_cmce_coded <= mk_payload(8'hC1);
        wr_cmce_pdu_type <= 2'd0; wr_cmce_target_tn <= 2'd0;
        wr_sds_valid     <= 1'b1; wr_sds_coded <= mk_payload(8'hC2);
        wr_sds_pdu_type  <= 2'd0; wr_sds_target_tn <= 2'd0;
        @(posedge clk);
        clear_writes;
        @(posedge clk);

        pop_one("collision MLE survives", 8'hC0, 2'd0);

        // drop_cnt should be old (1) + 2 collision losers = 3
        if (drop_cnt !== 16'd3) begin
            $display("FAIL [drop_cnt after collision]: %0d (want 3)", drop_cnt);
            fail_count = fail_count + 1;
        end else $display("PASS [drop_cnt=3 after collision]");

        repeat (4) @(posedge clk);

        if (fail_count == 0) begin
            $display("==================================================");
            $display("PASS: tb_dl_signal_queue all checks");
            $display("==================================================");
            $finish;
        end else begin
            $display("==================================================");
            $display("FAIL: tb_dl_signal_queue %0d failures", fail_count);
            $display("==================================================");
            $stop;
        end
    end

    initial begin
        #200000;
        $display("FAIL: tb_dl_signal_queue watchdog");
        $stop;
    end
endmodule

`default_nettype wire
