# Architecture

## Layer Stack

```
┌─────────────────────────────────────────────────┐
│ MM / CMCE / SNDCP                  (sw/mm/, ...│  Application layers
├─────────────────────────────────────────────────┤
│ MLE                                (sw/mle/)   │  Mobility Link Entity
├─────────────────────────────────────────────────┤
│ LLC                                (sw/llc/)   │  Logical Link Control
├─────────────────────────────────────────────────┤
│  ━━━━━━━━━━ TmaSap / TmdSap ━━━━━━━━━━━━       │  ◄ FPGA↔SW boundary
│              4× AXI-DMA                         │
├─────────────────────────────────────────────────┤
│ UMAC                              (rtl/umac/)  │  Upper MAC
├─────────────────────────────────────────────────┤
│ LMAC                              (rtl/lmac/)  │  Lower MAC + Channel Coding
├─────────────────────────────────────────────────┤
│ PHY                               (rtl/phy/)   │  Symbol-level + SDR-IO
└─────────────────────────────────────────────────┘
```

Layer-Schnitt 1:1 wie bluestation. Verhalten 1:1 wie Gold-Reference.

## FPGA↔SW Boundary

### TmaSap (Signalling)

Carries assembled MAC-RESOURCE PDUs both directions. Variable-length PDU body
plus addressing + control metadata.

**Frame format (RX FPGA→SW):**

```
Offset  Bytes  Field
─────────────────────────────────────────────────────────
  0      4    magic = 0x544D_4153 ("TMAS")
  4      2    frame_len (total bytes incl. header)
  6      2    pdu_len_bits
  8      4    ssi (24-bit MSB-aligned)
 12      1    ssi_type (0=Unknown 1=Ssi 2=Issi 3=Gssi 4=Ussi 5=Smi 6=Esi 7=EventLabel)
 13      1    flags ([0]chan_change_resp [4]new_endpoint_present [5]css_endpoint_present)
 14      2    reserved
 16      4    endpoint_id
 20      4    new_endpoint_id (0 if flags[4]=0)
 24      4    css_endpoint_id (0 if flags[5]=0)
 28      4    scrambling_code
 32      4    reserved
 36      ?    pdu_bits (ceil(pdu_len_bits/8) bytes, MSB-aligned)
─────────────────────────────────────────────────────────
```

**Frame format (TX SW→FPGA):**

Same layout but with these meta-field changes:
- `flags`: `[0]stealing_perm [1]stealing_repeats [3]chan_alloc_present [6]chan_change_response_req`
- `chan_alloc` (new field at offset 14, replaces `reserved`): 12-bit packed CmceChanAllocReq
  - `[3:0]` timeslots-bitmap (slot 1..4)
  - `[4]` usage-valid
  - `[7:5]` usage-value
  - `[9:8]` alloc_type (0=Replace 1=Additional 2=QuitAndGo 3=ReplaceWithCarrierSig)
  - `[11:10]` ul_dl_assigned (0=Augmented 1=Dl 2=Ul 3=Both)
- `req_handle` (new field at offset 32): SW-assigned monotonic counter for TmaReportInd correlation

**Report frame (FPGA→SW status, on Signalling-RX channel):**

```
Offset  Bytes  Field
─────────────────────────────────────────────────────────
  0      4    magic = 0x544D_4152 ("TMAR")
  4      2    frame_len = 12
  6      2    reserved
  8      4    req_handle (echo of TX commit)
 12      1    report_code
              0=ConfirmHandle
              1=SuccessReservedOrStealing
              2=FailedTransfer
              3=FragmentationFailure
              4=SuccessRandomAccess (MS-only, BS won't emit)
              5=RandomAccessFailure (MS-only)
 13      3    pad
─────────────────────────────────────────────────────────
```

### TmdSap (Voice TCH)

Carries TCH/S ACELP frames per timeslot.

**Frame format (both directions):**

```
Offset  Bytes  Field
─────────────────────────────────────────────────────────
  0      4    magic = 0x544D_4443 ("TMDC")
  4      2    frame_len = 44
  6      1    timeslot (1..4)
  7      1    reserved
  8     35    acelp_data (274 bits MSB-aligned)
 43      1    pad
─────────────────────────────────────────────────────────
```

### DMA Channels (4 total)

| Channel | Direction | Frames | Worst-case rate |
|---------|-----------|--------|-----------------|
| 1 | FPGA → SW | TmaSap-RX (TMAS) + Report (TMAR) | ~100 kbps |
| 2 | SW → FPGA | TmaSap-TX (TMAS) | ~100 kbps |
| 3 | FPGA → SW | TmdSap-RX (TMDC) | ~80 kbps |
| 4 | SW → FPGA | TmdSap-TX (TMDC) | ~80 kbps |

