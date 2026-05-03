# Migration Plan — tetra-bs

**Status:** Planning complete, implementation pending.
**Created:** 2026-05-03
**Source project:** `tetra-zynq-phy` (sister repo, same GitHub account)

---

## Architecture Decisions (locked)

These were agreed during the planning conversation and are not up for re-debate
without explicit user approval.

| # | Decision | Choice |
|---|---|---|
| 1 | FPGA↔SW boundary | bluestation-aligned: PHY+LMAC+UMAC in FPGA, LLC+MLE+MM+CMCE+SNDCP in SW |
| 2 | SAP boundary at | TmaSap (signalling) + TmdSap (voice TCH) |
| 3 | Transport | 4× AXI-DMA channels (TmaSap RX/TX, TmdSap RX/TX) |
| 4 | Frame format | Self-describing with magic headers: `TMAS` (0x544D_4153 signalling), `TMAR` (0x544D_4152 reports), `TMDC` (0x544D_4443 voice) |
| 5 | AACH-Encoder | Stays in FPGA. UMAC-Scheduler determines pattern. **No SW override path.** |
| 6 | req_handle / Reports | bluestation 1:1. TmaReportInd via signalling-RX-channel with TMAR magic |
| 7 | Migration strategy | Big-Bang. One branch, hard cut-over. No hybrid mode. |
| 8 | DB format | JSON, single file `/var/lib/tetra/db.json` |
| 9 | WebUI ↔ Daemon | Unix-Socket. Daemon authoritative. CGI is socket client. |
| 10 | AST persistence | Hybrid. Persist on clean SIGTERM, flush on crash. Reload only if `clean_shutdown_flag=true` |
| 11 | Test strategy | SW unit-tests + Verilator co-sim + live A/B on 2 boards |
| 12 | Hardware | Board #1 = migration target, Board #2 = sniffer/emulator |
| 13 | Message Bus | bluestation-style: central queue + priorities, `SapMsg{src,dest,sap,msg}` |
| 14 | Excluded bluestation parts | `net_brew`, `net_telemetry`, `net_control` (own WebUI replaces them) |
| 15 | Todo-field defaults | Extract from Gold-Reference captures. ETSI defaults only when Gold silent. |
| 16 | Code language | Verilog (FPGA), C (SW). No Rust ports, no SystemVerilog, no C++ |
| 17 | Documentation | `docs/` — integrate into existing files, no new top-level MD |
| 18 | License | GPL-2.0 |
| 19 | Source-of-truth hierarchy | Gold-Ref > Bluestation > ETSI |

---

## Layer Mapping (FPGA vs SW)

### FPGA modules (carried over from `tetra-zynq-phy`, validated)

- `rtl/phy/`
  - AD9361-LVDS interface
  - RX-Frontend: CIC, decimation, RRC matched filter, AGC
  - TX-Frontend: π/4-DQPSK modulator, RRC pulse-shaping, CIC, up-conversion
  - Symbol-Sync, Burst-Detector, Demodulator
- `rtl/lmac/`
  - Channel-Coding: Viterbi (`tetra_ul_viterbi_r14.v`), RCPC encoder/decoder, Reed-Muller AACH (RM30,14), BSCH-Encoder
  - Scrambler, Interleaver, CRC16
  - AACH-Encoder (`tetra_aach_encoder.v`) — pattern from UMAC scheduler
- `rtl/umac/`
  - `tetra_ul_demand_reassembly.v` — Frag-1 + Frag-2 → 129-bit MM body (TmaSap-RX source)
  - `tetra_ul_mac_access_parser.v`
  - `tetra_dl_signal_queue.v` + `tetra_dl_signal_scheduler.v`
  - `tetra_sch_f_encoder.v` + `tetra_mac_resource_dl_builder.v` (gets body via TmaSap-TX)
- `rtl/infra/`
  - 4× AXI-DMA wrapper (Xilinx LogiCORE `axi_dma:7.1`, free in Webpack)
  - TmaSap/TmdSap frame packer + unpacker
  - CDC for clk_axi ↔ clk_sys
- `rtl/tetra_top.v` — top-level instantiation

### FPGA modules to delete from carry-over (bluestation-non-conformant)

These existed in `tetra-zynq-phy` and are NOT brought over:

- `tetra_ul_demand_ie_parser.v` — IEP wandert nach SW (MM-Layer)
- `tetra_mle_registration_fsm.v` + Subs — MLE wandert nach SW
- `tetra_entity_table.v`, `tetra_profile_table.v`, `tetra_active_session_table.v` — Subscriber-DB nach SW
- `tetra_d_location_update_encoder.v` + `_reject_encoder.v` — Accept-Builder nach SW
- All `REG_SHADOW_*`, `REG_PROFILE_*`, `REG_DB_POLICY` AXI register — DB pure SW
- `REG_AACH_GRANT_HINT` — AACH bestimmt UMAC-Scheduler intern, kein SW-Hint

### SW modules (new, fresh implementation)

- `sw/core/` — Message-Bus, SapMsg dispatcher, Common types (TetraAddress, BitBuffer, TdmaTime, EndpointId, BurstType, TrainingSequence, LogicalChannel, PhysicalChannel, SsiType, …)
- `sw/llc/` — Logical Link Control: BL-DATA/BL-ADATA/BL-UDATA/BL-ACK, NR/NS sequencing, FCS (CRC-32), retransmission
- `sw/mle/` — Mobility Link Entity: registration FSM, attach/detach, multi-lookup
- `sw/mm/` — Mobility Management: U/D-LOC-UPDATE-DEMAND/ACCEPT/REJECT, ATTACH-DETACH-GRP-ID, IE-Parser GILD, Accept-Builder
- `sw/cmce/` — Circuit Mode Control Entity: U/D-SETUP, D-CALL-PROCEEDING, D-CONNECT, U-TX-DEMAND, D-TX-GRANTED, U/D-RELEASE, D-NWRK-BROADCAST
- `sw/sndcp/` — Subnetwork Dependent Convergence Protocol (packet data, deferred)
- `sw/persistence/` — JSON DB read/write (jansson or cJSON), atomic-rename, AST snapshot
- `sw/webui/` — CGI binaries + Unix-socket client + JSON request/response
- `sw/tetra_d.c` — main daemon: DMA event loop, message dispatcher, signal handlers

---

## Open Work Items (TODOs to complete planning)

