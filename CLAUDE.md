# CLAUDE.md — Project Conventions

## Source-of-Truth Hierarchy

1. **Gold-Reference Captures** (`docs/references/reference_*.md` + `wavs/gold_*`)
   bit-genau verbindlich. Keine Abweichung erlaubt.
2. **Bluestation Rust stack** — Strukturvorlage. SAPs, Datentypen, Layer-Schnitt,
   ETSI-Spec-Klausel-Verweise 1:1. Bit-Layouts NUR wenn Gold-Ref schweigt.
3. **ETSI EN 300 392-2** — Tie-Breaker wenn Gold-Ref UND bluestation schweigen.

Bei Konflikt zwischen Quellen: Gold > Bluestation > ETSI. STOP, melden, nicht raten.

## Languages

- **FPGA:** Verilog-2001. Keine SystemVerilog. Keine VHDL (Ausnahme: 3rd-party IP-Cores).
- **SW:** C (C11). Keine C++. Keine Rust-Ports.
- **Scripts:** Python 3 für Decoder/Analyzer. Bash für deploy/build.

## No-Go Patterns

- Keine erfundenen Zahlen ("13ms reserve", "300µs latency", "11 days") ohne Messung.
- Keine `Todo`-Felder mit Phantasie-Defaults füllen — Gold-Ref-Wert lesen oder ETSI nehmen.
- Keine Layer-Vermischung — UMAC↔MM kommunizieren NUR über TmaSap/TmdSap.
- Keine SW-Override-Pfade für AACH — UMAC im FPGA bestimmt AACH-Pattern selbst.

## Bluestation-Komponenten die wir NICHT übernehmen

- `net_brew`, `net_telemetry`, `net_control` — bluestation-spezifische
  externe Telemetry/Control-Server. Wir bauen eigenes umfangreiches WebUI.

## Test-Strategie

1. **SW Unit-Tests** auf Host (Cross-Compile-Stack mit gleichem Code, Build für x86)
2. **RTL TBs** mit IVerilog/Vivado-Sim
3. **Co-Simulation** Verilator + C-Daemon-Binary für vollen FPGA↔SW-Pfad
4. **Live A/B-Test** Board #1 (neu) vs Board #2 (Sniffer/Reference)

Alle Tests laufen gegen Gold-Reference Bit-Vektoren. Diff > 0 bit blocks merge.

## Boards

- **Board #1:** Migration-Target. Bekommt jeden neuen Bitstream + SW-Stack.
- **Board #2:** Spezial-Aufgaben. UL/DL-Sniffer, MS-Emulator, CMCE-Voice-Capture, Latency-Measurement.

Test-MS: 2× Motorola MTP3550 (identische Firmware, codeplug-unterschiedlich).

## Git

- `main` = always green: builds, RTL passes TBs, SW passes unit-tests, no air-test failures.
- Big-Bang migration via Branches. Branch-Names: `feat/*`, `fix/*`, `refactor/*`, `chore/*`.
- Co-Authored-By: Claude `<noreply@anthropic.com>` für AI-assisted Commits.

## Documentation

Sources stay in `docs/`:
- `ARCHITECTURE.md` — System-Architektur, Layer-Schnitt, AXI-DMA-Spec
- `PROTOCOL.md` — TETRA SAPs, PDU-Layouts, Channel-Coding
- `HARDWARE.md` — LibreSDR, AD9361, Boards, Pinout
- `OPERATIONS.md` — Build, Deploy, WebUI, Debug
- `MIGRATION_PLAN.md` — Phasen-Plan + offene TODOs
- `references/` — Gold-Ref-Memories + ETSI PDFs

KEINE neuen Top-Level-MD-Files anlegen. In existing docs integrieren.