Implementation: **Xilinx LogiCORE `axi_dma:7.1`**, Scatter-Gather mode, free in
Vivado Webpack license.

### Real-Time Constraint

bluestation `MACSCHED_TX_AHEAD = 1 timeslot` (= 3.54 ms). SW must commit
TmaSap-TX frame at least 1 timeslot before air slot start.

**Note:** This budget is from bluestation x86 reality. Cortex-A9 measured
budget TBD (TODO-D toolchain audit + Phase 4 live measurement).

## AACH (no SW path)

AACH is determined by FPGA-UMAC scheduler based on slot purpose:

| Slot purpose | AACH pattern (Gold-Ref) |
|---|---|
| MCCH idle | `0x0249` (DL=Common UL=Random f1=9 f2=9) |
| MCCH signalling-active (Pre-Reply, ACCEPT) | `0x0009` (DL=Unalloc UL=Unalloc f1=0 f2=9) |
| MCCH idle during DETACH-ACK | `0x0249` (stays idle) |
| MCCH idle during D-NWRK-BROADCAST | `0x0249` (stays idle) |
| Traffic slot default | `0x3000` (CapAlloc f1=0 f2=0) |
| Traffic slot during D-OTAR | `0x22C9`, `0x2049`, `0x304B`, `0x2249` (variants) |

Source: `docs/references/reference_gold_full_attach_timeline.md`.

## Message Bus (SW internal)

bluestation-style: central queue, `SapMsg{src, dest, sap, msg}`, priority levels:

| Priority | Use |
|---|---|
| Immediate | Time-critical reactions (e.g. AACH-aware Pre-Reply) |
| Normal | Standard L3 PDU processing |
| Low | Background tasks (TTL-Sweeper, persistence flushes) |

C implementation: linked-list queue with three priority buckets, single dispatch
loop in `tetra_d.c`. Workers receive `SapMsg` via callback registered per
(dest, sap) tuple.

## Subscriber-DB

- **Format:** JSON, single file `/var/lib/tetra/db.json`
- **Sections:** `entities[]`, `profiles[]`, (no `sessions[]` — AST not persisted on every change)
- **AST:** in-memory only. Snapshot to `/var/lib/tetra/ast.json` on `SIGTERM` only. Reload at start only if `clean_shutdown_flag = true` (separate marker file)
- **WebUI access:** via Unix-Socket to daemon (`/run/tetra_mle.sock`). Daemon validates and authoritatively writes. CGI is socket client.
- **Profile 0:** read-only invariant `0x0000_088F` (M2 GILA-Guard). WebUI hides edit. Daemon refuses delete + corrects on load if missing.

## AXI-Lite Live-Config Register Window

The PS controls the FPGA via a single AXI4-Lite slave at base address
`0x4000_0000`. The window is owned by Agent A5 (`A5-fpga-top-xdc-cleanup`) and
implemented in `rtl/infra/tetra_axi_lite_regs.v` — the same module name as the
carry-over register bank in `tetra-zynq-phy/rtl/infra/`, kept verbatim so that
build scripts and Vivado-BD `X_INTERFACE_INFO` annotations carry over without
edits. Behaviour is rewritten from scratch (the carry-over only had a flat
16-register PHY-status bank); the new bank is the **single live-config surface
for PHY + LMAC + UMAC + DMA**.

### Address-decode rule

The 12-bit AXI-Lite address (`s_axi_awaddr[11:0]` / `s_axi_araddr[11:0]`)
splits into four regions. Bits `[1:0]` are word-aligned (always 0 for legal
accesses; misaligned access returns `SLVERR`). Reads to undefined addresses
return `0x0000_0000`; writes are silently dropped. No protection-level check
(`s_axi_awprot`/`arprot` are accepted and ignored, matching carry-over R2 rule).

| Range | Region | R/W | Owner |
|---|---|---|---|
| `0x000..0x0FF` | **Configuration** (cell ID, RF trim, cipher, scrambler, training, slot table) | R/W (mostly) | daemon `tetra_d` writes via `apply.cgi` |
| `0x100..0x1FF` | **Telemetry / Stats** (read-only) | R/O | daemon polls or read-on-IRQ |
| `0x200..0x2FF` | **Test / Scratch** (TB hooks, scratch register, loopback control) | R/W | RTL TBs only |
| `0x300+`        | **Reserved** for future expansion | — | unused, returns 0 |

