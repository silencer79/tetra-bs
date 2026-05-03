// =============================================================================
// tb_dl_signal_scheduler.v — Sanity TB for tetra_dl_signal_scheduler.
// =============================================================================
// Verilog-2001 only. The scheduler is downstream of the queue and pulses
// pop_sys at slot_pulse_sys && tn_sys==3, then on the next posedge latches
// the queue head into one of four sched_blk1/2_tnX bundles depending on
// head_target_tn_sys. Idle slots get null_pdu_bits / sig_companion.
// =============================================================================

`timescale 1ns / 1ps
`default_nettype none

module tb_dl_signal_scheduler;
    reg clk_sys;
    reg rst_n_sys;

    reg [1:0] tn_sys;
    reg       slot_pulse_sys;

    wire        pop_sys;
    reg         head_valid_sys;
    reg [431:0] head_coded_sys;
    reg [1:0]   head_pdu_type_sys;
    reg [1:0]   head_target_tn_sys;
    reg [1:0]   head_prio_sys;
    reg         head_second_pdu_present_sys;
    reg         head_second_pdu_nr_sys;

    wire popped_second_pdu_present_sys;
    wire popped_second_pdu_nr_sys;

    reg [215:0] null_pdu_bits_sys;
    reg [215:0] sig_companion_sys;

    wire [215:0] sched_blk1_tn0_sys, sched_blk2_tn0_sys;
    wire [215:0] sched_blk1_tn1_sys, sched_blk2_tn1_sys;
    wire [215:0] sched_blk1_tn2_sys, sched_blk2_tn2_sys;
    wire [215:0] sched_blk1_tn3_sys, sched_blk2_tn3_sys;
    wire [3:0]   sched_ndb2_sys;
    wire [3:0]   sched_active_sys;
    wire [15:0]  override_cnt_sys, pop_cnt_sys;

    initial clk_sys = 1'b0;
    always #5 clk_sys = ~clk_sys;

    tetra_dl_signal_scheduler dut (
        .clk_sys(clk_sys),
        .rst_n_sys(rst_n_sys),
        .tn_sys(tn_sys),
        .slot_pulse_sys(slot_pulse_sys),
        .pop_sys(pop_sys),
        .head_valid_sys(head_valid_sys),
        .head_coded_sys(head_coded_sys),
        .head_pdu_type_sys(head_pdu_type_sys),
        .head_target_tn_sys(head_target_tn_sys),
        .head_prio_sys(head_prio_sys),
        .head_second_pdu_present_sys(head_second_pdu_present_sys),
        .head_second_pdu_nr_sys(head_second_pdu_nr_sys),
        .popped_second_pdu_present_sys(popped_second_pdu_present_sys),
        .popped_second_pdu_nr_sys(popped_second_pdu_nr_sys),
        .null_pdu_bits_sys(null_pdu_bits_sys),
        .sig_companion_sys(sig_companion_sys),
        .sched_blk1_tn0_sys(sched_blk1_tn0_sys),
        .sched_blk2_tn0_sys(sched_blk2_tn0_sys),
        .sched_blk1_tn1_sys(sched_blk1_tn1_sys),
        .sched_blk2_tn1_sys(sched_blk2_tn1_sys),
        .sched_blk1_tn2_sys(sched_blk1_tn2_sys),
        .sched_blk2_tn2_sys(sched_blk2_tn2_sys),
        .sched_blk1_tn3_sys(sched_blk1_tn3_sys),
        .sched_blk2_tn3_sys(sched_blk2_tn3_sys),
        .sched_ndb2_sys(sched_ndb2_sys),
        .sched_active_sys(sched_active_sys),
        .override_cnt_sys(override_cnt_sys),
        .pop_cnt_sys(pop_cnt_sys)
    );

    // Patch — re-instantiate? Keep single dut, use different wire names.
    integer fail_count;

    task tick;
        begin @(posedge clk_sys); end
    endtask

    // Pulse slot start for given tn (1-cycle slot_pulse strobe at tn).
    task slot_pulse_at_tn;
        input [1:0] tn;
        begin
            @(posedge clk_sys);
            tn_sys         <= tn;
            slot_pulse_sys <= 1'b1;
            @(posedge clk_sys);
            slot_pulse_sys <= 1'b0;
        end
    endtask

    initial begin
        fail_count = 0;
        rst_n_sys  = 1'b0;
        tn_sys     = 2'd0;
        slot_pulse_sys = 1'b0;
        head_valid_sys = 1'b0;
        head_coded_sys = 432'd0;
        head_pdu_type_sys = 2'd0;
        head_target_tn_sys = 2'd0;
        head_prio_sys = 2'd0;
        head_second_pdu_present_sys = 1'b0;
        head_second_pdu_nr_sys = 1'b0;
        null_pdu_bits_sys = {216{1'b1}};       // distinctive idle pattern
        sig_companion_sys = {{108{1'b0}}, {108{1'b1}}};

        repeat (4) @(posedge clk_sys);
        rst_n_sys = 1'b1;
        repeat (2) @(posedge clk_sys);

        // ==== Idle: queue empty. Pulse trigger at TN=3, expect no pop, no override
        head_valid_sys     <= 1'b0;
        slot_pulse_at_tn(2'd3);
        @(posedge clk_sys);
        if (pop_sys !== 1'b0) begin
            $display("FAIL [idle pop_sys]: pop_sys=%b want 0", pop_sys);
            fail_count = fail_count + 1;
        end else $display("PASS [idle pop_sys=0]");
        if (override_cnt_sys !== 16'd0) begin
            $display("FAIL [idle override_cnt]: %0d want 0", override_cnt_sys);
            fail_count = fail_count + 1;
        end else $display("PASS [idle override_cnt=0]");
        // All four ndb2 should be 1 (NULL-PDU is SCH/HD)
        if (sched_ndb2_sys !== 4'b1111) begin
            $display("FAIL [idle ndb2=1111]: got %b", sched_ndb2_sys);
            fail_count = fail_count + 1;
        end else $display("PASS [idle ndb2=1111]");

        repeat (2) @(posedge clk_sys);

        // ==== Active: queue has SCH/F PDU targeting TN=2
        head_valid_sys      <= 1'b1;
        head_coded_sys      <= {{8{8'hAB}}, {(432-64){1'b0}}, 32'hDEADBEEF};
        head_pdu_type_sys   <= 2'd0;     // SCH/F
        head_target_tn_sys  <= 2'd2;
        head_prio_sys       <= 2'd0;
        head_second_pdu_present_sys <= 1'b1;
        head_second_pdu_nr_sys      <= 1'b1;

        slot_pulse_at_tn(2'd3);
        // pop_sys is registered: it asserts on the cycle after the trigger.
        @(posedge clk_sys);
        if (pop_sys !== 1'b1) begin
            $display("FAIL [active pop_sys]: pop_sys=%b want 1", pop_sys);
            fail_count = fail_count + 1;
        end else $display("PASS [active pop_sys=1]");

        // Output registers should also have updated by now.
        if (sched_active_sys !== 4'b0100) begin
            $display("FAIL [sched_active TN2]: %b want 0100", sched_active_sys);
            fail_count = fail_count + 1;
        end else $display("PASS [sched_active=TN2]");
        // ndb2 should be 0 for TN2 (SCH/F path), 1 for the other three TNs
        if (sched_ndb2_sys !== 4'b1011) begin
            $display("FAIL [sched_ndb2 TN2 SCH_F]: %b want 1011", sched_ndb2_sys);
            fail_count = fail_count + 1;
        end else $display("PASS [sched_ndb2=1011]");
        // override_cnt incremented
        if (override_cnt_sys !== 16'd1) begin
            $display("FAIL [override_cnt=1]: %0d", override_cnt_sys);
            fail_count = fail_count + 1;
        end else $display("PASS [override_cnt=1]");
        // popped_second_pdu pass-through
        if (popped_second_pdu_present_sys !== 1'b1 ||
            popped_second_pdu_nr_sys      !== 1'b1) begin
            $display("FAIL [second_pdu pass-through]: present=%b nr=%b",
                     popped_second_pdu_present_sys, popped_second_pdu_nr_sys);
            fail_count = fail_count + 1;
        end else $display("PASS [second_pdu pass-through]");

        repeat (4) @(posedge clk_sys);

        if (fail_count == 0) begin
            $display("==================================================");
            $display("PASS: tb_dl_signal_scheduler all checks");
            $display("==================================================");
            $finish;
        end else begin
            $display("==================================================");
            $display("FAIL: tb_dl_signal_scheduler %0d failures", fail_count);
            $display("==================================================");
            $stop;
        end
    end

    initial begin
        #200000;
        $display("FAIL: tb_dl_signal_scheduler watchdog");
        $stop;
    end
endmodule

`default_nettype wire
