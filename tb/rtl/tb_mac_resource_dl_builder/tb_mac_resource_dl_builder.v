// =============================================================================
// tb_mac_resource_dl_builder.v — Bit-exact testbench for
//                                tetra_mac_resource_dl_builder.v
// =============================================================================
// Verilog-2001 only.  Drives a Gold-Ref-DL#735-shaped header config:
//   addr_type=001 (SSI), SSI=0x282FF4, NR=0, NS=0, llc_pdu_type=BL-ADATA,
//   random_access_flag=0, slot_granting_flag=1 (slot=0x00), other flags=0,
//   80-bit deterministic MM body (1010…) — and asserts the resulting 268-bit
//   MAC-RESOURCE PDU is bit-for-bit identical to a reference vector computed
//   from the bluestation-aligned algorithm documented in the source comments
//   (also re-derived in scripts/ref_mac_resource_dl.py).
//
// Why this specific input: the actual Gold-Ref DL#735 has a 102-bit MM body
// whose layout is variable-encoded (p-bits + nested type-3/4 IEs).  A6's
// scope is to validate the wrapper layer, not the MM-body encoder (that's
// owned by S3).  Choosing 80 bits 1010… isolates the wrapper bit-packing —
// MAC header, LLC BL-ADATA wrap, MLE-PD prefix, pad+fill_bit_ind — from MM
// body encoding while keeping the surrounding bit positions identical to
// the Gold-Ref DL#735 layout (mac_hdr_bits=51, length_ind=18 octets,
// fill_bit_ind=1, RA=0, slot_grant_flag=1).
//
// Bit-exact gate: 0/268 bits diff.  >0 ⇒ FAIL.
// =============================================================================

`timescale 1ns / 1ps
`default_nettype none

