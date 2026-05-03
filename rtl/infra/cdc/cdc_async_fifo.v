// -----------------------------------------------------------------------------
// cdc_async_fifo.v
//
// Asynchronous FIFO for high-throughput data crossings between two clock
// domains. Standard textbook design (Cliff Cummings, "Simulation and Synthesis
// Techniques for Asynchronous FIFO Design", SNUG 2002):
//   - Dual-port BRAM-friendly memory
//   - Binary write/read pointers (DEPTH-LOG2 + 1 bit) used for memory address
//     (lower DEPTH-LOG2 bits) and full/empty detection (top bit included).
//   - Gray-coded copies of the binary pointers crossed via 2-flop synchronizers
//     into the opposite domain, then compared for full/empty in the local
//     pointer's domain.
//
// Use cases in tetra-bs:
//   - AXI-DMA RX byte-stream into LMAC at clk_sys (AD9361-aligned).
//   - AACH telemetry / status frames from LMAC (clk_sys) into AXI-Lite (clk_axi).
//   - Any AXIS-style burst that needs deterministic no-loss crossing.
//
// Parameters:
//   WIDTH : payload width (bits)
//   DEPTH : FIFO depth in entries — MUST be a power of two
//
// Throughput: one word per wr_clk cycle on the write side, one per rd_clk on
// the read side, gated by full/empty.
//
// Locked interface: IF_CDC_v1.
// -----------------------------------------------------------------------------

`default_nettype none

module cdc_async_fifo #(
    parameter WIDTH = 32,
    parameter DEPTH = 16        // power of 2
) (
    // ---- write side ----
    input  wire             wr_clk,
    input  wire [WIDTH-1:0] wr_data,
    input  wire             wr_en,
    output wire             wr_full,

    // ---- read side ----
    input  wire             rd_clk,
    output wire [WIDTH-1:0] rd_data,
    input  wire             rd_en,
    output wire             rd_empty
);

    // ---------------------------------------------------------------- ADDR_W --
    // log2(DEPTH). Verilog-2001-friendly compile-time constant function.
    function integer clog2;
        input integer value;
        integer       v;
        begin
            v = value - 1;
            for (clog2 = 0; v > 0; clog2 = clog2 + 1)
                v = v >> 1;
        end
    endfunction

    localparam integer ADDR_W = clog2(DEPTH);

    // -------------------------------------------------------------- Memory ----
    reg [WIDTH-1:0] mem [0:DEPTH-1];

    // -------------------------------------------------------- Write pointer ---
    reg  [ADDR_W:0] wr_ptr_bin;
    reg  [ADDR_W:0] wr_ptr_gray;
    wire [ADDR_W:0] wr_ptr_bin_next;
    wire [ADDR_W:0] wr_ptr_gray_next;

    // synchronized read pointer (Gray) into wr_clk domain
    (* ASYNC_REG = "TRUE" *) reg [ADDR_W:0] rd_ptr_gray_sync_meta;
    (* ASYNC_REG = "TRUE" *) reg [ADDR_W:0] rd_ptr_gray_sync;

    // --------------------------------------------------------- Read pointer ---
    reg  [ADDR_W:0] rd_ptr_bin;
    reg  [ADDR_W:0] rd_ptr_gray;
    wire [ADDR_W:0] rd_ptr_bin_next;
    wire [ADDR_W:0] rd_ptr_gray_next;

    // synchronized write pointer (Gray) into rd_clk domain
    (* ASYNC_REG = "TRUE" *) reg [ADDR_W:0] wr_ptr_gray_sync_meta;
    (* ASYNC_REG = "TRUE" *) reg [ADDR_W:0] wr_ptr_gray_sync;

    initial begin
        wr_ptr_bin            = {(ADDR_W+1){1'b0}};
        wr_ptr_gray           = {(ADDR_W+1){1'b0}};
        rd_ptr_bin            = {(ADDR_W+1){1'b0}};
        rd_ptr_gray           = {(ADDR_W+1){1'b0}};
        rd_ptr_gray_sync_meta = {(ADDR_W+1){1'b0}};
        rd_ptr_gray_sync      = {(ADDR_W+1){1'b0}};
        wr_ptr_gray_sync_meta = {(ADDR_W+1){1'b0}};
        wr_ptr_gray_sync      = {(ADDR_W+1){1'b0}};
    end

    // ===================== WRITE-SIDE LOGIC =====================
    // NOTE: *_next pointers are computed as unconditional +1 increments — they
    // describe "the pointer value if a write were to happen this cycle". Using
    // (wr_en && !wr_full) here would create a combinational loop, since
    // wr_full feeds back through wr_ptr_bin_next -> wr_ptr_gray_next -> wr_full.
    // The actual gating happens in the synchronous always block below (where
    // we write into mem only when wr_en && !wr_full).
    assign wr_ptr_bin_next  = wr_ptr_bin + 1'b1;
    assign wr_ptr_gray_next = (wr_ptr_bin_next >> 1) ^ wr_ptr_bin_next;

    // FULL: next-write Gray pointer equals synced read Gray pointer with the
    // top two bits inverted (standard Cummings full-detect formula).
    assign wr_full =
        (wr_ptr_gray_next ==
         {~rd_ptr_gray_sync[ADDR_W:ADDR_W-1], rd_ptr_gray_sync[ADDR_W-2:0]});

    always @(posedge wr_clk) begin
        if (wr_en && !wr_full) begin
            wr_ptr_bin  <= wr_ptr_bin_next;
            wr_ptr_gray <= wr_ptr_gray_next;
            mem[wr_ptr_bin[ADDR_W-1:0]] <= wr_data;
        end
        rd_ptr_gray_sync_meta <= rd_ptr_gray;
        rd_ptr_gray_sync      <= rd_ptr_gray_sync_meta;
    end

    // ===================== READ-SIDE LOGIC =====================
    // Same rationale as wr_ptr_bin_next: unconditional +1, gated in always.
    assign rd_ptr_bin_next  = rd_ptr_bin + 1'b1;
    assign rd_ptr_gray_next = (rd_ptr_bin_next >> 1) ^ rd_ptr_bin_next;

    // EMPTY: next-read Gray pointer equals synced write Gray pointer.
    // Use the *current* read Gray pointer for empty detection (Cummings: empty
    // when rd_ptr_gray == wr_ptr_gray_sync). Using rd_ptr_gray_next would
    // create the same comb loop pattern. After a read, rd_ptr_gray updates
    // synchronously and the next-cycle empty result is correct.
    assign rd_empty = (rd_ptr_gray == wr_ptr_gray_sync);

    // Combinational read (first-word-fall-through): rd_data is the memory word
    // currently pointed to by rd_ptr_bin. Consumer pops with rd_en when
    // !rd_empty.
    assign rd_data = mem[rd_ptr_bin[ADDR_W-1:0]];

    always @(posedge rd_clk) begin
        if (rd_en && !rd_empty) begin
            rd_ptr_bin  <= rd_ptr_bin_next;
            rd_ptr_gray <= rd_ptr_gray_next;
        end
        wr_ptr_gray_sync_meta <= wr_ptr_gray;
        wr_ptr_gray_sync      <= wr_ptr_gray_sync_meta;
    end

endmodule

`default_nettype wire
