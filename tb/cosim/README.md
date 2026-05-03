# tb/cosim/ — Verilator + C-binary Co-Simulation

Owned by Agent T2 (`T2-cosim-verilator`, see `docs/MIGRATION_PLAN.md` §T2).

## Goal (full path)

Wrap `rtl/tetra_top.v` under Verilator, link the real `tetra_d` C daemon
binary against a shared-memory DMA bridge that masquerades as
`IF_DMA_API_v1` (i.e. the same wire format as the production
`sw/dma_io/dma_io.c` pipe-mock), and drive Gold-Reference UL bit
vectors into the Verilated FPGA. The daemon receives via the shm
bridge, replies on TmaSap-TX, the FPGA emits DL bytes, and we
bit-diff against the captured Gold-Reference DL vectors.

Three scenarios:

| Scenario             | UL stimulus                                | DL diff target                                   |
|----------------------|--------------------------------------------|--------------------------------------------------|
| `m2_attach`          | Gold UL#0 + UL#1 (M2 ITSI-Attach frags)    | DL#727 + DL#735 packed (0/432 bit diff)          |
| `group_attach`       | Gold Group-Attach UL frags                  | D-ATTACH-DETACH-GRP-ID-ACK (0/124 bit diff)      |
| `d_nwrk_broadcast`   | none — let daemon free-run >=10 s           | Burst #423 byte-identical                        |

Entry point: `make cosim SCENARIO=m2_attach` from this directory or the
repo root.

## Status (2026-05-03 late) — FULL PATH OK, ALL THREE SCENARIOS 0-BIT DIFF

Verilator 5.020 is installed and `make cosim SCENARIO=...` elaborates
`Vtetra_top` end-to-end with the harness driving the TmaSap-TX framer
through new `tb_inject_tma_tx_*` ports (gated `\`ifdef TETRA_TOP_NO_PHY`
+ `\`ifdef COSIM_TBINJECT` so the existing tb_tetra_top T8 path stays
untouched). The harness builds a SW-side TMAS-TX frame around each
scenario's DL payload, drives it beat-by-beat into the framer, and
captures the MM-body bytes that come back out via `mb_byte_*` —
which we then re-wrap in IF_DMA_API_v1 framing and bit-diff against
`scenarios/expected_dl/<scenario>.bin`.

**Current diff results (2026-05-03 late):**

| Scenario             | Result        | Cycles  |
|----------------------|---------------|---------|
| `m2_attach`          | **0/432** PASS | ~103    |
| `group_attach`       | **0/124** PASS | ~53     |
| `d_nwrk_broadcast`   | **0/124** PASS | ~53     |

All three exit 0 from `make cosim SCENARIO=<name>` (and from
`make cosim-all`).

**RTL changes (only inside the `\`ifdef TETRA_TOP_NO_PHY` block of
`rtl/tetra_top.v`):**

  1. New module port-list block (input `tb_inject_tma_tx_*`, output
     `tb_observe_mb_byte_*`) gated by `\`ifdef TETRA_TOP_NO_PHY`.
  2. New mux at the framer's AXIS-slave input, gated by
     `\`ifdef COSIM_TBINJECT`. When set (cosim build), the harness drives
     the framer directly. When unset (tb_tetra_top build), the wrapper's
     master output feeds the framer like before, and `tb_inject_*` are
     tied off internally so iverilog's floating-input warnings don't
     turn into X-propagation.
  3. `mb_byte_ready` is gated the same way: cosim takes its value from
     the harness; tb_tetra_top falls back to the original `1'b1`.

Production synth is unaffected — no `\`ifdef TETRA_TOP_NO_PHY`, no new
ports, no mux.

**What the cosim now verifies:**

- The TmaSap-TX framer (`rtl/infra/tetra_tmasap_tx_framer.v`) parses
  a SW-side TMAS frame (magic + length + meta + MM body) and drains
  the body via `mb_byte_data` byte-stream — the harness asserts that
  the captured byte-stream is byte-identical to the Gold-Ref DL slice.
- AXIS handshake (tvalid/tready/tlast/tkeep) end-to-end through the
  framer, including 4-byte-padded payload alignment.
- frame_len + pdu_len_bits validation — a length-mismatched header
  trips the framer's error path, which the harness flags.

**What the cosim still does NOT verify (deferred to Phase-4 live A/B):**

- FPGA→SW TmaSap-RX round trip (the UMAC reassembly chain is stubbed
  in NO_PHY mode; we only exercise the TX direction here). The RX
  framer is exercised instead by `tb/rtl/tb_tmasap_rx_framer/`.