### Reset defaults

Where a register has a Gold-Reference-derived default, the citation is
`reference_gold_full_attach_timeline.md` "Konstanten" section
(MCC=262, MNC=1010, CC=1, scrambCode=`0x4183_F207`, DL=392.9875 MHz).

**Carrier frequency caveat — two distinct values across captures:**

| Reference memo | DL frequency |
|---|---|
| `reference_gold_full_attach_timeline.md` "Konstanten" — full M2 attach + DETACH + heartbeat | `392_987_500` Hz (= 392.9875 MHz) |
| `reference_gold_attach_bitexact.md` description header — externe BS @ 392.9875 MHz | `392_987_500` Hz (consistent with full-attach) |
| `reference_group_attach_bitexact.md` capture filename `DL_baseband_392984000Hz_*.wav` (memo description text says "@ 392.9875 MHz" but the capture file is at the slightly different LO) | `392_984_000` Hz |

`REG_RX_CARRIER_HZ` resets to **`392_987_500`** (matches the
authoritative full-attach Gold-Cell memo, used for M2 acceptance gates).
`REG_TX_CARRIER_HZ` resets to **`382_891_062`** (UL of the same Gold-Cell,
filename `UL_baseband_382891062Hz_*.wav` consistent across both
group-attach and full-attach captures — no caveat).

The 3.5 kHz delta between the two DL captures is below the AD9361 LO step
size for casual operator use; operators tune per-cell at runtime via a
write to `REG_RX_CARRIER_HZ`. WebUI exposes this through
`config.cgi?op=put&group=rf` (see `OPERATIONS.md` §4). If a third distinct
carrier value appears in any future Gold capture, **stop and flag for
operator decision** before changing the reset (per CLAUDE.md
"No-Go Patterns" — no invented numbers).

### Carry-over registers preserved

The carry-over `tetra_axi_lite_regs.v` exposed 16 registers at offsets
`0x00..0x3C`. The new layout retains the **PHY status / IRQ / counter**
half of that bank as read-only telemetry in the `0x100..0x1FF` region (one
new offset each), and absorbs the **R/W control** half into the new
config region with renamed offsets. Names that describe the same physical
register were renamed where the new spec gives a clearer, layer-aware
identifier:

| Carry-over name (`tetra-zynq-phy`) | New name | New offset | Reason for rename / move |
|---|---|---|---|
| `REG_CTRL` (0x00, RX_EN/TX_EN/LOOPBACK/RST_CNTRS) | `REG_CTRL` | `0x080` | semantically same, moved out of `0x00` so cell-identity sits at the top of the window. Bit-layout preserved. |
| `REG_STATUS` (0x04, sync_locked / pll_locked / fifo / slot_status) | `REG_PHY_STATUS` | `0x100` | renamed to disambiguate from per-layer status; moved into telemetry region as read-only. |
| `REG_VERSION` (0x08, `0x0001_0000`) | `REG_VERSION` | `0x0FC` | same constant, reset bumped to `0x0002_0000` on first new bitstream so SW can detect "new register window vs carry-over". Lives at top of config region as a config-region-trailer. |
| `REG_SYNC_THRESH` (0x0C) | `REG_SYNC_THRESH` | `0x084` | preserved, moved into config region. |
| `REG_COLOUR_CODE` (0x10) | **`REG_CELL_CC`** | `0x008` | **renamed (collision with baseline)** — same 6-bit register, baseline calls it `REG_CELL_CC` (Cell Colour Code). Default 1 unchanged. |
| `REG_FRAME_NUM` (0x14, RO frame counter) | `REG_FRAME_NUM` | `0x104` | preserved, moved into telemetry. |
| `REG_SLOT_NUM` (0x18, RO slot counter) | `REG_SLOT_NUM` | `0x108` | preserved, moved into telemetry. |
| `REG_RX_GAIN` (0x1C) | `REG_RX_GAIN` | `0x088` | preserved, moved into config. |
| `REG_TX_ATT` (0x20) | `REG_TX_ATT` | `0x08C` | preserved (separate from the new signed-dB `REG_TX_GAIN_TRIM` — see note below). |
| `REG_IRQ_ENABLE` (0x24) | `REG_IRQ_ENABLE` | `0x090` | preserved, moved into config (it is R/W). |
| `REG_IRQ_STATUS` (0x28, R/W1C) | `REG_IRQ_STATUS` | `0x10C` | preserved, moved into telemetry; W1C semantics retained (the one R/W exception in the telemetry region — write `1` to a bit clears it; hardware-set wins). |
| `REG_DMA_BLK_CNT` (0x2C) | `REG_DMA_BLK_CNT` | `0x110` | preserved, moved into telemetry. Joined by per-channel counters (`0x120..0x12F`). |
| `REG_CRC_ERR_CNT` (0x30) | `REG_CRC_ERR_CNT` | `0x114` | preserved, moved into telemetry. |
| `REG_SYNC_LST_CNT` (0x34) | `REG_SYNC_LST_CNT` | `0x118` | preserved, moved into telemetry. |
| `REG_RESERVED` (0x38) | — | — | dropped; new reserved space is the unmapped `0x300+` region. |
| `REG_SCRATCH` (0x3C, R/W byte-laned 32-bit) | `REG_SCRATCH` | `0x200` | preserved, moved into the test/scratch region. Byte-lane strobes still honoured. |

