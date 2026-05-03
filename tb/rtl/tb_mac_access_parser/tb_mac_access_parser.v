// =============================================================================
// tb_mac_access_parser.v — Bit-exact testbench for tetra_ul_mac_access_parser.
// =============================================================================
// Verilog-2001 only. Drives the Gold-Ref UL#0 hex `01 41 7F A7 01 12 66 34
// 20 C1 22 60` into the parser via info_bits_sys (MSB-first, bit[0] = first
// on-air bit) and asserts every parsed field matches the field-table in
// docs/references/reference_gold_attach_bitexact.md.
// =============================================================================

`timescale 1ns / 1ps
`default_nettype none

module tb_mac_access_parser;
    // -------- DUT clock / reset --------
    reg clk_sys;
    reg rst_n_sys;

    // -------- DUT inputs --------
    reg [91:0] info_bits_sys;
    reg        info_valid_sys;
    reg        crc_ok_sys;

    // -------- DUT outputs --------
    wire        pdu_type_sys;
    wire        fill_bit_sys;
    wire        encryption_mode_sys;
    wire [1:0]  ul_addr_type_sys;
    wire [23:0] ul_issi_sys;
    wire [9:0]  ul_event_label_sys;
    wire        optional_field_flag_sys;
    wire        ul_frag_flag_sys;
    wire [3:0]  ul_reservation_req_sys;
    wire [4:0]  ul_length_ind_sys;
    wire [3:0]  mm_pdu_type_sys;
    wire [2:0]  loc_upd_type_sys;
    wire [91:0] raw_info_bits_sys;
    wire        pdu_valid_sys;
    wire [15:0] pdu_count_sys;
    wire        bl_ack_valid_sys;
    wire        bl_ack_nr_sys;
    wire [15:0] bl_ack_count_sys;
    wire        ul_llc_is_bl_data_sys;
    wire        ul_llc_is_bl_ack_sys;
    wire        ul_llc_has_fcs_sys;
    wire        ul_llc_ns_valid_sys;
    wire        ul_llc_ns_sys;
    wire        ul_llc_nr_valid_sys;
    wire        ul_llc_nr_sys;
    wire        ul_llc_is_mle_mm_sys;
    wire [3:0]  ul_llc_mm_pdu_type_sys;
    wire [2:0]  ul_llc_mm_loc_upd_type_sys;
    wire [3:0]  ul_llc_pdu_type_sys;
    wire [2:0]  ul_mle_disc_sys;
    wire        ul_pdu_is_continuation_sys;
    wire        ul_continuation_valid_sys;
    wire [84:0] ul_continuation_bits_sys;
    wire [23:0] ul_continuation_ssi_sys;
    wire [15:0] ul_continuation_count_sys;

    // -------- Clock --------
    initial clk_sys = 1'b0;
    always #5 clk_sys = ~clk_sys;

    // -------- DUT instance --------
    tetra_ul_mac_access_parser #(.INFO_BITS(92)) dut (
        .clk_sys(clk_sys),
        .rst_n_sys(rst_n_sys),
        .info_bits_sys(info_bits_sys),
        .info_valid_sys(info_valid_sys),
        .crc_ok_sys(crc_ok_sys),
        .pdu_type_sys(pdu_type_sys),
        .fill_bit_sys(fill_bit_sys),
        .encryption_mode_sys(encryption_mode_sys),
        .ul_addr_type_sys(ul_addr_type_sys),
        .ul_issi_sys(ul_issi_sys),
        .ul_event_label_sys(ul_event_label_sys),
        .optional_field_flag_sys(optional_field_flag_sys),
        .ul_frag_flag_sys(ul_frag_flag_sys),
        .ul_reservation_req_sys(ul_reservation_req_sys),
        .ul_length_ind_sys(ul_length_ind_sys),
        .mm_pdu_type_sys(mm_pdu_type_sys),
        .loc_upd_type_sys(loc_upd_type_sys),
        .raw_info_bits_sys(raw_info_bits_sys),
        .pdu_valid_sys(pdu_valid_sys),
        .pdu_count_sys(pdu_count_sys),
        .bl_ack_valid_sys(bl_ack_valid_sys),
        .bl_ack_nr_sys(bl_ack_nr_sys),
        .bl_ack_count_sys(bl_ack_count_sys),
        .ul_llc_is_bl_data_sys(ul_llc_is_bl_data_sys),
        .ul_llc_is_bl_ack_sys(ul_llc_is_bl_ack_sys),
        .ul_llc_has_fcs_sys(ul_llc_has_fcs_sys),
        .ul_llc_ns_valid_sys(ul_llc_ns_valid_sys),
        .ul_llc_ns_sys(ul_llc_ns_sys),
        .ul_llc_nr_valid_sys(ul_llc_nr_valid_sys),
        .ul_llc_nr_sys(ul_llc_nr_sys),
        .ul_llc_is_mle_mm_sys(ul_llc_is_mle_mm_sys),
        .ul_llc_mm_pdu_type_sys(ul_llc_mm_pdu_type_sys),
        .ul_llc_mm_loc_upd_type_sys(ul_llc_mm_loc_upd_type_sys),
        .ul_llc_pdu_type_sys(ul_llc_pdu_type_sys),
        .ul_mle_disc_sys(ul_mle_disc_sys),
        .ul_pdu_is_continuation_sys(ul_pdu_is_continuation_sys),
        .ul_continuation_valid_sys(ul_continuation_valid_sys),
        .ul_continuation_bits_sys(ul_continuation_bits_sys),
        .ul_continuation_ssi_sys(ul_continuation_ssi_sys),
        .ul_continuation_count_sys(ul_continuation_count_sys)
    );

    // ---- Gold-Ref UL#0 hex `01 41 7F A7 01 12 66 34 20 C1 22 60` ----
    // info_bits_sys[0] = first on-air bit (MSB-first within hex bytes),
    // i.e. info_bits_sys[i] = bit i of the 92-bit MAC-ACCESS PDU.
    // Computed value (LSB at info_bits_sys[0]): 92'h64483042c664880e5fe8280
    localparam [91:0] GOLD_UL0 = 92'h64483042c664880e5fe8280;

    integer fail_count;

    task expect_eq_int;
        input [255:0] tag;
        input [31:0]  got;
        input [31:0]  want;
        begin
            if (got !== want) begin
                $display("FAIL [%0s]: got=%0d (0x%0h) want=%0d (0x%0h)",
                         tag, got, got, want, want);
                fail_count = fail_count + 1;
            end else begin
                $display("PASS [%0s]: %0d (0x%0h)", tag, got, got);
            end
        end
    endtask

    task expect_eq_bit;
        input [255:0] tag;
        input         got;
        input         want;
        begin
            if (got !== want) begin
                $display("FAIL [%0s]: got=%b want=%b", tag, got, want);
                fail_count = fail_count + 1;
            end else begin
                $display("PASS [%0s]: %b", tag, got);
            end
        end
    endtask

    initial begin
        fail_count     = 0;
        rst_n_sys      = 1'b0;
        info_bits_sys  = 92'd0;
        info_valid_sys = 1'b0;
        crc_ok_sys     = 1'b0;

        repeat (4) @(posedge clk_sys);
        rst_n_sys = 1'b1;
        repeat (2) @(posedge clk_sys);

        // ---- Drive Gold-Ref UL#0 ----
        @(posedge clk_sys);
        info_bits_sys  <= GOLD_UL0;
        info_valid_sys <= 1'b1;
        crc_ok_sys     <= 1'b1;
        @(posedge clk_sys);
        info_valid_sys <= 1'b0;
        crc_ok_sys     <= 1'b0;
        // Wait for the registered outputs to update (1 cycle after the input).
        @(posedge clk_sys);

        // ---- Per Gold-Ref reference_gold_attach_bitexact.md UL#0 ----
        expect_eq_bit("pdu_type=MAC-ACCESS",     pdu_type_sys,        1'b0);
        expect_eq_bit("fill_bit",                fill_bit_sys,        1'b0);
        expect_eq_bit("encryption_mode",         encryption_mode_sys, 1'b0);
        expect_eq_int("addr_type=00",            {30'd0, ul_addr_type_sys}, 32'd0);
        expect_eq_int("issi=0x282FF4",           {8'd0, ul_issi_sys}, 32'h00282FF4);
        expect_eq_bit("opt_field=1",             optional_field_flag_sys, 1'b1);
        expect_eq_bit("frag_flag=1",             ul_frag_flag_sys,    1'b1);
        expect_eq_int("reservation_req=0",       {28'd0, ul_reservation_req_sys}, 32'd0);
        // length_ind only meaningful when length_or_cap=0; here cap_req=1 so
        // ul_length_ind_sys is forced 0.
        expect_eq_int("length_ind=0 (cap_req branch)", {27'd0, ul_length_ind_sys}, 32'd0);
        // Continuation flag = 0 (this is MAC-ACCESS, not MAC-END-HU).
        expect_eq_bit("is_continuation=0",       ul_pdu_is_continuation_sys, 1'b0);
        expect_eq_bit("pdu_valid pulsed",        pdu_valid_sys,       1'b1);
        // -------- LLC layer at TL-SDU start = bit 36 --------
        // BL-DATA: link_type=0, has_fcs=0, bl_pdu_type=01.
        expect_eq_bit("LLC is_bl_data",          ul_llc_is_bl_data_sys, 1'b1);
        expect_eq_bit("LLC is_bl_ack=0",         ul_llc_is_bl_ack_sys,  1'b0);
        expect_eq_bit("LLC has_fcs=0",           ul_llc_has_fcs_sys,    1'b0);
        expect_eq_bit("LLC ns_valid=1",          ul_llc_ns_valid_sys,   1'b1);
        expect_eq_bit("LLC ns=0",                ul_llc_ns_sys,         1'b0);
        // MLE protocol discriminator = 001 (MM).
        expect_eq_int("MLE disc = 001 (MM)",
                      {29'd0, ul_mle_disc_sys}, 32'd1);
        expect_eq_bit("LLC is_mle_mm",           ul_llc_is_mle_mm_sys,  1'b1);
        // MM type = 0010 = 2 = U-LOCATION-UPDATE-DEMAND.
        expect_eq_int("MM pdu_type=2",
                      {28'd0, ul_llc_mm_pdu_type_sys}, 32'd2);
        // loc_upd_type at MM_body[0..2] = 011 = 3 (ITSI-Attach).
        expect_eq_int("loc_upd_type=3 (ITSI-Attach)",
                      {29'd0, ul_llc_mm_loc_upd_type_sys}, 32'd3);
        // raw_info_bits_sys preserved bit-exact.
        if (raw_info_bits_sys !== GOLD_UL0) begin
            $display("FAIL [raw_info_bits_sys]: got=92'h%023h want=92'h%023h",
                     raw_info_bits_sys, GOLD_UL0);
            fail_count = fail_count + 1;
        end else begin
            $display("PASS [raw_info_bits_sys]: 92-bit MSB-first preserved");
        end

        repeat (4) @(posedge clk_sys);

        if (fail_count == 0) begin
            $display("==================================================");
            $display("PASS: tb_mac_access_parser all field checks");
            $display("==================================================");
            $finish;
        end else begin
            $display("==================================================");
            $display("FAIL: tb_mac_access_parser %0d failures", fail_count);
            $display("==================================================");
            $stop;
        end
    end

    initial begin
        #100000;
        $display("FAIL: tb_mac_access_parser watchdog");
        $stop;
    end
endmodule

`default_nettype wire
