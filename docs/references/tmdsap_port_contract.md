---
name: TmdSap framer port contract — IF_TMDSAP_FRAMER_v1
description: Bit-width + handshake-protocol contract of the A3 TmdSap RX/TX framers (rtl/infra/tetra_tmdsap_*_framer.v) plus the LMAC TCH/S placeholder bus that A5 must wire when the carry-over LMAC voice path lands. Output of A3 (FPGA TmdSap RX/TX framer). Locks 2026-05-03.
type: reference
---

# TmdSap framer port contract

This document is the formal A5-blocker output of agent **A3**
(`A3-fpga-tmdsap-framer`).  It enumerates the I/O signals — bit width,
direction, semantics, handshake protocol — of the two TmdSap framer RTL
blocks plus the placeholder LMAC-side TCH/S half-block bus that A5 must
wire when `tetra_top.v` instantiates the voice path.

The contract is named `IF_TMDSAP_FRAMER_v1` per the interface-locking
schedule in `docs/MIGRATION_PLAN.md` §"A. Interface-locking schedule"
entry #11.

**Source-of-truth:** the RTL files themselves are authoritative
(`rtl/infra/tetra_tmdsap_rx_framer.v`,
`rtl/infra/tetra_tmdsap_tx_framer.v`).  This doc is a derived snapshot
for cross-agent coordination; if a port name disagrees, the RTL wins
and this doc is wrong.

**Verilog-2001 only.**  Both modules compile clean under `iverilog
-g2001 -Wall -t null` standalone.

**A3 RTL files:**

| File | Module | Role |
|---|---|---|
| `rtl/infra/tetra_tmdsap_rx_framer.v` | `tetra_tmdsap_rx_framer` | LMAC channel-decode (432-bit NUB) → AXIS S2MM (FPGA→PS) |
| `rtl/infra/tetra_tmdsap_tx_framer.v` | `tetra_tmdsap_tx_framer` | AXIS MM2S (PS→FPGA) → LMAC channel-encode (432-bit NUB) |

---

## Common conventions

- **Bit ordering on `*_nub_bits[431:0]`.** MSB-first.  Bit `[431]` is the
  first on-air bit, matching the existing carry-over LMAC convention
  (`coded_bits[431]` in `rtl/lmac/tetra_sch_f_encoder.v`).  Bit `[0]` is
  the last on-air bit.
- **Resets.** Active-low synchronous-de-asserted reset (`rst_n`).
  Hold low for at least 4 cycles before driving inputs.
- **Clock.** Single clock per module (`clk`).  Wrapper-side wiring puts
  both framers on `clk_axi` (PS-side, ~100 MHz).  CDC to/from the
  LMAC-side `clk_sys` is the responsibility of A4 (`IF_CDC_AXIS_v1`).
- **AXIS handshake.** Matches A1 `IF_AXIDMA_v1` 1:1 — 32-bit `tdata`,
  big-endian-on-the-wire, MSB-lane first, `tlast` on the final beat,
  `tkeep` contiguous from MSB.  Sparse-keep is **not** generated and is
  treated as an error on input (TX framer).
- **Counters.** `tlm_*` outputs are 32-bit saturating counters; they
  never wrap.  Read by A5 from the AXI-Lite register window — see
  `docs/ARCHITECTURE.md` addendum below.

---

## 1. `tetra_tmdsap_rx_framer`

LMAC channel-decoder hands a fully-recovered 432-bit voice half-block
(NUB) on `in_nub_bits` with one-cycle `in_valid` → framer prepends the
TMDC magic + length-prefix and emits 16 AXIS beats to A1's
`s_axis_tmd_rx_*` slave port (FPGA→PS direction).

