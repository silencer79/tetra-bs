# tetra-bs

TETRA basestation, FPGA-PHY/LMAC/UMAC + SW-LLC/MLE/MM/CMCE/SNDCP architecture
on Zynq-7020 (LibreSDR + AD9361).

## Architecture

This project follows the bluestation layer split:

```
┌──────────────────────────────────────────────┐
│ FPGA (Zynq PL): PHY + LMAC + UMAC            │
│  - SDR-IO (AD9361 LVDS, CIC, RRC, AGC)       │
│  - Symbol-Sync, Demod, Modulator             │
│  - Channel-Coding (Viterbi, RCPC, RM, AACH)  │
│  - Slot-Scheduler, Burst-Build, AACH-Encoder │
│  - Reassembly + FCS                          │
│  - MAC-RESOURCE wrap                         │
└────────────┬─────────────────────────────────┘
             │ AXI-DMA: TmaSap + TmdSap
             │ (TMAS / TMAR / TMDC magic frames)
┌────────────┴─────────────────────────────────┐
│ SW (Zynq PS, Cortex-A9, Linux):              │
│  - Core: Message-Bus + SapMsg + priorities   │
│  - LLC, MLE, MM, CMCE, SNDCP                 │
│  - Subscriber-DB (JSON-persistent)           │
│  - WebUI (Unix-socket to daemon)             │
│  - tetra_d daemon                            │
└──────────────────────────────────────────────┘
```

**Verhaltens-Source-of-Truth:** Gold-Reference-Captures bit-genau (`docs/references/`).
**Strukturvorlage:** bluestation Rust stack — Layer-Schnitt, SAP-Definitionen, Datentypen, ETSI-Spec-Klauseln 1:1 als Kommentare. Code: Verilog (FPGA) + C (SW), kein Rust-Port.

## Status

**Phase 0 — Skeleton.** Migration plan written, code structure created, validated FPGA modules carried over from previous project (`tetra-zynq-phy`). No SW stack yet, no DMA boundary yet, no WebUI yet.

See `docs/MIGRATION_PLAN.md` for the full plan + open work items.

## Hardware

- LibreSDR (Zynq-7020 + AD9361)
- 2 boards available: Board #1 = migration target, Board #2 = sniffer/emulator/capture
- Test MS: 2× Motorola MTP3550 (identical FW, codeplug-differentiated)

## License

GPL-2.0 (matches Creonic/bluestation references and existing TETRA tooling).

## Origin

Forked design knowledge from `tetra-zynq-phy` (sister project under same GitHub
account). PHY + LMAC channel-coding modules carried over verbatim and validated
on-air. Rest is fresh implementation against bluestation structure +
gold-reference behavior.
