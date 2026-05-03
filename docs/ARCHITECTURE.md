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

## Hardware

See `HARDWARE.md`.