| Direction | Width | Name | Semantics |
|---|---|---|---|
| in  | 1   | `clk`             | system clock (clk_axi domain) |
| in  | 1   | `rst_n`           | async-asserted, sync-deasserted active-low reset |
| in  | 432 | `in_nub_bits`     | bit-transparent voice half-block, MSB-first; bit `[431]` = first on-air bit |
| in  | 1   | `in_valid`        | level-sensitive: HIGH → present a fresh NUB |
| out | 1   | `in_ready`        | level-sensitive: HIGH while framer is in S_IDLE; data is captured on the cycle (`in_valid & in_ready`) |
| out | 32  | `m_axis_tdata`    | AXIS-master tdata (MSB-lane = byte 0 on the wire) |
| out | 1   | `m_axis_tvalid`   | AXIS-master tvalid |
| in  | 1   | `m_axis_tready`   | AXIS-master tready |
| out | 1   | `m_axis_tlast`    | AXIS-master tlast (HIGH on beat 15 only) |
| out | 4   | `m_axis_tkeep`    | AXIS-master tkeep (4'b1111 on beats 0..14, 4'b1100 on beat 15) |
| out | 32  | `tlm_rx_frames`   | saturating 32-bit count of frames sent on AXIS-master |

**Per-frame layout on AXIS-master output** (16 beats, 62 bytes total):

```
beat[0]   bytes 0..3   = 0x544D_4443  ("TMDC")               tkeep=1111
beat[1]   bytes 4..7   = 0x0000_003E  (length = 62)           tkeep=1111
beat[2]   bytes 8..11  = payload[0..3]                         tkeep=1111
   ...
beat[14]  bytes 56..59 = payload[48..51]                       tkeep=1111
beat[15]  bytes 60..61 = payload[52..53] (upper 2 lanes only)  tkeep=1100, tlast=1
```

Payload bytes 0..53 are the 432-bit NUB packed MSB-first
(`payload[0][7]` = `in_nub_bits[431]` = first on-air bit;
`payload[53][0]` = `in_nub_bits[0]` = last on-air bit).

---

## 2. `tetra_tmdsap_tx_framer`

A1's MM2S master (`m_axis_tmd_tx_*`, PS→FPGA) presents a TMDC frame on
the AXIS-slave port → framer validates magic + length, accumulates the
54-byte payload, and emits the recovered 432-bit NUB on a single-cycle
parallel bus (`out_nub_bits` + `out_valid`/`out_ready`).

| Direction | Width | Name | Semantics |
|---|---|---|---|
| in  | 1   | `clk`             | system clock (clk_axi domain) |
| in  | 1   | `rst_n`           | active-low reset |
| in  | 32  | `s_axis_tdata`    | AXIS-slave tdata |
| in  | 1   | `s_axis_tvalid`   | AXIS-slave tvalid |
| out | 1   | `s_axis_tready`   | AXIS-slave tready |
| in  | 1   | `s_axis_tlast`    | AXIS-slave tlast (must be HIGH on beat 15 only) |
| in  | 4   | `s_axis_tkeep`    | AXIS-slave tkeep (4'b1111 on beats 0..14, 4'b1100 on beat 15) |
| out | 432 | `out_nub_bits`    | bit-transparent voice half-block, MSB-first, valid only on `out_valid==1` |
| out | 1   | `out_valid`       | level-sensitive: HIGH for ≥1 cycle when a fresh NUB is ready |
| in  | 1   | `out_ready`       | level-sensitive: must be HIGH for the framer to release the NUB and accept the next frame |
| out | 32  | `tlm_tx_frames`   | saturating 32-bit count of frames successfully decoded → handed off to LMAC |
| out | 32  | `tlm_err_count`   | saturating 32-bit count of bad-magic / bad-length / bad-tkeep / framing-error events |

**Error policy.** On any of:
- beat[0] `tdata != 0x544D_4443` (bad magic),
- beat[1] `tdata != 0x0000_003E` (bad length),
- any non-final beat with `tkeep != 4'b1111` (sparse-keep),
- premature `tlast` (before beat 15),
- final beat (beat 15) without `tlast=1` or `tkeep!=4'b1100`,

the framer increments `tlm_err_count`, drains remaining beats until
`tlast`, and returns to S_HDR_M.  No `out_valid` pulse is emitted
for a rejected frame.

---

## 3. LMAC TCH/S placeholder bus — TODO MARKER

```
<-- TODO: confirm LMAC TCH/S port shape with A5 when tetra_top.v wires it -->
```

The carry-over `rtl/lmac/` does **not** currently expose a TCH/S
half-block port — `docs/PROTOCOL.md` §"TCH/S" notes "RTL TBD when
CMCE-Voice-Path implemented".  A3 therefore exposes a clean placeholder
parallel bus on the LMAC-facing side of both framers:

