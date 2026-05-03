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

## Migration Phases

### Phase 0 — Project skeleton (this commit)

- [x] Repo structure created
- [x] Documentation skeleton
- [x] Migration plan with locked decisions
- [x] Carry-over of validated FPGA modules (PHY + LMAC channel-coding + UMAC reassembly)
- [x] Reference materials migrated from old project

### Phase 1 — Planning completion (TODO-A through TODO-D)

Run as separate sessions. Each TODO is its own context to avoid quality drop.

- [ ] TODO-A: Gold-Ref `Todo`-Felder extraction
- [ ] TODO-B: WebUI spec
- [ ] TODO-C: Agent topology
- [x] TODO-D: Toolchain audit (`docs/HARDWARE.md` §Toolchain + Dependencies, 2026-05-03)

### Phase 2 — Big-Bang implementation (parallel agents)

Per topology in TODO-C. Sub-agents work concurrently with declared interfaces.

### Phase 3 — Co-Simulation

Verilator wrap of FPGA top, drives gold-ref UL-bursts as stimulus, pipes
TmaSap-RX into real C daemon, daemon's TmaSap-TX back to FPGA, DL-output
diff'd against Gold-Ref DL.

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
