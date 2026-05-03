# Protocol Reference

## TETRA Stack Mapping

| Layer | Code location | Source-of-truth |
|---|---|---|
| PHY | `rtl/phy/` | ETSI EN 300 392-2 §5 + Gold-Ref WAVs |
| LMAC | `rtl/lmac/` | ETSI EN 300 392-2 §11 + bluestation `lmac/components/` |
| UMAC | `rtl/umac/` | ETSI EN 300 392-2 §21 + bluestation `umac/` |
| LLC | `sw/llc/` | ETSI EN 300 392-2 §22 + bluestation `llc/` |
| MLE | `sw/mle/` | ETSI EN 300 392-2 §18 + bluestation `mle/` |
| MM | `sw/mm/` | ETSI EN 300 392-2 §16 + bluestation `mm/` |
| CMCE | `sw/cmce/` | ETSI EN 300 392-2 §14 + bluestation `cmce/` |
| SNDCP | `sw/sndcp/` | ETSI EN 300 392-2 §28 + bluestation `sndcp/` |

## SAPs

### TpSap (PHY ↔ LMAC, FPGA-internal)

ETSI EN 300 392-2 §11.2.

Carries raw type-5 burst bits per timeslot. Internal to FPGA.

### TmvSap (LMAC ↔ UMAC, FPGA-internal)

ETSI EN 300 392-2 §11.4.

Carries decoded type-1 bits per logical channel. Internal to FPGA.

### TmaSap (UMAC ↔ LLC, FPGA↔SW boundary) — see ARCHITECTURE.md

ETSI EN 300 392-2 §20.4.1.1.

Primitives:
- `TmaUnitdataReq` — LLC → MAC, transmit TM-SDU
- `TmaUnitdataInd` — MAC → LLC, deliver received TM-SDU
- `TmaCancelReq` — LLC → MAC, cancel outstanding request
- `TmaReleaseInd` — MAC → LLC, channel disconnected
- `TmaReportInd` — MAC → LLC, request progress report

### TmdSap (UMAC ↔ CMCE, FPGA↔SW boundary) — see ARCHITECTURE.md

Carries TCH/S ACELP frames per timeslot.

Primitives:
- `TmdCircuitDataReq` — CMCE → MAC
- `TmdCircuitDataInd` — MAC → CMCE

### Higher SAPs (SW-internal)

- TlaSap (LLC ↔ MLE) — ETSI EN 300 392-2 §20.4
- TleSap (MLE ↔ MM) — ETSI EN 300 392-2 §18.4
- TlmbSap (MLE ↔ CMCE)
- TlmcSap (MLE ↔ SNDCP)
- TnmmSap (MM ↔ NMM)

Bit layouts: see bluestation `tetra-saps/src/` and `docs/references/`.

## PDU Bit Layouts

For bit-genau verbindliche layouts of TETRA PDUs see `docs/references/`:

- `reference_gold_attach_bitexact.md` — ITSI-Attach M2 (UL#0, UL#1, UL#2, DL#727, DL#735)
- `reference_demand_reassembly_bitexact.md` — UL-Demand Frag-1 + Frag-2 → 132-bit MM body
- `reference_group_attach_bitexact.md` — Group-Attach 3-way handshake
- `reference_cmce_group_call_pdus.md` — CMCE Group-Call PDUs (layouts from bluestation, bit-verification pending TODO-A)
- `reference_gold_full_attach_timeline.md` — Frame-genaue Timeline aller M2-Sequenzen
- `reference_subscriber_db_arch.md` — DB-Records §9.2 layout

## Channel Coding

### SCH/F (signalling, 268 bits → 432 bits)

CRC-16 + RCPC rate 2/3 + matrix interleaver (K=216) + LFSR scrambler.
RTL: `rtl/lmac/tetra_sch_f_encoder.v` (carried over).

### SCH/HD (signalling half-block, 124 bits → 216 bits)

Same chain at half-rate.

### SCH/HU (uplink RA-burst, ~92 bits)

UL-only, hardened encoding for random-access. Decoder: `rtl/lmac/tetra_ul_viterbi_r14.v`.

### TCH/S (voice, 274 bits ACELP per slot)

Light protection. RTL TBD when CMCE-Voice-Path implemented.

### AACH (14 bits → Reed-Muller (30,14))

Per-slot access announcement.
RTL: `rtl/lmac/` Reed-Muller encoder + `rtl/lmac/tetra_aach_encoder.v`.

### BSCH (60 bits → SB1 burst)

Carries SYSINFO + sync. RTL: `rtl/tx/tetra_sb1_encoder.v` (carried over).

## Constants from bluestation `bs_sched.rs`

- `SCH_HD_CAP = 124` (bits)
- `SCH_F_CAP = 268` (bits)
- `TCH_S_CAP = 274` (bits)
- `MACSCHED_TX_AHEAD = 1` (timeslots)
- `MACSCHED_NUM_FRAMES = 18`
- `NUM_TIMESLOTS = 4`
- `NULL_PDU_LEN_BITS = 16`

## Frame / Slot / Multiframe Geometry

- 1 slot = ~14.167 ms / 4 = 3.5417 ms
- 1 frame = 4 slots = 14.167 ms
- 1 multiframe = 18 frames = 255 ms
- 1 hyperframe = 60 multiframes = 15.3 s