**Note on TX gain.** Carry-over `REG_TX_ATT` (8-bit unsigned, AD9361
attenuator step in 0.25 dB units) is kept verbatim because the AD9361
driver path needs it. The new baseline introduces an additional 8-bit
**signed** `REG_TX_GAIN_TRIM` for SW-side fine-trim above the AD9361
attenuator. Both registers exist; daemon writes both when an operator
adjusts TX power. WebUI shows the sum.

**Carry-over registers explicitly NOT carried (per `MIGRATION_PLAN.md`
"FPGA modules to delete from carry-over"):**

- `REG_SHADOW_*`, `REG_PROFILE_*`, `REG_DB_POLICY` — Subscriber-DB moves
  to SW (`sw/persistence/db.c`).
- `REG_AACH_GRANT_HINT` — UMAC scheduler determines AACH internally; no
  SW-override path (CLAUDE.md "No-Go Patterns").

These names are **forbidden** in the new register decoder and the address
decode for them returns 0 on read / drops on write.

### Configuration region (`0x000..0x0FF`) — full layout

All R/W unless marked otherwise. Width column is the meaningful bit
count; upper bits read as 0 and are ignored on write. Reset defaults are
rounded values from `reference_gold_full_attach_timeline.md` Konstanten
unless otherwise noted.