module tb_mac_resource_dl_builder;
    reg         clk;
    reg         rst_n;
    reg         start;

    reg [23:0]  ssi;
    reg [2:0]   addr_type;
    reg         ns, nr;
    reg [3:0]   llc_pdu_type;
    reg         random_access_flag;
    reg         power_control_flag;
    reg [3:0]   power_control_element;
    reg         slot_granting_flag;
    reg [7:0]   slot_granting_element;
    reg         chan_alloc_flag;
    reg [31:0]  chan_alloc_element;
    reg [4:0]   chan_alloc_element_len;
    reg         second_pdu_valid;
    reg [5:0]   second_pdu_length_ind;
    reg         second_pdu_random_access_flag;
    reg [2:0]   second_pdu_addr_type;
    reg [23:0]  second_pdu_ssi;
    reg [79:0]  second_pdu_tl_sdu;
    reg [6:0]   second_pdu_tl_sdu_len;
    reg         second_pdu_pc_flag;
    reg [3:0]   second_pdu_pc_element;
    reg         second_pdu_sg_flag;
    reg [7:0]   second_pdu_sg_element;
    reg         second_pdu_ca_flag;
    reg [31:0]  second_pdu_ca_element;
    reg [4:0]   second_pdu_ca_element_len;
    reg [127:0] mm_pdu_bits;
    reg [7:0]   mm_pdu_len_bits;

    wire [267:0] pdu_bits;
    wire         valid;

    initial clk = 1'b0;
    always #5 clk = ~clk;

    tetra_mac_resource_dl_builder #(
        .PDU_BITS(268),
        .LLC_BUF_BITS(144)
    ) dut (
        .clk(clk), .rst_n(rst_n), .start(start),
        .ssi(ssi), .addr_type(addr_type),
        .ns(ns), .nr(nr),
        .llc_pdu_type(llc_pdu_type),
        .random_access_flag(random_access_flag),
        .power_control_flag(power_control_flag),
        .power_control_element(power_control_element),
        .slot_granting_flag(slot_granting_flag),
        .slot_granting_element(slot_granting_element),
        .chan_alloc_flag(chan_alloc_flag),
        .chan_alloc_element(chan_alloc_element),
        .chan_alloc_element_len(chan_alloc_element_len),
        .second_pdu_valid(second_pdu_valid),
        .second_pdu_length_ind(second_pdu_length_ind),
        .second_pdu_random_access_flag(second_pdu_random_access_flag),
        .second_pdu_addr_type(second_pdu_addr_type),
        .second_pdu_ssi(second_pdu_ssi),
        .second_pdu_tl_sdu(second_pdu_tl_sdu),
        .second_pdu_tl_sdu_len(second_pdu_tl_sdu_len),
        .second_pdu_pc_flag(second_pdu_pc_flag),
        .second_pdu_pc_element(second_pdu_pc_element),
        .second_pdu_sg_flag(second_pdu_sg_flag),
        .second_pdu_sg_element(second_pdu_sg_element),
        .second_pdu_ca_flag(second_pdu_ca_flag),
        .second_pdu_ca_element(second_pdu_ca_element),
        .second_pdu_ca_element_len(second_pdu_ca_element_len),
        .mm_pdu_bits(mm_pdu_bits),
        .mm_pdu_len_bits(mm_pdu_len_bits),
        .pdu_bits(pdu_bits),
        .valid(valid)
    );

    // ---- Reference vectors (computed by scripts/ref_mac_resource_dl.py) ----
    // Input: 80-bit MM body 1010… (mm_pdu_bits[127] = first bit = 1)
    //        addr_type=SSI, ssi=0x282FF4, ns=0, nr=0, BL-ADATA, ra=0,
    //        sg_flag=1 (sg_elem=0x00), pc/ca flags 0, no second PDU.
    // Expected pdu_bits (268-bit, bit[267]=first on-air):
    localparam [127:0] MM_PDU_BITS_TEST =
        128'haaaaaaaaaaaaaaaaaaaa000000000000;
    localparam [267:0] EXPECT_PDU =
        268'h2091282ff440001aaaaaaaaaaaaaaaaaaaa80000000000000000000000000000000;

    integer fail_count;
    integer diff_bits;
    integer i;
    integer waits;

    initial begin
        fail_count = 0;
        diff_bits  = 0;
        rst_n      = 1'b0;

        // Drive Gold-Ref-DL#735-shaped inputs
        start                  = 1'b0;
        ssi                    = 24'h282FF4;
        addr_type              = 3'b001;
        ns                     = 1'b0;
        nr                     = 1'b0;
        llc_pdu_type           = 4'd0;        // BL-ADATA
        random_access_flag     = 1'b0;
        power_control_flag     = 1'b0;
        power_control_element  = 4'd0;
        slot_granting_flag     = 1'b1;
        slot_granting_element  = 8'h00;
        chan_alloc_flag        = 1'b0;
        chan_alloc_element     = 32'd0;
        chan_alloc_element_len = 5'd0;
        second_pdu_valid       = 1'b0;
        second_pdu_length_ind  = 6'd0;
        second_pdu_random_access_flag = 1'b0;
        second_pdu_addr_type   = 3'd0;
        second_pdu_ssi         = 24'd0;
        second_pdu_tl_sdu      = 80'd0;
        second_pdu_tl_sdu_len  = 7'd0;
        second_pdu_pc_flag     = 1'b0;
        second_pdu_pc_element  = 4'd0;
        second_pdu_sg_flag     = 1'b0;
        second_pdu_sg_element  = 8'd0;
        second_pdu_ca_flag     = 1'b0;
        second_pdu_ca_element  = 32'd0;
        second_pdu_ca_element_len = 5'd0;
        mm_pdu_bits            = MM_PDU_BITS_TEST;
        mm_pdu_len_bits        = 8'd80;

        repeat (4) @(posedge clk);
        rst_n = 1'b1;
        repeat (2) @(posedge clk);

        // Pulse start.
        @(posedge clk);
        start <= 1'b1;
        @(posedge clk);
        start <= 1'b0;

        // Wait for valid (latency ~6 cycles).
        waits = 0;
        while (!valid && waits < 200) begin
            @(posedge clk);
            waits = waits + 1;
        end
        if (!valid) begin
            $display("FAIL: valid never asserted");
            fail_count = fail_count + 1;
        end else begin
            for (i = 0; i < 268; i = i + 1) begin
                if (pdu_bits[i] !== EXPECT_PDU[i])
                    diff_bits = diff_bits + 1;
            end
            if (diff_bits == 0) begin
                $display("PASS: pdu_bits 268/268 bit-exact");
            end else begin
                $display("FAIL: pdu_bits %0d/268 bit diff", diff_bits);
                $display("  got = 268'h%067h", pdu_bits);
                $display("  want= 268'h%067h", EXPECT_PDU);
                fail_count = fail_count + 1;
            end
        end

        repeat (4) @(posedge clk);

        if (fail_count == 0 && diff_bits == 0) begin
            $display("==================================================");
            $display("PASS: tb_mac_resource_dl_builder bit-exact");
            $display("==================================================");
            $finish;
        end else begin
            $display("==================================================");
            $display("FAIL: tb_mac_resource_dl_builder %0d failures, %0d bit diff",
                     fail_count, diff_bits);
            $display("==================================================");
            $stop;
        end
    end

    initial begin
        #200000;
        $display("FAIL: tb_mac_resource_dl_builder watchdog");
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
        $dumpvars(0, tb_mac_resource_dl_builder);
    end
`endif

endmodule

`default_nettype wire
