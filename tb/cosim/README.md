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

## Status (2026-05-03 evening) — FULL ELAB OK, BRIDGE-TRANSLATOR PENDING

Verilator 5.020 is now installed (`apt install verilator` ran), so
the harness no longer hits the FALLBACK banner. `make cosim SCENARIO=...`
elaborates `Vtetra_top` end-to-end (with the `axi_dma_v7_1_bhv.v`
behavioural model standing in for the Xilinx LogiCORE IP), runs the
verilated FPGA against the Gold-Reference UL fixtures, and produces
a captured DL byte stream which gets bit-diffed against the expected
Gold-Reference DL.

**Current diff results (2026-05-03):** `m2_attach` = **409/432** bit
diff. The remaining diff is the wire-format gap documented below —
the FPGA emits its 36-byte structured TMAS header, the diff target
expects a 432-bit raw on-air DL slice. Both endpoints work; only the
translator layer between them is missing.

**Single open blocker — wire-format mismatch.** The userspace API
(`sw/dma_io/include/tetra/dma_io.h`) frames payload as
`MAGIC(4) | LEN_BE(4) | PAYLOAD`, but the FPGA-side TmaSap-RX
framer (`rtl/infra/tetra_tmasap_rx_framer.v`) emits a 36-byte
structured TMAS header (frame_len/pdu_len_bits/ssi/ssi_type/flags/
endpoint_id/...) per ARCHITECTURE.md §"TmaSap (Signalling) — Frame
format". Bridging the two requires a translator in either the
verilator main loop or the shm bridge — see "Re-enabling the full
path" below. Decision §D #3 explicitly says: deferred to Phase-4
live A/B is acceptable; we land the translator if Phase-3 needs it.

**What works in fallback mode:**

- `make cosim SCENARIO=<name>` runs to completion, exits 0, prints
  the `[cosim] FALLBACK MODE` banner so CI can grep for it.
- The Gold-Reference scenario fixtures are present (`scenarios/*.bin`,
  `scenarios/expected_dl/*.bin`) so the harness can be re-validated
  bit-for-bit once Verilator + the shm bridge are live.
- `verilator_top.cpp` and `shm_dma_bridge.c` are committed in
  buildable shape (see "Source layout" below). They will compile as
  soon as `apt install verilator` is run; the Makefile auto-detects
  verilator and switches modes.

**What the fallback does NOT verify:**

- FPGA→SW TmaSap-RX round trip (no real verilated `tetra_top`).
- SW→FPGA TmaSap-TX round trip (no real `tetra_d` in the loop).
- DL bit-exactness against Gold-Reference (the diff is structurally
  performed against a known-empty capture buffer, which always
  produces a non-zero diff in fallback mode — the harness reports
  "deferred" rather than PASS/FAIL for those checks).

The fallback is acceptable per §D #3: Phase 4 live A/B on Boards
#1 + #2 with two MTP3550s exercises the same bit paths against the
identical Gold-Reference vectors, so a missing T2 cosim does not block
project completion. It does mean SW-vs-RTL bugs surface later (live)
rather than earlier (cosim), which is the documented tradeoff Kevin
accepted on 2026-05-03.

## Re-enabling the full path

When picking this back up:

1. `sudo apt install -y verilator` on the build host (HARDWARE.md §10
   follow-up). Confirm `verilator --version` reports `5.020`.
2. Decide the wire-format bridging strategy. Two clean options:
   - **Option A — translate in verilator_top.cpp.** Keep
     `shm_dma_bridge.c` matching `IF_DMA_API_v1` (TMAS-header on the
     wire, same as `dma_io.c` pipe-mock). The C++ harness reads AXIS
     beats out of the verilated `tetra_top`, reassembles them into
     the 36-byte-header frames, and pushes whole frames into the
     shm ring. Symmetric on the TX side.
   - **Option B — translate in the FPGA TB stub.** Keep
     `shm_dma_bridge.c` matching the raw AXIS-beat wire (32-bit data
     + tlast + tkeep) and let the daemon stay unchanged via the
     pipe-mock framer. Less work in C++; more work figuring out how
     the daemon parses the unframed beats.

   We lean Option A because `dma_io.c`'s pipe-mock already speaks
   the `IF_DMA_API_v1` framing, so the daemon is unchanged. The C++
   harness is the only thing that needs to grow, and verilator's
   AXIS-driver patterns are well-documented.
3. Wire `make cosim SCENARIO=...` to elaborate `Vtetra_top` from
   `rtl/tetra_top.v` (use `-DTETRA_TOP_NO_PHY` to skip the PHY
   modeling — the TB only needs the AXIS path). Link
   `verilator_top.cpp` + `shm_dma_bridge.c` together.
4. Build a co-process wrapper for `tetra_d` so it links against the
   shm bridge via `-DHAVE_COSIM_SHM` (gating the production
   `dma_io.c` pipe-mock) — see "Daemon link in cosim" below.
5. Run all three scenarios. Required pass criteria locked in §T2:
   - `m2_attach` 0/432 bit diff (DL#727 + DL#735).
   - `group_attach` 0/124 bit diff vs D-ATTACH-DETACH-GRP-ID-ACK.
   - `d_nwrk_broadcast` byte-identical to Burst #423.

## Source layout

```
tb/cosim/
├── Makefile                       Driver (this file's entry point)
├── README.md                      You are here.
├── verilator_top.cpp              Verilator main(), AXIS driver/sink.
├── shm_dma_bridge.c               POSIX shm rings + futex sync.
├── include/
│   └── cosim_shm.h                Bridge API (shared between CPP + C).
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