| Offset | Name | Width | R/W | Reset | Description |
|---|---|---|---|---|---|
| `0x000` | `REG_CELL_MCC` | `[9:0]` | R/W | `262` (`0x106`) | Mobile Country Code, ETSI EN 300 392-2 §16.10.42.1. Gold-Cell value. |
| `0x004` | `REG_CELL_MNC` | `[13:0]` | R/W | `1010` (`0x3F2`) | Mobile Network Code. Gold-Cell value. |
| `0x008` | `REG_CELL_CC` | `[5:0]` | R/W | `1` | Cell Colour Code, ETSI §9.2.6. Drives scrambling. **Renamed from carry-over `REG_COLOUR_CODE`.** |
| `0x00C` | `REG_CELL_LA` | `[13:0]` | R/W | `0` | Location Area. ETSI §16.10.42.7. Operator-assigned, no Gold default mandatory. |
| `0x010` | `REG_RX_CARRIER_HZ` | `[31:0]` | R/W | `392_987_500` | DL receive carrier frequency in Hz. See "Carrier frequency caveat" above. |
| `0x014` | `REG_TX_CARRIER_HZ` | `[31:0]` | R/W | `382_891_062` | UL transmit carrier frequency in Hz. Drives AD9361 LO via libiio shadowing. |
| `0x018` | `REG_TX_GAIN_TRIM` | `[7:0]` (signed) | R/W | `0` (= 0 dB) | Signed dB trim added on top of AD9361 `REG_TX_ATT`. Range −128..+127 dB. |
| `0x01C` | `REG_CIPHER_MODE` | `[1:0]` | R/W | `0` | 0=Clear, 1=SCK, 2=DCK, 3=ESI. Gold-Cell runs in `0` (Clear). |
| `0x020` | `REG_SCRAMBLER_INIT` | `[31:0]` | R/W | `0x4183_F207` | Scrambler seed, derived from MCC+MNC+CC. Gold-Cell value cited in `reference_gold_full_attach_timeline.md` Konstanten. |
| `0x024` | `REG_TS_N` | `[11:0]` | R/W | `0xCB2` | Normal Training Sequence (12 bit). ETSI §9.4.4.3.2 Table 9.7. |
| `0x028` | `REG_TS_P` | `[11:0]` | R/W | `0x536` | Pilot Training Sequence (Sync, 12 bit). ETSI §9.4.4.3.2 Table 9.7. |
| `0x02C` | `REG_TS_Q` | `[11:0]` | R/W | `0x0E2` | Extended Training Sequence (12 bit). ETSI §9.4.4.3.2 Table 9.7. |
| `0x030..0x07F` | `SLOT_TABLE` | 80 B | R/W | per slot (see below) | 20-entry slot config — see "SLOT_TABLE" sub-chapter. |
| `0x080` | `REG_CTRL` | `[3:0]` | R/W | `0` | `[0]` RX_EN, `[1]` TX_EN, `[2]` LOOPBACK, `[3]` RST_CNTRS (write-1-pulse self-clears). Carry-over from `REG_CTRL@0x00`. |
| `0x084` | `REG_SYNC_THRESH` | `[7:0]` | R/W | `0xC8` (200) | Burst-detector correlator threshold (carry-over). |
| `0x088` | `REG_RX_GAIN` | `[6:0]` | R/W | `0x20` (32) | AD9361 RX gain (carry-over). |
| `0x08C` | `REG_TX_ATT` | `[7:0]` | R/W | `0x28` (40) | AD9361 TX attenuator, 0.25 dB units (carry-over). |
| `0x090` | `REG_IRQ_ENABLE` | `[4:0]` | R/W | `0` | IRQ-enable mask, bit-aligned with `REG_IRQ_STATUS@0x10C`. |
| `0x0A0` | `REG_DMA_CH_ENABLE`  | `[3:0]` | R/W   | `0` | Per-channel enable, owned by Agent A1 (`tetra_axi_dma_wrapper.v`). `[0]` tma_rx, `[1]` tma_tx, `[2]` tmd_rx, `[3]` tmd_tx. While 0, the corresponding `axi_dma_v7_1` instance is held in reset and ignores all AXIS / AXI-MM activity. |
| `0x0A4` | `REG_DMA_CH_RESET`   | `[3:0]` | R/W   | `0` | Per-channel write-1-pulse soft reset (1 cycle), self-clearing. Bit positions match `REG_DMA_CH_ENABLE`. Used by daemon to recover from `REG_DMA_OVERRUN_CNT` increments. |
| `0x0A8` | `REG_DMA_IRQ_ENABLE` | `[3:0]` | R/W   | `0` | Per-channel IRQ-output enable mask. The status is latched regardless of this mask; only the output line `irq_*_o` is gated. |
| `0x0AC` | `REG_DMA_IRQ_STATUS` | `[3:0]` | R/W1C | `0` | Per-channel sticky completion status. HW-set wins over SW-clear when both happen on the same cycle (matches `REG_IRQ_STATUS@0x10C` semantics). |
| `0x094..0x09F`, `0x0B0..0x0FB` | reserved (config) | — | R/O | `0` | reads as 0; writes dropped. |
| `0x0FC` | `REG_VERSION` | `[31:0]` | R/O | `0x0002_0000` | Bitstream version. Bumped from carry-over `0x0001_0000` so SW can distinguish the new register window unambiguously. |

#### SLOT_TABLE window (`0x030..0x07F`)

20 entries × 4 bytes per entry. Each entry is a packed 32-bit slot
descriptor. The window covers 5 frames × 4 slots = 20 slot positions
(one TDMA multiframe is 18 frames, but the scheduler only needs the
upcoming-5-frames lookahead — index `i` = `(frame_in_lookahead × 4) +
slot_in_frame_minus_1`). Daemon writes the 20-entry array as a single
strided burst; FPGA-UMAC reads asynchronously with a 1-cycle handshake.

```
Bit-layout per 32-bit slot descriptor:
  [1:0]   slot_type     0=RA           (Random-Access uplink slot)
                        1=Common       (MCCH common downlink)
                        2=Unalloc      (idle / fallback)
                        3=Allocated    (TCH / dedicated signalling)
  [25:2]  assigned_ssi  24-bit SSI (only meaningful when slot_type=3 Allocated;
                        zero otherwise)
  [29:26] aach_hint     4-bit hint into the AACH-Modes table — UMAC encoder
                        clamps invalid combinations to default-for-slot_type.
                        Encoded values (mapping to raw AACH per
                        reference_gold_full_attach_timeline.md "AACH-Modes"):
                          0 = AACH_DEFAULT_FOR_SLOT_TYPE   (UMAC picks)
                          1 = AACH_RAW_0x0249              (MCCH idle / DETACH-ACK / D-NWRK-BCAST)
                          2 = AACH_RAW_0x0009              (MCCH signalling-active: Pre-Reply, ACCEPT, Group-Attach Reply)
                          3 = AACH_RAW_0x3000              (Traffic CapAlloc f1=0 f2=0)
                          4 = AACH_RAW_0x22C9              (D-OTAR variant)
                          5 = AACH_RAW_0x2049              (D-OTAR variant)
                          6 = AACH_RAW_0x304B              (D-OTAR variant)
                          7 = AACH_RAW_0x2249              (D-OTAR variant)
                         8..15 = reserved (UMAC clamps to 0)
  [31:30] reserved     reads as 0, must be written 0
```

