# tb/rtl/models/

Behavioural models of vendor IP cores for use in iverilog test benches.

These models are for **simulation only**. They mimic the externally observable
behaviour of vendor IP enough to exercise the surrounding RTL logic, but do
not substitute for vendor-IP simulation in production sign-off.

## `axi_dma_v7_1_bhv.v`

Behavioural model of one channel of Xilinx LogiCORE `axi_dma_v7_1` (the IP
that the `rtl/infra/tetra_axi_dma_wrapper.v` instantiates 4× in production).

### Why a model instead of the real IP

The real `axi_dma_v7_1` IP requires the Xilinx UNISIM / SIMPRIM libraries
which `iverilog` does not ship with. Vivado's xsim simulator can use them,
but `make tb` runs on plain `iverilog -g2001` to keep CI cheap and the build
host requirement-free (HARDWARE.md §6).

The model defines a Verilog module `axi_dma_channel_inst` with the same
port-list as the production-flow `axi_dma_channel_inst` shim (a thin wrapper
around the real `axi_dma_v7_1` produced by the Vivado-IP-Tcl in
`rtl/infra/ip/axi_dma_*.tcl`, deferred to Phase 4 synth — see
`docs/MIGRATION_PLAN.md` §A1).

### What it models

- AXIS-slave handshake (S2MM direction, FPGA→DDR write side):
  - Always asserts `tready` while not in reset.
  - Captures every accepted byte (per `tkeep`) into a 4 KiB ring buffer
    `s2mm_capture[]`, accessible by the TB via hierarchical reference for
    bit-exact verification of the framer output.
  - Pulses `irq_done` for one cycle on every `tlast`.
  - Increments `frame_count` per completed frame.

- AXIS-master generation (MM2S direction, DDR→FPGA read side):
  - Idle until the TB calls `inject_byte(...)` (one byte per call) and then
    `inject_frame_done()` to launch emission.
  - Drives a 32-bit AXIS stream with `tlast` on the final beat and `tkeep`
    set to the partial-trailing-bytes mask.
  - Pulses `irq_done` and increments `frame_count` per completed emission.

- AXI-MM master ports — **NOT modelled**. All `m_axi_*` are tied to constants
  (`m_axi_awvalid = 0`, `m_axi_arvalid = 0`, `m_axi_bready = 1`,
  `m_axi_rready = 1`, etc.). The TB does not need a DDR model because
  framer-level verification only inspects the AXIS path and the IRQ pulse.

- Scatter-gather descriptor ring — **NOT modelled**. The real IP fetches
  descriptors from DDR via its second AXI4-MM master and processes them
  asynchronously of the data path. For wrapper-level TBs we drive frames
  directly into the AXIS path; descriptor-ring behaviour is verified live
  on Board #1 in Phase 4 (CLAUDE.md §Test-Strategie #4).

- AXI-Lite control register window of the IP itself — **NOT modelled**.
  The wrapper exposes its own AXI-Lite sub-window for channel-enable /
  reset / IRQ-control, which IS modelled by the wrapper RTL itself
  (`rtl/infra/tetra_axi_dma_wrapper.v`). The IP's internal control regs
  (MM2S_DMACR, MM2S_DMASR, MM2S_SA, MM2S_LENGTH, etc., per Xilinx PG021)
  are programmed by `libaxidma` userspace lib in production; we trust
  Xilinx's IP for that and verify the framer end-to-end on Board #1.

### Production substitution

In the synth flow, the Vivado Block Design generator pulls the real
`axi_dma_v7_1` IP via `rtl/infra/ip/axi_dma_*.tcl` (Phase 4 deliverable
of A1) and the model file is **not** included. The wrapper file
`rtl/infra/tetra_axi_dma_wrapper.v` is identical between TB and synth —
only the included `axi_dma_channel_inst` definition differs.

### Limitations / caveats

- No simulation of AXI4-MM bursts, so the model does not exercise:
  - DDR-back-pressure stall behaviour
  - 4 KiB AXI4-boundary descriptor splits
  - AXI-protocol errors (BRESP/RRESP non-zero)
  - Out-of-order responses
- No simulation of overrun / underrun under realistic timing. The model
  drives `overrun_pulse = 0` and `underrun_pulse = 0` always; the wrapper
  TB injects synthetic overrun events via `force` if needed.
- No simulation of clock-domain crossings — the wrapper assumes a single
  AXI clock at this layer (the AXIS↔PHY-clk CDC is Agent A4's
  responsibility, see `docs/MIGRATION_PLAN.md` §A4).

These are all caught by Phase 4 live A/B testing on Board #1 vs. Board #2
sniffer (CLAUDE.md §Test-Strategie).

### Public TB-side API

```verilog
// Inject one frame, byte-by-byte, into a MM2S channel:
tb.dut.u_ch1_tma_tx.inject_reset();
for (i = 0; i < frame_len; i = i + 1)
    tb.dut.u_ch1_tma_tx.inject_byte(my_frame[i]);
tb.dut.u_ch1_tma_tx.inject_frame_done();
// → axi_dma_channel_inst will drive m_axis_* with the frame.

// Verify a captured S2MM frame, byte-by-byte:
if (tb.dut.u_ch0_tma_rx.s2mm_capture[0]   !== expected[0])  $fatal;
if (tb.dut.u_ch0_tma_rx.s2mm_capture_len !== expected_len) $fatal;
```

The TB owns the hierarchical paths above; if a future refactor renames
`u_ch0_tma_rx`, the TBs need updating in lockstep.