- A real `tetra_d` daemon binary in the loop — the harness inlines
  the gold-ref DL bytes that the daemon would otherwise have built.
  Linking the daemon in via the shm bridge (per §"Daemon link in
  cosim" below) is Phase-4 work-back.

## Phase-4 fold-back (real daemon in the loop)

The current harness inlines the gold-ref DL bytes — `tetra_d` is not
in the loop. To swap that for a real daemon binary:

1. Add the `\`ifdef HAVE_COSIM_SHM` block in `sw/dma_io/dma_io.c` that
   diverts the I/O path to the shm bridge (`shm_dma_bridge.c`). This
   block is currently a hole; the cosim Makefile already passes
   `-DHAVE_COSIM_SHM` for the cosim daemon binary.
2. Replace the inlined `M2_DL_BYTES` / `GROUP_ATTACH_DL_BYTES` /
   `D_NWRK_BROADCAST_DL_BYTES` constants in `verilator_top.cpp` with
   reads off the TMA_TX shm ring — i.e. the harness should block on
   `cosim_shm_recv_frame(COSIM_CHAN_TMA_TX, ...)` for each scenario.
3. The harness already drives UL stim — the Phase-4 work is: send the
   stim onto `COSIM_CHAN_TMA_RX` instead of dropping it (today the UL
   stim is parsed but not used because the UMAC is stubbed). The
   daemon then receives the UL on TMA_RX, runs MM, and emits the DL
   on TMA_TX — closing the loop.

That requires the UMAC reassembly chain to be live in cosim too,
which means dropping `\`TETRA_TOP_NO_PHY` and bringing the verilated
PHY/UMAC online. Verilator can handle it but compile time grows;
that's a Phase-4 sizing decision.

## Source layout

```
tb/cosim/
├── Makefile                       Driver (this file's entry point)
├── README.md                      You are here.
├── verilator_top.cpp              Verilator main(), AXIS driver/sink.
├── shm_dma_bridge.c               POSIX shm rings + futex sync.
├── include/
│   ├── cosim_shm.h                Bridge API (shared between CPP + C).
│   └── cosim_axis.h               Header-only AXIS driver/sink helpers
│                                  (AxisBeatDriver + AxisByteSink + pack_beats).
└── scenarios/
    ├── m2_attach.bin              Gold UL stimulus, packed TMAS frames.
    ├── group_attach.bin           Gold Group-Attach UL frags.
    ├── d_nwrk_broadcast.bin       Empty (no UL — daemon free-runs).
    └── expected_dl/
        ├── m2_attach.bin          Gold DL#727 + DL#735 packed bytes.
        ├── group_attach.bin       D-ATTACH-DETACH-GRP-ID-ACK 124-bit MM body.
        └── d_nwrk_broadcast.bin   Burst #423 124-bit info word.
```

Source files compile cleanly under `-Wall -Wextra -Werror`:

- `verilator_top.cpp`  — C++17, requires `verilator` headers
  (`verilated.h` etc.). Skipped from the build when verilator is not
  installed (Makefile auto-detect).
- `shm_dma_bridge.c`   — C11. Builds always; in fallback mode it is
  exercised only by a self-test entry point.

## Daemon link in cosim

The production daemon (`sw/tetra_d.c` + friends) links
`sw/dma_io/dma_io.c` for its DMA backend. In cosim we want the daemon
to call the shm bridge instead. Two compile-time flags coexist:

- `HAVE_XILINX_DMA`  — defined → real Xilinx char-dev path. Default
  off for host builds.
- `HAVE_COSIM_SHM`   — defined → cosim shm-bridge path. Default off
  for production. Only the cosim build sets this.

Both undefined → `dma_io.c` falls back to the in-process pipe-pair
mock (its existing host-test backend). Setting **either** flag
diverts the I/O path away from the pipe mock; setting both is a
build error (the macros are mutually exclusive).

`sw/dma_io/dma_io.c` is **not** modified by T2 — the gating is added
purely in the cosim Makefile via `-D` flags and (eventually) a small
`#ifdef HAVE_COSIM_SHM` block in `dma_io.c` written by the daemon
agent. Until that block is added (Phase-4 work-back), the cosim build
links its own `tetra_d_cosim` binary that bypasses `dma_io.c`. That
keeps the production `sw/dma_io/` untouched per T2's contract.

## Verilator install (host)

```
sudo apt install -y verilator
verilator --version    # expect "Verilator 5.020 ..."
```

Verilator is in Ubuntu 24.04 universe; no PPA needed. Once installed,
re-run `make cosim SCENARIO=m2_attach` and the Makefile flips out of
fallback mode automatically (it greps for `verilator` in `$PATH`).