These are identified but not yet executed. Each should be a separate work session
(don't bundle into one context to avoid quality drop):

### TODO-A — Bluestation `Todo`-Felder aus Gold-Ref extrahieren (CRITICAL)

For every field marked `Todo` in bluestation source: read the Gold-Reference
captures bit-by-bit and document the actual value the real BS sends. Never
assume "0 because bluestation Todo".

Fields to extract (per PDU type):
- TmaSap: `subscriber_class`, `air_interface_encryption`, `data_category`, `pdu_prio`, `chan_change_handle`, `chan_info`, `endpoint_id`, `new_endpoint_id`, `css_endpoint_id`, `chan_change_response_req`, `stealing_permission`, `stealing_repeats_flag`
- CmceChanAllocReq: `usage`, `carrier`
- mm/u_location_update_demand: `class_of_ms`, `energy_saving_mode`, `optional_field_value`
- mm/d_location_update_accept: `loc_acc_type`, `energy_saving_info`, all p-bits
- cmce/d_setup, u_setup, d_call_proceeding, d_connect, u_tx_demand, d_tx_granted, u/d_release: all fields
- d_nwrk_broadcast: full layout

Captures to use: `wavs/gold_*` (M2-Attach, Group-Attach, DETACH, Group-Call, NWRK-Broadcast).

Output: `docs/references/gold_field_values.md` — table per field, Gold-value(s),
in which captures observed, ETSI-default if Gold silent.

### TODO-B — WebUI Spec (umfangreich)

Catalog of views / endpoints. Lieber zu viel als zu wenig Einstellungen.

Sections to define:
- **Live Status:** all layer states, AST live view, current calls, RX statistics, TX queues, AACH sequence, FPGA register dump
- **Subscriber DB:** Profile editor (all 6 fields), Entity editor (256 entries), Session view (read-only)
- **Debug:** PDU trace with hex bits, AACH timeline, slot schedule, DMA counters, IRQ counters, message-bus tap
- **Configuration:** MCC/MNC/CC/LA, frequencies (RX/TX), TX power, cipher mode, scrambler, training sequences, slot table
- **Tools:** manual PDU sender, reset buttons (counters, AST, DB), bitstream switch, capture trigger, decoder upload

Output: `docs/OPERATIONS.md` new chapter "WebUI" + endpoint reference.

### TODO-C — Big-Bang Branch + parallele Agenten Topologie

Define the parallel work breakdown. Each agent has a clear contract (interfaces,
test gates, deliverables). Cross-dependencies declared.

Output: `docs/MIGRATION_PLAN.md` new section "Agent Topology" — task list per
agent, blocks/blockedBy graph, sync-points.

### TODO-D — External Dependencies (toolchain audit)

Catalog every external dependency, version, install path, license.

Required:
- Vivado version (we use 2022.2)
- Cross-compiler ARM (`arm-linux-gnueabihf-gcc` version)
- Linux kernel version on board (driver compat for axi_dma)
- jansson or cJSON (JSON-Lib) — pick one
- Unity or Check (C unit-test framework) — pick one
- Verilator version (co-sim)
- xilinx_axidma driver source/version
- httpd: which busybox httpd version, CGI conventions
- gcc-host for SW unit-tests (x86)

Output: `docs/HARDWARE.md` new chapter "Toolchain + Dependencies".

---

## Agent Topology

Phase-2 work breakdown for parallele Big-Bang Implementation. Each agent owns a
contract — Scope, Deliverables, Interface (lock-point), Test gate, Blocks /
BlockedBy. Once interfaces are locked (see "Interface-locking schedule" at the
end of this section), agents run concurrently.

Conventions:
- Pfade sind relativ zum Repo-Root.
- "Lines"-Budgets sind grobe Ziel-Hausnummern (S/M/L/XL Effort), keine
  Hard-Limits.
- Effort: **S** = ≤1 module/file & happy-path; **M** = mehrere Files,
  unit-tested; **L** = mehrere Layer-Module + cosim-relevant; **XL** =
  cross-cutting + Live-Air-bound.
- Languages-Locks (Decision #16) gelten unverändert: Verilog-2001 (FPGA), C11
  (SW), Python 3 (Scripts).
- Source-of-truth pro Agent: Gold > Bluestation > ETSI (Decision #19).
  Wo Gold-Ref vorliegt → `docs/references/gold_field_values.md` und
  `reference_*_bitexact.md` sind verbindlich.

### Agent index

| ID | Side | Title | Effort |
|---|---|---|---|
| `A1-fpga-axi-dma`            | FPGA   | 4× AXI-DMA Wrapper + DT overlay        | L  |
| `A2-fpga-tmasap-framer`      | FPGA   | TmaSap RX/TX Framer + TMAR Reports     | L  |
| `A3-fpga-tmdsap-framer`      | FPGA   | TmdSap RX/TX Framer (TMDC, voice)      | M  |
| `A4-fpga-cdc-clkdomains`     | FPGA   | CDC clk_axi ↔ clk_sys + reset bridge   | M  |
| `A5-fpga-top-xdc-cleanup`    | FPGA   | `tetra_top.v` + xdc + delete-list      | L  |
| `A6-fpga-umac-carryover`     | FPGA   | UMAC carry-over Re-Validation          | M  |
| `S0-sw-core-msgbus-types`    | SW     | core types + Message-Bus               | L  |
| `S1-sw-dma-glue`             | SW     | xilinx_axidma userspace driver glue    | M  |
| `S2-sw-llc`                  | SW     | LLC (BL-DATA/ACK, NR/NS, FCS)          | L  |
| `S3-sw-mle-mm`               | SW     | MLE FSM + MM IE-Parser/Builder         | XL |
| `S4-sw-cmce`                 | SW     | CMCE (Setup/TX-Demand/Granted/Release) | L  |
| `S5-sw-persistence`          | SW     | jansson DB + AST snapshot/reload       | M  |
| `S6-sw-webui-cgis`           | SW     | CGI binaries + cgi_common              | L  |
| `S7-sw-tetra-d`              | SW     | tetra_d.c main daemon + dispatcher     | L  |
| `T1-tb-rtl`                  | Test   | RTL TBs per new RTL block              | L  |
| `T2-cosim-verilator`         | Test   | Verilator + C-binary cosim harness     | L  |
| `T3-tb-sw-host`              | Test   | Host x86 SW unit-tests + decoder helps | M  |

(16 Agents — siehe einzelne Verträge unten.)

---

### A1 — `A1-fpga-axi-dma` — 4× AXI-DMA Wrapper + DT overlay

| Field | Value |
|---|---|
| **Side** | FPGA |
| **Scope** | Instanziiert 4× LogiCORE `axi_dma:7.1` (TmaSap RX, TmaSap TX, TmdSap RX, TmdSap TX) im Scatter-Gather-Mode. Liefert Stream-IF nach innen, AXI4-MM nach außen, plus PL-DT-Overlay-Snippet für die 4 Nodes. |
| **Deliverables** | `rtl/infra/tetra_axi_dma_wrapper.v` (~400 LOC), `rtl/infra/tetra_dma_descriptor_ring.v` (~250 LOC), Vivado-IP-Tcl `rtl/infra/ip/axi_dma_*.tcl` (4×), `boot/devicetree/tetra_dma_overlay.dtsi`. |
| **Interface contract** | Verilog top-level wrapper port-list: `axi_dma_wrapper(clk_axi, rstn_axi, s_axi_lite_*, m_axi_mm2s_*, m_axi_s2mm_*, axis_tma_rx_*, axis_tma_tx_*, axis_tmd_rx_*, axis_tmd_tx_*, irq_mm2s[3:0], irq_s2mm[3:0])`. AXIS-Streams: `tdata[31:0], tvalid, tready, tlast, tkeep[3:0]`. Naming locked: `IF_AXIDMA_v1`. |
| **Inputs consumed** | Decision #3 (4 Channels). HARDWARE.md §4 (libaxidma decision = vendored Jacob-Feder). |
| **Test gate** | TB `tb/rtl/tb_axi_dma_wrapper.v` mit BFM: descriptor-ring round-trip 1 KiB MM2S + 1 KiB S2MM auf jedem der 4 Channels, IRQ-assert geprüft, no AXI protocol errors (Vivado axi_protocol_checker). |
| **Blocks** | `A2`, `A3`, `A5`, `S1`. |
| **BlockedBy** | — (kann sofort nach Interface-Lock starten). |
| **Effort** | L |

---

### A2 — `A2-fpga-tmasap-framer` — TmaSap RX/TX Framer + TMAR Reports

| Field | Value |
|---|---|
| **Side** | FPGA |
| **Scope** | Packt ARCHITECTURE.md TmaSap-Frame (`TMAS` 0x544D4153 + `TMAR` 0x544D4152) auf AXIS in beide Richtungen. RX: konsumiert reassembled MM-Body von `tetra_ul_demand_reassembly.v` + Slot-Meta, baut TMAS-Frame, pusht über `axis_tma_rx_*`. TX: parsed TMAS vom Daemon, übergibt Body+Meta an `tetra_mac_resource_dl_builder.v`. TMAR generiert UMAC-Scheduler bei `req_handle`-Events (ConfirmHandle / SuccessReservedOrStealing / FailedTransfer / FragmentationFailure). |
| **Deliverables** | `rtl/infra/tetra_tmasap_rx_framer.v` (~500 LOC), `rtl/infra/tetra_tmasap_tx_framer.v` (~500 LOC), `rtl/infra/tetra_tmar_report_emitter.v` (~200 LOC). |
| **Interface contract** | RX: `tmasap_rx_framer(clk, rstn, in_pdu_bits[131:0], in_pdu_len_bits[10:0], in_meta_*, in_valid, in_ready, m_axis_*)`. TX: `tmasap_tx_framer(clk, rstn, s_axis_*, out_pdu_bits[..], out_meta_*, out_valid, out_ready, req_handle_out[31:0], chan_alloc_out[11:0])`. Frame-Layout BIT-EXACT laut ARCHITECTURE.md "Frame format (RX/TX)". Locked: `IF_TMASAP_FRAME_v1`. |
| **Inputs consumed** | `IF_AXIDMA_v1` (A1). UMAC carry-over Ports (A6). ARCHITECTURE.md §"FPGA↔SW Boundary" für Byte-Layout. |
| **Test gate** | TB `tb/rtl/tb_tmasap_framer.v`: Inject Gold-Ref-MM-Body (132-bit aus `reference_demand_reassembly_bitexact.md`) → Frame-Bytes vergleichen Byte-für-Byte gegen vorab-generierten Reference-Frame (Python helper in `scripts/gen_tmasap_frame_ref.py`). Diff > 0 byte → Fail. |
| **Blocks** | `A5`, `S0` (interface-only), `T2`. |
| **BlockedBy** | `A1` (AXIS), `A6` (UMAC ports). |
| **Effort** | L |

---

### A3 — `A3-fpga-tmdsap-framer` — TmdSap RX/TX Framer (TMDC, voice)

| Field | Value |
|---|---|
| **Side** | FPGA |
| **Scope** | TmdSap-Frame `TMDC` (0x544D4443) für TCH/S ACELP, 274 bits MSB-aligned, 44 bytes total laut ARCHITECTURE.md. Beide Richtungen. Per-timeslot (1..4). |
| **Deliverables** | `rtl/infra/tetra_tmdsap_rx_framer.v` (~250 LOC), `rtl/infra/tetra_tmdsap_tx_framer.v` (~250 LOC). |
| **Interface contract** | `tmdsap_rx_framer(clk, rstn, in_acelp_bits[273:0], in_timeslot[1:0], in_valid, in_ready, m_axis_*)` und symmetrisch TX. Locked: `IF_TMDSAP_FRAME_v1`. |
| **Inputs consumed** | `IF_AXIDMA_v1` (A1). ARCHITECTURE.md §"TmdSap (Voice TCH)". |
| **Test gate** | TB `tb/rtl/tb_tmdsap_framer.v`: 4 timeslots × 1 ACELP-Pattern → Frame-Layout-check (44 bytes, magic, slot, MSB-alignment). Bit-Exact-Vergleich noch nicht möglich (kein Gold-Ref-Voice-Frame extrahiert; siehe `gold_field_values.md` Caveat). Daher: structural-only TB, ggf. später nachgeschärft. |
| **Blocks** | `A5`. |
| **BlockedBy** | `A1`. |
| **Effort** | M |

---

### A4 — `A4-fpga-cdc-clkdomains` — CDC clk_axi ↔ clk_sys

| Field | Value |
|---|---|
| **Side** | FPGA |
| **Scope** | Saubere clock-domain-Crossings zwischen `clk_axi` (PS-Side, ~100 MHz) und `clk_sys` (PL-Sample-Side, AD9361-getrieben). Async-FIFOs für AXIS-Streams + 2-Flip-Flop-Sync für single-bit Status/IRQ. |
| **Deliverables** | `rtl/infra/tetra_cdc_axis_async_fifo.v` (~150 LOC), `rtl/infra/tetra_cdc_sync_2ff.v` (~50 LOC), `rtl/infra/tetra_cdc_pulse.v` (~80 LOC). Verwendet in A1+A5 Top. |
| **Interface contract** | `cdc_axis_async_fifo(clk_wr, rstn_wr, clk_rd, rstn_rd, s_axis_*, m_axis_*, depth_log2)`. Locked: `IF_CDC_AXIS_v1`. |
| **Inputs consumed** | — (pure infra-block). |
| **Test gate** | TB `tb/rtl/tb_cdc_async_fifo.v` mit zwei async-Clocks (97 MHz / 36.864 MHz Beat), random push+pop, kein Datenverlust, kein duplicate. Plus Vivado `report_cdc` clean (no critical warnings). |
| **Blocks** | `A1`, `A2`, `A3`, `A5`. |
| **BlockedBy** | — (kann parallel zu A1 starten, da CDC unabhängig vom DMA-Wrapper-Inneren). |
| **Effort** | M |

---

### A5 — `A5-fpga-top-xdc-cleanup` — `tetra_top.v` + xdc + delete-list cleanup

| Field | Value |
|---|---|
| **Side** | FPGA |
| **Scope** | Neues Top-Level: instanziiert PHY + LMAC + UMAC (carry-over) + A1/A2/A3/A4. Constraints `libresdr_tetra.xdc` updaten für 4× DMA-IRQ-Lines + neue AXI-MM-Ports. Löscht (entfernt aus Build-File-List **und** Repo-Tree, nicht nur `.gitignore`d) die in MIGRATION_PLAN.md "FPGA modules to delete from carry-over" gelisteten Files. Entfernt `REG_SHADOW_*`, `REG_PROFILE_*`, `REG_DB_POLICY`, `REG_AACH_GRANT_HINT` AXI-Register-Decoder. |
| **Deliverables** | `rtl/tetra_top.v` (~600 LOC), updated `constraints/libresdr_tetra.xdc`, neuer Vivado-Tcl-Build-Script-Anpassung in `scripts/vivado_build.tcl`. **Delete:** `tetra_ul_demand_ie_parser.v`, `tetra_mle_registration_fsm.v` (+ subs), `tetra_entity_table.v`, `tetra_profile_table.v`, `tetra_active_session_table.v`, `tetra_d_location_update_encoder.v`, `tetra_d_location_update_reject_encoder.v`. **Diese files sind aktuell schon NICHT in `rtl/`** — der Cleanup-Schritt verifiziert nur, dass keine versehentlichen Wiedereinführungen passierten + dass nichts unter `rtl/umac/` oder `rtl/lmac/` darauf referenziert. |
| **Interface contract** | `tetra_top` Port-Liste (Top-of-FPGA): AD9361-LVDS-Ports + 4× AXI-DMA-Master + 1× AXI-Lite-Slave (Config-Reg-Window) + IRQs + clk/rst. Locked: `IF_TETRA_TOP_v1`. |
| **Inputs consumed** | `IF_AXIDMA_v1` (A1), `IF_TMASAP_FRAME_v1` (A2), `IF_TMDSAP_FRAME_v1` (A3), `IF_CDC_AXIS_v1` (A4). |
| **Test gate** | (1) Vivado synth + impl green (timing met @ target clocks), (2) RTL-Top-TB `tb/rtl/tb_tetra_top.v` instanziiert reduced-PHY-stub und rennt einen DL-Burst-Build End-to-End durch SCH/F-Encoder ohne Daten-Verlust, (3) carry-over RTL-TBs (A6) bleiben grün am neuen Top. |
| **Blocks** | `T2` (cosim braucht den Top), Phase 4 Bitstream. |
| **BlockedBy** | `A1`, `A2`, `A3`, `A4`, `A6`. |
| **Effort** | L |

---

### A6 — `A6-fpga-umac-carryover` — UMAC carry-over Re-Validation

| Field | Value |
|---|---|
| **Side** | FPGA |
| **Scope** | Re-validiert die übernommenen UMAC-Module aus `tetra-zynq-phy` gegen ihre vorhandenen Gold-Ref-TBs. **Kein Logik-Change**, nur: Compile gegen neue File-Hierarchie, AXI-Lite-Register-Decoder schrumpfen (siehe A5 Delete-Liste), Port-Anpassungen für TmaSap/TmdSap-Framer (A2/A3). |
| **Deliverables** | Updates an `rtl/umac/tetra_ul_demand_reassembly.v`, `tetra_ul_mac_access_parser.v`, `tetra_dl_signal_queue.v`, `tetra_dl_signal_scheduler.v`, `tetra_mac_resource_dl_builder.v`, `tetra_mac_resource_bl_ack_builder.v`, `tetra_lmac.v`. Net change ≤ 200 LOC. |
| **Interface contract** | UMAC ↔ Framer-Ports: `umac_to_tmasap_rx(pdu_bits, pdu_len_bits, ssi, ssi_type, endpoint_id, scrambling_code, valid, ready)` + Symmetrie für TX. Locked: `IF_UMAC_TMASAP_v1`. |
| **Inputs consumed** | `reference_demand_reassembly_bitexact.md` (Frag-1+Frag-2 → 132-bit MM body) als RX-Reference; `reference_gold_attach_bitexact.md` als TX-Reference. |
| **Test gate** | 0/N bit diff der bestehenden carry-over RTL-TBs (sind in `tb/rtl/`, sollen alle weiter PASSen). Plus 0/132 bit diff für Reassembly gegen Gold-Ref-Frag-1+Frag-2-Pair aus M2-Attach-Capture. |
| **Blocks** | `A2`, `A5`. |
| **BlockedBy** | — (carry-over, in-place). |
| **Effort** | M |

---

### S0 — `S0-sw-core-msgbus-types` — core types + Message-Bus

| Field | Value |
|---|---|
| **Side** | SW |
| **Scope** | Common-Types-Header (`TetraAddress`, `BitBuffer`, `TdmaTime`, `EndpointId`, `BurstType`, `TrainingSequence`, `LogicalChannel`, `PhysicalChannel`, `SsiType`, `SapMsg`, `SapId`, `MsgPriority`) + zentrale Message-Bus-Implementation (3-Prio-Queues, single-dispatch-loop-API, callback-registration per `(dest, sap)`-Tuple) gemäß ARCHITECTURE.md §"Message Bus". Bluestation `tetra-core/src/` ist Strukturvorlage. |
| **Deliverables** | `sw/core/include/tetra/types.h` (~300 LOC), `sw/core/include/tetra/sap.h` (~200 LOC), `sw/core/include/tetra/msgbus.h`, `sw/core/src/msgbus.c` (~400 LOC), `sw/core/src/bitbuffer.c` (~300 LOC). |
| **Interface contract** | `int msgbus_init(MsgBus *bus, const MsgBusCfg *cfg);` `int msgbus_register(MsgBus *, SapId dest, SapId sap, msgbus_handler_fn cb, void *ctx);` `int msgbus_post(MsgBus *, MsgPriority, const SapMsg *msg);` `int msgbus_dispatch_one(MsgBus *);` (non-blocking, returns 0 wenn leer). `BitBuffer bb_init(uint8_t *buf, size_t len_bits);` `void bb_put_bits(BitBuffer*, uint32_t v, uint8_t n);` `uint32_t bb_get_bits(BitBuffer*, uint8_t n);`. Locked: `IF_CORE_API_v1`. |
| **Inputs consumed** | `gold_field_values.md` für Field-Bit-Widths (TetraAddress: ssi 24-bit, ssi_type 3-bit; EndpointId 32-bit; etc.). |
| **Test gate** | `tb/sw/test_core_msgbus.c` (Unity): post-N-messages-priority-order, register-dispatch-roundtrip, queue-overflow → drop+counter, BitBuffer round-trip 0..32-bit-widths. 100 % function coverage in core. |
| **Blocks** | `S1`, `S2`, `S3`, `S4`, `S5`, `S6`, `S7`. |
| **BlockedBy** | — (foundation). |
| **Effort** | L |

---

### S1 — `S1-sw-dma-glue` — xilinx_axidma userspace driver glue

| Field | Value |
|---|---|
| **Side** | SW |
| **Scope** | Vendoring + Build-Glue für Jacob-Feder `xilinx_axidma` (HARDWARE.md §4 Option B, gepinnter Commit), kombiniert mit einer C-API-Schicht die TmaSap/TmdSap-Frames empfängt+sendet (TMAS/TMAR/TMDC magic-parse + length-prefix-Reassembly). Macht 4 DMA-Channels über char-dev erreichbar. Kein Logik-Code — pure Transport. |
| **Deliverables** | `sw/external/xilinx_axidma/` (vendored, pinned commit-hash dokumentiert in `docs/HARDWARE.md` §10), `sw/core/src/dma_io.c` (~400 LOC) + Header. Kernel-Module-Build-Glue als `Makefile.kmod`. |
| **Interface contract** | `int dma_init(DmaCtx *, const DmaCfg *cfg);` `int dma_recv_frame(DmaCtx *, DmaChan ch, uint8_t *buf, size_t cap, size_t *out_len, int timeout_ms);` `int dma_send_frame(DmaCtx *, DmaChan ch, const uint8_t *buf, size_t len);` `int dma_get_irq_fd(DmaCtx *, DmaChan ch);` (für poll/epoll im daemon main loop). DmaChan ∈ {`TMA_RX`, `TMA_TX`, `TMD_RX`, `TMD_TX`}. Locked: `IF_DMA_API_v1`. |
| **Inputs consumed** | `IF_AXIDMA_v1` (A1, AXIS frame format), DT-overlay aus A1 (für device paths). |
| **Test gate** | `tb/sw/test_dma_loopback.c` als x86-mock (DMA-Sim mit pipe()-paaren) — exercise's send_frame/recv_frame round-trip, magic-parse, partial-frame-Reassembly. Echte Hardware-Validation in T2/Phase 4. |
| **Blocks** | `S7`. |
| **BlockedBy** | `A1` (Interface), `S0`. |
| **Effort** | M |

---

### S2 — `S2-sw-llc` — LLC (BL-DATA/ACK, NR/NS, FCS)

| Field | Value |
|---|---|
| **Side** | SW |
| **Scope** | Logical Link Control nach ETSI EN 300 392-2 §22 + bluestation `llc/`. PDU-Encoder/Decoder für BL-DATA, BL-ADATA, BL-UDATA, BL-ACK; NR/NS sequence-tracking pro endpoint; FCS (CRC-32, ETSI-Polynom); retransmission-state-machine. |
| **Deliverables** | `sw/llc/llc.c` (~700 LOC), `sw/llc/llc_pdu.c` (~500 LOC), `sw/llc/include/tetra/llc.h`. |
| **Interface contract** | `int llc_init(Llc*, MsgBus*, const LlcCfg*);` `int llc_handle_tma_unitdata_ind(Llc*, const TmaUnitdataInd*);` LLC postet `TlaXxx` an MLE über msgbus. Locked: `IF_LLC_v1`. |
| **Inputs consumed** | `IF_CORE_API_v1` (S0), TmaSap-Layout (ARCHITECTURE.md). |
| **Test gate** | `tb/sw/test_llc_pdu.c` (Unity): encode/decode round-trip BL-DATA/ACK; NR/NS-counter wraps; CRC-32 vs Gold-Ref-LLC-Frame-Bits aus M2-Attach (UL#0 enthält LLC-BL-ADATA mit FCS — siehe `reference_gold_attach_bitexact.md`). 0/32 bit diff FCS. |
| **Blocks** | `S3`, `S7`. |
| **BlockedBy** | `S0`. |
| **Effort** | L |

---

### S3 — `S3-sw-mle-mm` — MLE FSM + MM IE-Parser/Builder

| Field | Value |
|---|---|
| **Side** | SW |
| **Scope** | MLE registration FSM (attach/detach, multi-lookup) + komplette MM-Layer (`U/D-LOC-UPDATE-DEMAND/ACCEPT/REJECT`, `U/D-ATTACH-DETACH-GRP-ID(-DEMAND/-ACK)`, IE-Parser GILD, Accept-Builder). Größtes SW-Paket weil zwei Layer eng gekoppelt sind und beide Gold-Ref-bound. |
| **Deliverables** | `sw/mle/mle.c` (~800 LOC), `sw/mle/mle_fsm.c` (~600 LOC), `sw/mm/mm.c` (~500 LOC), `sw/mm/mm_iep.c` (~600 LOC, IE-Parser GILD), `sw/mm/mm_accept_builder.c` (~500 LOC), Header in beiden. |
| **Interface contract** | MLE: `int mle_init(Mle*, MsgBus*, SubscriberDb*, const MleCfg*);` MM: `int mm_init(Mm*, MsgBus*, SubscriberDb*, const MmCfg*);` IE-Parser: `int mm_iep_decode(const uint8_t *bits, size_t len_bits, MmDecoded *out);`. Beide Layer kommunizieren NUR über msgbus (TleSap zwischen MLE↔MM). Locked: `IF_MLE_v1`, `IF_MM_v1`. |
| **Inputs consumed** | `IF_CORE_API_v1`, `IF_LLC_v1`, **`gold_field_values.md`** (alle MM/MLE-Felder). `reference_gold_attach_bitexact.md` für DL#727 + DL#735 Bit-Layout. `reference_group_attach_bitexact.md` für D-ATTACH-DETACH-GRP-ID-ACK. |
| **Test gate** | `tb/sw/test_mm_d_loc_update_accept.c` (Unity): build D-LOC-UPDATE-ACCEPT für Gold-Ref M2-Scenario → 0/432 bit diff vs DL#727 + DL#735 (bit-exact). `test_mm_iep_d_attach_grp_ack.c`: 0/124 bit diff vs Gold-Ref Group-Attach-ACK. |
| **Blocks** | `S6` (sessions/entities WebUI braucht AST-Reads), `S7`. |
| **BlockedBy** | `S0`, `S2`, `S5` (DB-Access für AST + entity lookup). |
| **Effort** | XL |

---

### S4 — `S4-sw-cmce` — CMCE (Setup/TX-Demand/Granted/Release)

| Field | Value |
|---|---|
| **Side** | SW |
| **Scope** | Circuit Mode Control Entity nach ETSI §14 + bluestation `cmce/`. Encoder/Decoder + State-Machine für: U/D-SETUP, D-CALL-PROCEEDING, D-CONNECT, U-TX-DEMAND, D-TX-GRANTED, U/D-RELEASE, D-NWRK-BROADCAST. Letzteres mit 10 s Cadence (Gold-Ref). |
| **Deliverables** | `sw/cmce/cmce.c` (~800 LOC), `sw/cmce/cmce_pdu.c` (~600 LOC), `sw/cmce/cmce_fsm.c` (~400 LOC), `sw/cmce/cmce_nwrk_bcast.c` (~200 LOC, eigener Periodic-Driver). |
| **Interface contract** | `int cmce_init(Cmce*, MsgBus*, const CmceCfg*);` `int cmce_send_d_nwrk_broadcast(Cmce*);` (vom Scheduler 10s-cadence aufgerufen). Locked: `IF_CMCE_v1`. |
| **Inputs consumed** | `IF_CORE_API_v1`, `IF_LLC_v1`, `gold_field_values.md` Caveat-Sektion (CMCE-Felder bluestation-defaults), `reference_cmce_group_call_pdus.md`, `reference_gold_full_attach_timeline.md` §"D-NWRK-BROADCAST-Cadence" + `scripts/gen_d_nwrk_broadcast.py:GOLD_INFO_124`. |
| **Test gate** | `tb/sw/test_cmce_d_nwrk_broadcast.c` (Unity): builder output 124-bit-info-word == `GOLD_INFO_124` byte-für-byte. CMCE-Group-Call-PDUs: structural-only TB (kein Gold-Ref-Bit-Capture vorhanden, wird in Phase 4 nachgeschärft, MIGRATION_PLAN Phase 4 acceptance gate). |
| **Blocks** | `S7`. |
| **BlockedBy** | `S0`, `S2`. |
| **Effort** | L |

---

### S5 — `S5-sw-persistence` — jansson DB + AST snapshot/reload

| Field | Value |
|---|---|
| **Side** | SW |
| **Scope** | Subscriber-DB Read/Write (jansson, statisch gelinkt — HARDWARE.md §5), Atomic-Rename auf `/var/lib/tetra/db.json`. AST-Snapshot auf SIGTERM (`/var/lib/tetra/ast.json` + `clean_shutdown_flag` marker). Reload nur wenn `clean_shutdown_flag=true` (Decision #10). Profile-0 Invariant `0x0000_088F` enforce. |
| **Deliverables** | `sw/persistence/db.c` (~600 LOC), `sw/persistence/ast_snapshot.c` (~250 LOC), `sw/persistence/include/tetra/db.h`. |
| **Interface contract** | `int db_open(SubscriberDb*, const char *path);` `int db_get_profile(SubscriberDb*, uint8_t id, Profile *out);` `int db_put_profile(SubscriberDb*, uint8_t id, const Profile *);` `int db_get_entity(...);` `int db_lookup_entity(...);` `int db_atomic_save(SubscriberDb*);` `int ast_snapshot(const Ast*, const char *path);` `int ast_reload(Ast*, const char *path, bool *out_loaded);`. Locked: `IF_DB_API_v1`, `IF_AST_PERSIST_v1`. |
| **Inputs consumed** | `reference_subscriber_db_arch.md` für Record-Layout, ARCHITECTURE.md §"Subscriber-DB" für File-Locations + Profile-0-Invariante. |
| **Test gate** | `tb/sw/test_db_atomic.c` (Unity): write+rename, kill-mid-write → next open clean (alte Datei intakt), Profile-0 read-only enforced, schema-violation rejected. AST snapshot/reload round-trip mit valid+invalid `clean_shutdown_flag`. |
| **Blocks** | `S3` (MM braucht DB-Lookup), `S6` (CGIs lesen DB). |
| **BlockedBy** | `S0`. |
| **Effort** | M |

---

### S6 — `S6-sw-webui-cgis` — CGI binaries + cgi_common

| Field | Value |
|---|---|
| **Side** | SW |
| **Scope** | Alle CGI-Binaries laut `docs/OPERATIONS.md` §1–§9. Jeder CGI ist Thin-Client: parse query/body, JSON-Envelope (§6 Wire-Format, length-prefixed JSON über `/run/tetra_d.sock`), socket round-trip, emit `Content-Type: application/json` + Body. SSE-Streaming-Helper für `*_stream`-Ops (§3 Debug). cgi_common bündelt Auth-Slot (§7), Envelope-Builder, Socket-Connect+Length-Prefix. |
| **Deliverables** | `sw/webui/cgi_common.c` (~400 LOC) + Header, plus 1 Binary pro Top-Level-CGI: `status.cgi`, `profiles.cgi`, `entities.cgi`, `sessions.cgi`, `db.cgi`, `policy.cgi`, `debug.cgi`, `config.cgi`, `apply.cgi`, `tools.cgi`, `jobs.cgi`, `stop.cgi` — je ~150 LOC = 12 × ~150 LOC = ~1800 LOC. |
| **Interface contract** | CGIs ↔ Daemon Wire: §6 envelope mit `op` ∈ Tabellen §1–§5. Locked: `IF_WEBUI_WIRE_v1` (= OPERATIONS.md §6 + dotted op names). |
| **Inputs consumed** | `IF_CORE_API_v1`, `IF_DB_API_v1`, OPERATIONS.md §1–§9 (alle Endpoint-Tabellen + Error-Codes + Async-Job-Lifecycle). HARDWARE.md §8 (busybox-httpd-CGI-Konvention, 3-s-Timeout, SSE-Bypass). |
| **Test gate** | `tb/sw/test_cgi_common.c` (Unity): envelope round-trip, length-prefix-frame, error-code-shape. Plus per-CGI shell-test `tb/sw/cgi_smoke.sh` = curl-vs-mocked-daemon-socket Roundtrip aller Ops. SSE-Stream: open → 3 events → close, no leak. |
| **Blocks** | Phase 4 (operator UI). |
| **BlockedBy** | `S0`, `S5`, `S7` (Op-Names locked am Daemon). |
| **Effort** | L |

---

### S7 — `S7-sw-tetra-d` — tetra_d.c main daemon + dispatcher

| Field | Value |
|---|---|
| **Side** | SW |
| **Scope** | Main daemon process: epoll-Loop über DMA-IRQ-FDs (S1) + Unix-Socket-Listener (`/run/tetra_d.sock`, mode 0660 root:tetra) + Signal-Handler. Op-Dispatcher (`<entity>.<verb>` → handler-table). Async-Job-Worker-Pool laut OPERATIONS.md §9. Wires alle SW-Layer (S2/S3/S4/S5) am gemeinsamen MsgBus zusammen. |
| **Deliverables** | `sw/tetra_d.c` (~600 LOC), `sw/webui/socket_handler.c` (~400 LOC), `sw/webui/op_dispatch.c` (~500 LOC, op→handler table), `sw/webui/jobs.c` (~400 LOC, async-job-pool). systemd unit `deploy/tetra_d.service`. |
| **Interface contract** | Defines daemon-side of `IF_WEBUI_WIRE_v1`: full op-handler-table mit op-names laut OPERATIONS.md §1–§5. Daemon emits CLI on `--help`. Locked: `IF_DAEMON_OPS_v1`. |
| **Inputs consumed** | `IF_CORE_API_v1`, `IF_DMA_API_v1`, `IF_LLC_v1`, `IF_MLE_v1`, `IF_MM_v1`, `IF_CMCE_v1`, `IF_DB_API_v1`, `IF_AST_PERSIST_v1`, OPERATIONS.md §6+§9. |
| **Test gate** | `tb/sw/test_daemon_smoke.c` (host-cross-compile run): start daemon mit DMA-Mock + DB-Mock, drive 5 representative ops (`status.summary`, `profile.list`, `profile.put`, `tools.pdu_send`-dry_run, `daemon.stop`) → all return ok-envelope. Clean-shutdown → AST snapshot + flag set. |
| **Blocks** | Phase 3 cosim, Phase 4 live. |
| **BlockedBy** | `S0`, `S1`, `S2`, `S3`, `S4`, `S5`. |
| **Effort** | L |

---

### T1 — `T1-tb-rtl` — RTL TBs per new RTL block

| Field | Value |
|---|---|
| **Side** | Test |
| **Scope** | Schreibt + pflegt die per-block TBs für ALLE neu eingeführten RTL-Module: `tb_axi_dma_wrapper.v`, `tb_tmasap_framer.v`, `tb_tmdsap_framer.v`, `tb_cdc_async_fifo.v`, `tb_tetra_top.v`. Plus reusable BFM-Lib (AXI-Lite, AXIS, AXI-MM). Carry-over UMAC-TBs werden NICHT von T1 berührt — die laufen weiter unverändert (gehören semantisch zu A6). |
| **Deliverables** | `tb/rtl/bfm/axi_lite_bfm.v` (~250 LOC), `tb/rtl/bfm/axis_bfm.v` (~200 LOC), 5 TBs siehe Scope, `tb/rtl/Makefile` (iverilog-driven). |
| **Interface contract** | TB-Modul-Templates. Kein code-level Interface — nur Datei-Pfade. T1 OWNS die TB-Files, nicht den DUT. |
| **Inputs consumed** | DUT-Ports von `IF_AXIDMA_v1`, `IF_TMASAP_FRAME_v1`, `IF_TMDSAP_FRAME_v1`, `IF_CDC_AXIS_v1`, `IF_TETRA_TOP_v1`. Reference-Bit-Vektoren aus `docs/references/` für Bit-Exact-Vergleich. |
| **Test gate** | `make tb` grün auf iverilog 12.0 (HARDWARE.md §6). 0/N bit diff für die TBs die Gold-Ref-bound sind (siehe A2/A6 test gates). |
| **Blocks** | Merge-Gate für A1/A2/A3/A4/A5. |
| **BlockedBy** | A1..A5 Interface-Lock (DUT-Ports stabil). |
| **Effort** | L |

---

### T2 — `T2-cosim-verilator` — Verilator + C-binary cosim harness

| Field | Value |
|---|---|
| **Side** | Test |
| **Scope** | Phase-3 Co-Simulation: Verilator-Wrap von `tetra_top.v` + AXIS-Stub für AD9361-Sample-Stream + Linkup mit kompiliertem `tetra_d`-Binary über shared-memory-DMA-Mock. Drives Gold-Ref-UL-Bursts als Stimulus, pipes TmaSap-RX in real C-daemon, daemon's TmaSap-TX zurück in FPGA, DL-Output diff'd gegen Gold-Ref-DL. |
| **Deliverables** | `tb/cosim/cosim_top.cpp` (Verilator harness, ~800 LOC), `tb/cosim/dma_shm_bridge.c` (~300 LOC), `tb/cosim/scenarios/m2_attach.c` (~200 LOC, Gold-Ref-driven), `tb/cosim/scenarios/group_attach.c`, `tb/cosim/scenarios/d_nwrk_broadcast.c`, `tb/cosim/Makefile`. |
| **Interface contract** | Bridge: shared-memory ring matches `IF_DMA_API_v1` semantics so daemon binary läuft unmodifiziert. Cosim entry: `make cosim SCENARIO=<name>`. |
| **Inputs consumed** | `IF_TETRA_TOP_v1` (DUT), `IF_DMA_API_v1` (daemon-side), Gold-Ref-WAV-Stimuli aus `wavs/gold_*`. |
| **Test gate** | Phase-3 acceptance: alle 3 Scenarios green (M2-Attach 0/432 bit diff DL#727/DL#735, Group-Attach 0/124 bit diff D-ATTACH-DETACH-GRP-ID-ACK, D-NWRK-BCAST byte-identisch zu Burst #423). |
| **Blocks** | Phase 4 entry. |
| **BlockedBy** | `A5`, `S7`. (Echte HW nicht nötig.) |
| **Effort** | L |

---

### T3 — `T3-tb-sw-host` — Host x86 SW unit-tests + decoder helpers

| Field | Value |
|---|---|
| **Side** | Test |
| **Scope** | Host-Cross-Compile-Stack (gleicher Code, x86-Build) für SW-Unit-Tests. Unity vendoring (HARDWARE.md §5), build-system-glue, plus die Python-Decoder-Helper, die SW-TBs anfüttern: bestehende `scripts/decode_*.py` werden NICHT geändert, aber neue Helper `scripts/gen_tmasap_frame_ref.py` (Reference-Frame-Generator für A2-TB) und `scripts/gen_d_nwrk_broadcast.py` (existiert bereits — re-use für S4-TB). |
| **Deliverables** | `sw/external/unity/` (vendored, pinned), `tb/sw/Makefile` (host build), `tb/sw/unity_runner.c`, neue Scripts unter `scripts/gen_*.py` wo SW-TBs Reference-Bit-Vektoren brauchen, CI-helper `scripts/run_sw_tests.sh`. |
| **Interface contract** | `make sw-test` aus Repo-Root. Per-Modul TB-Targets `sw-test-llc`, `sw-test-mm`, … |
| **Inputs consumed** | Alle SW-Module-Quellen (S0..S7). |
| **Test gate** | `make sw-test` grün auf Ubuntu 24.04 GCC 13.3 (HARDWARE.md §5). Alle TBs aus S0..S7 gehen über T3 — T3 ist Infrastruktur, blockt aber nichts inhaltlich, nur den Build-Pfad. |
| **Blocks** | Merge-Gate für SW-Module (S0..S7 brauchen Unity-Build-Glue). |
| **BlockedBy** | — (kann sofort starten, vendoring + Makefile sind unabhängig). |
| **Effort** | M |

---

### A. Interface-locking schedule

Diese Verträge MÜSSEN schriftlich fixiert sein, bevor parallele Arbeit
sinnvoll losläuft. Reihenfolge der Locks (= "Day 0" der Phase 2):

| # | Interface name | Owned by | Consumers | Notes |
|---|---|---|---|---|
| 1 | `IF_CORE_API_v1` | `S0` | `S1`, `S2`, `S3`, `S4`, `S5`, `S6`, `S7` | Header-Only-Lock, `sw/core/include/tetra/*.h` muss compile-clean sein |
| 2 | `IF_DB_API_v1` + `IF_AST_PERSIST_v1` | `S5` | `S3`, `S6`, `S7` | Headerset; Implementation darf später nachziehen |
| 3 | `IF_LLC_v1` | `S2` | `S3`, `S4`, `S7` | Bluestation-1:1, kein Layer-Schnitt-Streit erwartet |
| 4 | `IF_MLE_v1` + `IF_MM_v1` | `S3` | `S6`, `S7` | TleSap zwischen den beiden ist Bluestation-1:1 |
| 5 | `IF_CMCE_v1` | `S4` | `S7` | inkl. periodic-driver-Hook für D-NWRK-BCAST |
| 6 | `IF_WEBUI_WIRE_v1` + `IF_DAEMON_OPS_v1` | `S7` (+ OPERATIONS.md) | `S6` | dotted op-names sind Source-of-truth in OPERATIONS.md §1–§5 |
| 7 | `IF_DMA_API_v1` | `S1` | `S7`, `T2` | Userspace-API über `xilinx_axidma`, Channel-Enum locked |
| 8 | `IF_AXIDMA_v1` | `A1` | `A2`, `A3`, `A5`, `T1`, `S1` (DT) | AXIS-Naming + DT-Overlay-Pfade |
| 9 | `IF_CDC_AXIS_v1` | `A4` | `A1`, `A5`, `T1` | async-FIFO-Ports |
| 10 | `IF_TMASAP_FRAME_v1` | `A2` | `A5`, `T1`, `T2` | Byte-Layout = ARCHITECTURE.md verbatim |
| 11 | `IF_TMDSAP_FRAME_v1` | `A3` | `A5`, `T1`, `T2` | dito |
| 12 | `IF_UMAC_TMASAP_v1` | `A6` | `A2` | UMAC-Carry-Over-Ports nach Cleanup |
| 13 | `IF_TETRA_TOP_v1` | `A5` | `T1`, `T2` | Top-Level-Pinout + AXI-Lite-Window. Locked 2026-05-03: AD9361 LVDS pins (rx/tx_clk/frame/data + control) verbatim from carry-over xdc; AXI-Lite slave (12-bit addr, 32-bit data); 4× AXI4-MM master sets (m_axi_tma_{rx,tx}_*, m_axi_tmd_{rx,tx}_*); 4× IRQ outputs (irq_tma_{rx,tx}_o, irq_tmd_{rx,tx}_o); clk_axi + clk_sys + rstn_axi + rstn_sys clock/reset pair. Header comment in `rtl/tetra_top.v` is the source-of-truth. |

Locks #1, #8, #9 (foundation interfaces) müssen zuerst stehen — sie blocken am
meisten. Locks #6 + #13 sind die letzten, weil sie alle anderen aggregieren.

---

### B. Dependency graph (blocks → blockedBy DAG)

```
                        ┌──────────────────────────────────────────────┐
                        │                                              │
   [A6 umac-carryover]──┘                                              │
            │                                                          │
            ▼                                                          │
   [A1 axi-dma]──┬──[A4 cdc] (parallel; A4 has no upstream)            │
        │       │                                                     │
        ▼       ▼                                                     │
   [A2 tma-framer]──[A3 tmd-framer]                                   │
        │       │                                                    │
        └───┬───┘                                                    │
            ▼                                                        │
   [A5 tetra_top + xdc + cleanup] ──────────┐                        │
            │                               │                        │
            ▼                               ▼                        │
   [T1 rtl-tb merge gate]             [T2 cosim] ◄───────┐           │
                                            ▲           │           │
                                            │           │           │
   ─── SW side ───────────────────────────────────────  │           │
                                                        │           │
   [S0 core+msgbus]──┬──[S5 persistence]                │           │
        │            │       │                          │           │
        │            ▼       ▼                          │           │
        │       [S2 llc]──[S3 mle+mm]──┐                │           │
        │            │        │        │                │           │
        │            ▼        ▼        ▼                │           │
        │       [S4 cmce]                                │           │
        │            │                                   │           │
        ▼            ▼                                   │           │
   [S1 dma-glue]──►[S7 tetra_d]──[S6 webui-cgis]─────────┘           │
        ▲             ▲                                              │
        │             │                                              │
        └── (A1 IF) ──┘                                              │
                                                                     │
   ─── Test infra (no dependents in graph; just gates) ──────────────┘
   [T3 host sw-test]   (gates merges of S0..S7)
   [T1 rtl tb]         (gates merges of A1..A5)
```

**Critical path** (Phase-2 longest chain to first cosim-green):

`S0 → S5 → S3 → S7 → T2-cosim` (SW chain)
parallel zu
`A6 → A1 → A2 → A5 → T2-cosim` (FPGA chain)

Beide Chains laufen tatsächlich parallel; die längere ist die SW-Chain weil
**S3** = `XL` und intern auf S5 plus S2 wartet. Damit ist S3 single-most-likely
Schedule-Risk-Item — es braucht Gold-Ref-Bit-Lookups für **jedes** D-LOC-UPDATE-
ACCEPT-Feld + Group-Attach-ACK.

**Cross-side join**: T2-cosim ist der erste Punkt wo FPGA-Chain und SW-Chain
zusammenkommen müssen. Davor laufen sie unabhängig.

**Overraschung gefunden**: `S6` (WebUI CGIs) ist NICHT auf der kritischen Pfad.
Die CGIs sind Thin-Clients mit einem stabilen Wire-Format (OPERATIONS.md §6),
also kann S6 starten sobald S7 die op-Liste fixiert (`IF_DAEMON_OPS_v1`) — die
Daemon-Implementation der einzelnen Ops kann nachziehen, weil CGIs mockable
sind. Operativ heißt das: S6 darf nach S7-Lock parallel zu S2/S3/S4 laufen.

---

### C. Sync-points / merge cadence

Vier Sync-Points zwischen Phase-2-Start und Phase-4-Live-A/B:

| # | Sync-Point | Trigger | Required artifact in `main` before next phase opens |
|---|---|---|---|
| 1 | **Interface-Lock-Day** | Day 0 of Phase 2 | Alle 13 Interface-Header (`IF_*`) committed, jede Datei mit Owning-Agent-ID im Comment-Header. Branch-Protection: kein agent-spezifischer Branch darf vor diesem Commit forken. |
| 2 | **Per-agent green-CI merge** | jeder agent einzeln, sobald sein Test-gate grün ist | Pro Agent ein Squash-Merge auf `main`. Vorbedingung: alle Test-gates aus dem Agent-Vertrag erfüllt. CI runs: `make tb` (RTL) + `make sw-test` (SW host) müssen grün sein. Reihenfolge folgt der DAG (B). |
| 3 | **Co-Sim Integration Sync** | wenn A5 + S7 gemerged sind | `make cosim SCENARIO=m2_attach` 0/432 bit diff DL#727 + DL#735, `SCENARIO=group_attach` 0/124 bit diff, `SCENARIO=d_nwrk_broadcast` byte-identisch Burst #423. Artifact: `build/cosim/results/*.diff` alle leer, gecheckt in CI. Erst dann öffnet Phase 4. |
| 4 | **Live-A/B-Sync auf Boards** | Phase 4 entry | Bitstream auf Board #1 geladen (`scripts/deploy.sh`), Board #2 als Sniffer aufgesetzt (HARDWARE.md §"Boards"). Acceptance gates per Phase 4 (M2 ≥3 cycles green, M3 ≥3 cycles green, DETACH-ACK AACH `0x0249`, D-NWRK-BCAST 10s cadence byte-identisch). MTP3550-Power-Cycle-Procedure ausgeführt (memory `feedback_announce_ms_restart`). |

**Merge-Gate Hard Rules (CLAUDE.md §Git "main = always green"):**

- Kein Merge wenn `make tb` rot.
- Kein Merge wenn `make sw-test` rot.
- Kein Merge wenn ein Bit-Exact-Test irgendwo > 0 bit diff zeigt.
- Kein Merge ohne Co-Authored-By: Claude `<noreply@anthropic.com>` für AI-assisted Commits.

**Was passiert wenn ein Sync-Point hängt:**

- Interface-Lock-Day rutscht → STOP, Kevin-Konsens vor Forken aller Branches.
- Co-Sim hängt > 1 Cycle → fällt auf den Bit-Exact-Test im Agent-TB zurück
  (Gold-Ref-Bit-Vektor sagt wer Recht hat, FPGA oder SW).
- Live-A/B hängt → Cold-Rollback laut MIGRATION_PLAN.md §"Rollback Strategy".

### D. Pre-Phase-2 Architektur-Entscheidungen (Kevin, 2026-05-03)

Drei offene Punkte aus den Agent-Topology-Reports wurden vor Phase-2-Start
mit Kevin entschieden:

| # | Frage | Entscheidung | Konsequenz |
|---|---|---|---|
| 1 | `libaxidma`-Strategie | **Option B** — Jacob Feder `xilinx_axidma` (MIT) vendored unter `sw/external/xilinx_axidma/` at pinned commit, Kernel-Modul rebuild gegen Board-Kernel 5.10. | S1-Agent (`S1-sw-dma-glue`) baut C-Glue-Layer um `libaxidma.so`. HARDWARE.md §10 follow-up `libaxidma` jetzt geschlossen. |
| 2 | AXI-Register-Window-Naming für `tetra_top.v` | **Sofortiger Spec-Pass** in `docs/ARCHITECTURE.md` vor A5-Start. Eigener Pre-Phase-2-Agent legt das Register-Window fest (Adressen + Namen + Bit-Felder + Reset-Defaults). | Schließt die 12 `<!-- TODO: confirm reg name -->` Marker in `OPERATIONS.md §4`. A5 + S6 + S7 referenzieren benannte Register von Tag 0. |
| 3 | Verilator+libaxidma shm-DMA-Bridge für T2-Cosim | **Bundle in T2-Scope** — kein eigener Pre-Spike. Falls Bridge scheitert, fallback auf Verilator-only-TB ohne C-Daemon-Loop, Phase-4-Live-A/B fängt das auf. | T2-Agent-Vertrag erweitert um Risk-Note + Fallback. Kein Schedule-Impact wenn Bridge klappt. |

---

## Migration Phases

### Phase 0 — Project skeleton (this commit)

- [x] Repo structure created
- [x] Documentation skeleton
- [x] Migration plan with locked decisions
- [x] Carry-over of validated FPGA modules (PHY + LMAC channel-coding + UMAC reassembly)
- [x] Reference materials migrated from old project

### Phase 1 — Planning completion (TODO-A through TODO-D)

Run as separate sessions. Each TODO is its own context to avoid quality drop.

- [x] TODO-A: Gold-Ref `Todo`-Felder extraction (`docs/references/gold_field_values.md`, 2026-05-03)
- [x] TODO-B: WebUI spec (`docs/OPERATIONS.md` §"WebUI", 2026-05-03)
- [x] TODO-C: Agent topology (`docs/MIGRATION_PLAN.md` §"Agent Topology", 2026-05-03)
- [x] TODO-D: Toolchain audit (`docs/HARDWARE.md` §Toolchain + Dependencies, 2026-05-03)

### Phase 2 — Big-Bang implementation (parallel agents)  ✅ COMPLETE 2026-05-03

Per topology in TODO-C. All 16 agents shipped + merged on `main`:
A1/A2/A3/A4/A5/A6 (FPGA), S0/S1/S2/S3/S4/S5/S6/S7 (SW), T0/T1/T2 (test).
Test gate: 24 RTL TBs + 155 SW unit-tests all green.

### Phase 3 — Co-Simulation  ✅ COMPLETE 2026-05-03

Verilator 5.020 elaborates `Vtetra_top` with the behavioural axi_dma model;
harness drives `tetra_tmasap_tx_framer` end-to-end via `tb_inject_*` ports
(gated by `\`ifdef TETRA_TOP_NO_PHY` + `\`ifdef COSIM_TBINJECT`, no production
synth impact). All three §T2 scenarios reach 0-bit diff:

| Scenario | Cycles | Bit-diff |
|---|---|---|
| `m2_attach` | 103 | 0/432 |
| `group_attach` | 53 | 0/124 |
| `d_nwrk_broadcast` | 53 | 0/124 |

Daemon-in-loop variant (real `tetra_d` over shm bridge) is Phase-4 fold-back
work — `sw/dma_io/dma_io.c` needs `\`ifdef HAVE_COSIM_SHM` block; UMAC
reassembly chain has to come online in cosim (currently stubbed under
`TETRA_TOP_NO_PHY`).

### Phase 3.5 — Hardware bring-up (open blocker before Phase 4) 🔴

Vivado synth pipeline (`make synth` → `scripts/build/synth.tcl`) is wired
end-to-end and pins the `axi_dma:7.1` IP config (SG=1, S2MM_DRE=1,
addr_width=32, data_width=32 per channel). The IP gets created OK and
its OOC synth completes. **`synth_design` then fails** at top-level
elaboration because `rtl/infra/tetra_axi_dma_wrapper.v` instantiates
`axi_dma_channel_inst` with a slim port-list + custom parameters
(`CHANNEL_ID`, `DIR_IS_S2MM`, slim AXIS s/m + slim AXI4-MM r/w +
`irq_done`/`frame_count`/`overrun_pulse`/`underrun_pulse`) that match
the simulation behavioural model `tb/rtl/models/axi_dma_v7_1_bhv.v`.
The real LogiCORE IP exposes the full port-list (S_AXI_LITE control,
separate SG master, mm2s/s2mm_introut, full burst signals) and accepts
none of those parameters.

**Open architectural decision required (not autonomous):** add a
synthesis-only shim `rtl/infra/ip/axi_dma_channel_inst.v` that exposes
the slim port/param shape externally and internally instantiates the
real `axi_dma:7.1` IP. Two design choices are roughly equivalent for
us (4–8h work either way):

- **Shim option A — simple-mode (no SG).** Re-config the IP with
  `c_include_sg=0`. Shim drives DMACR/DMASR/SA/DA/LENGTH per-transfer
  via an internal AXI-Lite master, fed from a small descriptor BRAM
  the shim manages. Pro: no DDR descriptor writes, no SG-engine state
  machine. Contra: re-program registers per burst, burst-throughput
  tax.
- **Shim option B — keep SG-mode, manage descriptors in BRAM.** Shim
  contains a tiny descriptor manager + ring of N descriptors in BRAM,
  tied to the IP's M_AXI_SG. Pro: classic Xilinx flow, IRQ-on-completion
  handles itself. Contra: more state machine, BRAM cost.

Until one is picked + implemented, `make synth` fails fast with the
exact error logged in `scripts/build/synth.tcl` TODO block + the runme.log
under `build/vivado/tetra_bs.runs/synth_1/`. **Not a blocker for Phase
0–3 development**; only gates Phase 4 (live A/B needs a bitstream).

### Phase 4 — Live A/B on Boards

Board #1: new bitstream + SW. Board #2: sniffer captures Board #1 output and
diffs against Gold-Ref. MTP3550 attaches against Board #1.

Acceptance gates per Gold-Ref scenario:
- M2 ITSI-Attach: ≥3 cycles green, 0/432 bit diff vs Gold-Ref DL#727 + DL#735
- M3 Group-Attach: ≥3 cycles green, 0/124 bit diff vs Gold-Ref D-ATTACH-DETACH-GRP-ID-ACK
- DETACH-ACK: AACH stays `0x0249`, LI=6 + LI=6 pair correct
- D-NWRK-BROADCAST: 10s cadence, byte-identical to Gold-Ref Burst #423
- Group-Call: TBD when CMCE captures bit-extracted (TODO-A)

### Phase 5 — Production cutover

Document deployment, write operator guide, hand off.

---

## Rollback Strategy

Big-Bang has no hot rollback. Cold rollback only:
- Old project `tetra-zynq-phy` remains as read-only reference
- Board #1 can be reflashed with last-good `tetra-zynq-phy` bitstream
- WAVs and decoder scripts are common to both projects (copied here)

---

## Source Material Inventory

### From old project (carried over)

- All `rtl/phy/*` (RX/TX frontend, AD9361 interface, modulator, demodulator)
- `rtl/lmac/` channel-coding modules (Viterbi, RCPC, RM, scrambler, interleaver, CRC, AACH-Encoder)
- `rtl/umac/` reassembly + MAC-ACCESS parser + DL signal queue/scheduler + SCH/F encoder + MAC-RESOURCE builder
- `constraints/libresdr_tetra.xdc`
- `scripts/decode_*.py` (decoders for verification)
- `scripts/deploy.sh` (adapted for new SW structure)
- Gold-reference WAVs (large files, kept in `wavs/` but excluded from git via .gitignore)
- Reference memories migrated to `docs/references/`

### From bluestation (read-only inspiration)

- Layer architecture (`tetra-entities/src/{phy,lmac,umac,llc,mle,mm,cmce,sndcp}/`)
- SAP definitions (`tetra-saps/src/{tp,tmv,tma,tmd,tla,tle,tlmb,tlmc,tnmm}/`)
- Common types (`tetra-core/src/{address.rs, sap_fields.rs, tdma_time.rs, ...}`)
- ETSI clause references in source comments

### From ETSI (tie-breaker)

- EN 300 392-2 (V+D Air Interface)
- See `docs/references/` for PDFs