`aach_hint` is **never** an override — per CLAUDE.md "No SW-Override-Pfade
für AACH": the SW hint may **only** select among the 8 patterns the UMAC
scheduler also considers valid for the current slot context. If the hint
contradicts UMAC's own state machine (e.g. SW asks for `0x0009` while
UMAC has nothing in the DL signal queue), UMAC ignores the hint and
emits its own pattern.

**Reset defaults (slot table):** entries 0..19 reset to
`slot_type=Unalloc`, `assigned_ssi=0`, `aach_hint=0` (= UMAC default).
Daemon writes the real 20-entry layout during boot from
`config.json`'s `slot_table` group.

### Telemetry region (`0x100..0x1FF`) — read-only

All read-only unless noted. Counters are 32-bit free-running with
saturation at `0xFFFF_FFFF`; `REG_CTRL[3]` (RST_CNTRS) clears them as a
1-cycle pulse. AACH transition counters are sourced from
`rtl/lmac/tetra_aach_encoder.v`; per-DMA-channel counters are sourced
from the planned A1 AXI-DMA wrappers (`rtl/infra/tetra_axi_dma_wrapper.v`,
agent A1 deliverable).

| Offset | Name | Width | Reset | Source RTL | Description |
|---|---|---|---|---|---|
| `0x100` | `REG_PHY_STATUS` | `[7:0]` | `0` | rx-frontend, frame counter | `[0]` SYNC_LOCKED, `[1]` PLL_LOCKED, `[2]` FIFO_EMPTY, `[3]` FIFO_FULL, `[7:4]` SLOT_STATUS bitmap. Carry-over of `REG_STATUS@0x04`. |
| `0x104` | `REG_FRAME_NUM` | `[4:0]` | live | `tetra_frame_counter.v` | Current frame number 1..18 within multiframe. |
| `0x108` | `REG_SLOT_NUM` | `[1:0]` | live | `tetra_frame_counter.v` | Current slot 0..3. |
| `0x10C` | `REG_IRQ_STATUS` | `[4:0]` | `0` | irq aggregator | R/W1C: `[0]` MAC_BLOCK_RDY, `[1]` SYNC_ACQUIRED, `[2]` SYNC_LOST, `[3]` CRC_ERROR, `[4]` RX_FIFO_FULL. Hardware-set wins over SW-clear. |
| `0x110` | `REG_DMA_BLK_CNT` | `[15:0]` | `0` | DMA bridge | Total DMA blocks transferred (carry-over). |
| `0x114` | `REG_CRC_ERR_CNT` | `[15:0]` | `0` | DMA bridge | CRC failures on RX (carry-over). |
| `0x118` | `REG_SYNC_LST_CNT` | `[15:0]` | `0` | sync detector | Sync-loss events (carry-over). |
| `0x11C` | `REG_FRAME_TICK_CNT` | `[31:0]` | `0` | `tetra_frame_counter.v` | Free-running multiframe counter (clears on RST_CNTRS). |
| `0x120` | `REG_DMA_TMA_RX_FRAMES` | `[31:0]` | `0` | A1 wrapper, ch1 (TmaSap RX) | Frame count delivered to PS over TmaSap-RX channel. |
| `0x124` | `REG_DMA_TMA_TX_FRAMES` | `[31:0]` | `0` | A1 wrapper, ch2 | TmaSap TX frames consumed from PS. |
| `0x128` | `REG_DMA_TMD_RX_FRAMES` | `[31:0]` | `0` | A1 wrapper, ch3 | TmdSap RX (voice) frames. |
| `0x12C` | `REG_DMA_TMD_TX_FRAMES` | `[31:0]` | `0` | A1 wrapper, ch4 | TmdSap TX frames. |
| `0x130` | `REG_DMA_IRQ_CNT_RX` | `[31:0]` | `0` | A1 wrapper | OR-reduce of S2MM IRQs across 4 channels (per-channel detail via `/proc/interrupts`). |
| `0x134` | `REG_DMA_IRQ_CNT_TX` | `[31:0]` | `0` | A1 wrapper | OR-reduce of MM2S IRQs. |
| `0x138` | `REG_DMA_OVERRUN_CNT` | `[15:0]` | `0` | A1 wrapper | Descriptor-ring overrun events (S2MM dropped a frame because PS didn't refill descriptors fast enough). |
| `0x13C` | `REG_DMA_UNDERRUN_CNT` | `[15:0]` | `0` | A1 wrapper | MM2S underrun (TX path starved while UMAC expected a frame). |
| `0x140` | `REG_AACH_LAST_RAW` | `[15:0]` | `0` | `tetra_aach_encoder.v` | Most recently emitted AACH 16-bit word. SW reads after each frame tick to log the timeline. |
| `0x144` | `REG_AACH_TRANSITION_CNT` | `[31:0]` | `0` | `tetra_aach_encoder.v` | Number of AACH-pattern changes (counts an event when consecutive emitted AACH words differ). |
| `0x148` | `REG_AACH_IDLE_CNT` | `[31:0]` | `0` | `tetra_aach_encoder.v` | Number of frames AACH emitted `0x0249` (MCCH idle / DETACH-ACK / D-NWRK-BCAST). |
| `0x14C` | `REG_AACH_SIG_ACTIVE_CNT` | `[31:0]` | `0` | `tetra_aach_encoder.v` | Number of frames AACH emitted `0x0009` (MCCH signalling-active). |
| `0x150` | `REG_AACH_TRAFFIC_CNT` | `[31:0]` | `0` | `tetra_aach_encoder.v` | Number of frames AACH emitted `0x3000` (CapAlloc traffic-default). |
| `0x154` | `REG_UMAC_DLQ_DEPTH` | `[7:0]` | `0` | `tetra_dl_signal_queue.v` | Current DL signal queue depth (pending TX PDUs). |
| `0x158` | `REG_UMAC_DLQ_DROPS` | `[15:0]` | `0` | `tetra_dl_signal_queue.v` | DL queue overflow drops. |
| `0x15C` | `REG_UMAC_REASM_FAIL_CNT` | `[15:0]` | `0` | `tetra_ul_demand_reassembly.v` | Frag-1+Frag-2 reassembly failures (mismatched LI / SSI / timeout). |
| `0x160` | `REG_TMASAP_RX_FRAMES_CNT` | `[31:0]` | `0` | `tetra_tmasap_rx_framer.v` (A2) | TMAS frames emitted on the FPGA→PS signalling-RX channel. |
| `0x164` | `REG_TMASAP_TX_FRAMES_CNT` | `[31:0]` | `0` | `tetra_tmasap_tx_framer.v` (A2) | TMAS frames consumed from the PS→FPGA signalling-TX channel and committed to the UMAC DL signal queue. |
| `0x168` | `REG_TMASAP_TX_ERR_CNT` | `[15:0]` | `0` | `tetra_tmasap_tx_framer.v` (A2) | TX framer drops: bad magic word, frame_len/pdu_len_bits mismatch, or premature tlast. Saturates at `0xFFFF`. |
| `0x16C` | `REG_TMAR_FRAMES_CNT` | `[31:0]` | `0` | `tetra_tmasap_rx_framer.v` (A2) | TMAR (`0x544D_4152`) report frames emitted on the FPGA→PS signalling-RX channel (shared AXIS-out with TMAS). |
| `0x170` | `REG_TMDSAP_TX_FRAMES_CNT` | `[31:0]` | `0` | `tetra_tmdsap_tx_framer.v` (A3) | Saturating count of successfully decoded TMDC frames handed off to LMAC TX (PS→FPGA voice path). See `docs/references/tmdsap_port_contract.md`. |
| `0x174` | `REG_TMDSAP_RX_FRAMES_CNT` | `[31:0]` | `0` | `tetra_tmdsap_rx_framer.v` (A3) | Saturating count of TMDC frames emitted on AXIS-master to A1's S2MM port (FPGA→PS voice path). |
| `0x178` | `REG_TMDSAP_ERR_CNT` | `[31:0]` | `0` | `tetra_tmdsap_tx_framer.v` (A3) | Saturating count of TMDC framing errors (bad magic, bad length, bad tkeep, premature/missing tlast). |
| `0x17C..0x1FF` | reserved (telemetry) | — | `0` | — | reads as 0; allows future per-layer counters without bumping `REG_VERSION`. |

### Test / Scratch region (`0x200..0x2FF`)

Operator-disabled by default. Reads always allowed; writes only via
`tools.cgi?op=reg_write` with `confirm:"YES_I_AM_SURE"` in the daemon's
allow-list (see `OPERATIONS.md` §5).

| Offset | Name | Width | R/W | Reset | Description |
|---|---|---|---|---|---|
| `0x200` | `REG_SCRATCH` | `[31:0]` | R/W (byte-laned) | `0` | Carry-over scratch register, byte-lane strobes honoured. Used by RTL TBs to verify byte-strobe routing. |
| `0x204` | `REG_TB_FORCE_AACH` | `[15:0]` + `[31]` valid | R/W | `0` | TB-only: when bit `[31]=1`, the LMAC AACH-encoder mux selects this raw word instead of the UMAC-driven pattern. **Synthesis-stripped in production builds via `` `ifdef TETRA_TB_HOOKS ``**; the address still exists in production but reads as 0 / writes are dropped. |
| `0x208` | `REG_TB_INJECT_BURST` | `[31:0]` | W-only | `0` | TB-only: writes trigger a single-burst stimulus injection into the RX-frontend mux (synth-stripped, same gating as 0x204). |
| `0x20C..0x2FF` | reserved (test) | — | R/O | `0` | future TB hooks. |

### Module home in `tetra_top.v`

The register window is implemented as a single Verilog module
`rtl/infra/tetra_axi_lite_regs.v` (Agent A5 deliverable). The carry-over
file of the same name in `tetra-zynq-phy/` is rewritten — only the
**module name, file path, AXI-Lite port-list, and Vivado
`X_INTERFACE_INFO` annotations** are kept verbatim so the Vivado Block
Design auto-connects the PS7 GP0 master to the bank without
re-customisation.

**Fan-out from `tetra_axi_lite_regs.v`:**

```
tetra_axi_lite_regs.v
  ├── cfg_cell_*         → rtl/lmac/tetra_sb1_encoder.v       (MCC/MNC/CC/LA into SYSINFO)
  ├── cfg_scrambler_init → rtl/lmac/tetra_scrambler.v
  ├── cfg_ts_{n,p,q}     → rtl/lmac/tetra_burst_builder.v + tetra_sync_detect.v
  ├── cfg_cipher_mode    → rtl/lmac/tetra_lmac.v              (gates encryption FSM, currently no-op since cipher=0 in Gold)
  ├── cfg_rx_carrier_hz  → AD9361 control path (libiio shadow; no direct RTL consumer; daemon also writes AD9361 via libiio)
  ├── cfg_tx_carrier_hz  → AD9361 control path (same)
  ├── cfg_tx_gain_trim   → rtl/phy/tetra_tx_chain.v           (final-stage digital gain multiplier)
  ├── cfg_ctrl_*         → top-level enable strobes (RX_EN/TX_EN/LOOPBACK/RST_CNTRS)
  ├── cfg_slot_table[20] → rtl/umac/tetra_dl_signal_scheduler.v  (lookahead slot purpose + AACH hint)
  └── tlm_*              ← all telemetry counters (1-direction reads from PHY/LMAC/UMAC/DMA)
```

The 20-entry **SLOT_TABLE is a separate sub-module**
`rtl/infra/tetra_slot_table_bram.v` (also A5 deliverable, ~120 LOC). It
wraps a 20×32 distributed-RAM (Vivado `RAM32M`-style, no BRAM block
needed at this depth) with a write port from `tetra_axi_lite_regs.v` and
an asynchronous read port for `tetra_dl_signal_scheduler.v`. Kept
separate from the main register file so synthesis can map it to LUT-RAM
without forcing the rest of the register decoder into a BRAM.

The carry-over `tetra_axi_dma_bridge.v` is **not** carried into
`rtl/infra/`; it is replaced by the 4× `tetra_axi_dma_wrapper.v`
instances (Agent A1 deliverable, see `MIGRATION_PLAN.md` §A1). The
register window's DMA-counter inputs come from those wrappers, not from
the old single-bridge module.

### Daemon-side mapping

The C daemon `tetra_d` uses a single typed header
`sw/include/tetra/axi_regmap.h` (S0/S7 deliverable, derived 1:1 from
this chapter) for the offset constants and bit-field shifts. Header
generation is manual; CI checks the offsets in this chapter against
`axi_regmap.h` via `scripts/check_axi_regmap.py` (T3 deliverable, runs
in `make sw-test`).

## Hardware

See `HARDWARE.md`.