| Module | Port | Width | Direction (vs framer) | Direction (vs LMAC) |
|---|---|---|---|---|
| `tetra_tmdsap_rx_framer` | `in_nub_bits[431:0]`  | 432 | input  | output (LMAC channel-decode result) |
| `tetra_tmdsap_rx_framer` | `in_valid`            | 1   | input  | output |
| `tetra_tmdsap_rx_framer` | `in_ready`            | 1   | output | input  |
| `tetra_tmdsap_tx_framer` | `out_nub_bits[431:0]` | 432 | output | input (LMAC channel-encode body) |
| `tetra_tmdsap_tx_framer` | `out_valid`           | 1   | output | input  |
| `tetra_tmdsap_tx_framer` | `out_ready`           | 1   | input  | output |

**Rationale for parallel-bus shape (not bit-serial):**

- Every existing LMAC encoder/decoder uses MSB-first parallel buses
  (`coded_bits[431:0]`, `pdu_bits[267:0]`, `info_bits[123:0]`).  A
  bit-serial interface here would duplicate what the burst muxer
  already does internally.
- 432 / 8 = 54 bytes exactly: byte-aligned packing on the AXIS side has
  zero pad bits at the byte boundary, and byte-aligned framing matches
  the A1 `tetra_dma_frame_packer` convention (8-byte self-describing
  header + payload bytes, MSB-lane first).
- One-shot handshake (single-cycle `valid & ready`) keeps the
  framer-side FSMs trivial: ≤200 LOC each, no FIFOs, no partial-word
  bookkeeping.  CDC to clk_sys is added by A4 if needed (the framers
  are clock-agnostic).

**When A5 wires `tetra_top.v`** with the eventual TCH/S decode/encode
chain, the connection is one-to-one; the framer ports above lock as
named.  No bit-layout change to the AXIS-side wire format is
permitted: that is `IF_TMDSAP_FRAMER_v1` and is already locked here
for downstream consumers (T1, T2).

---

## 4. Telemetry register addendum

Three 32-bit counters are surfaced from the framers up to the A5 AXI-
Lite register window in the `0x110..0x117` band (avoiding A1's
`0x100..0x10F` and the A2 TmaSap counters):

| Offset | Name | Width | Source | Reset | Description |
|---|---|---|---|---|---|
| `0x160` | `REG_TMDSAP_TX_FRAMES_CNT` | `[31:0]` | `tetra_tmdsap_tx_framer.tlm_tx_frames` | `0` | Saturating count of successfully decoded TMDC frames handed off to LMAC TX. |
| `0x164` | `REG_TMDSAP_RX_FRAMES_CNT` | `[31:0]` | `tetra_tmdsap_rx_framer.tlm_rx_frames` | `0` | Saturating count of TMDC frames emitted on AXIS-master to A1's S2MM port. |
| `0x168` | `REG_TMDSAP_ERR_CNT`       | `[31:0]` | `tetra_tmdsap_tx_framer.tlm_err_count` | `0` | Saturating count of frame-error events (bad magic, bad length, bad tkeep, premature/missing tlast). |

**Offset rationale.**  The original A3 task spec named the
`0x110..0x117` band, but `0x110/0x114/0x118` are already claimed by
carry-over registers (`REG_DMA_BLK_CNT`, `REG_CRC_ERR_CNT`,
`REG_SYNC_LST_CNT`) listed in `docs/ARCHITECTURE.md`.  The next free
band that is also explicitly reserved for "future per-layer counters"
is `0x160..0x1FF`, so A3 places the three TmdSap counters at
`0x160/0x164/0x168` — clear of A1 (`0x120..0x13F`), the AACH/UMAC
counters (`0x140..0x15F`), and any future A2 TmaSap counters which
will logically sit alongside the TmaSap channels in the
`0x140..0x15F` range or share the reserved `0x16C..0x1FF` band.

A5 wires these signals through the AXI-Lite read mux when the framers
are instantiated in `rtl/tetra_top.v`.  The same addendum is mirrored
in `docs/ARCHITECTURE.md` §"AXI-Lite Live-Config Register Window".

