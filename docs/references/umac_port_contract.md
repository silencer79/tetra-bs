---
name: UMAC carry-over port contract — IF_UMAC_TMASAP_v1
description: Bit-width + handshake-protocol contract of every carry-over UMAC RTL module (rtl/umac/*.v) that the A2 TmaSap framer must lock its ports against. Output of A6 (UMAC carry-over Re-Validation). Locks 2026-05-03.
type: reference
---

# UMAC carry-over port contract

This document is the formal A2-blocker output of agent **A6**
(`A6-fpga-umac-carryover`).  It enumerates the I/O signals — bit width,
direction, semantics, handshake protocol — of every carry-over UMAC RTL block
that the A2 TmaSap framer (`A2-fpga-tmasap-framer`) must wire against.

The contract is named `IF_UMAC_TMASAP_v1` per the interface-locking schedule
in `docs/MIGRATION_PLAN.md` §"A. Interface-locking schedule" entry #12.

**Source-of-truth:** the carry-over RTL files themselves are authoritative
(`rtl/umac/tetra_*.v`).  This doc is a derived snapshot for cross-agent
coordination; if a port name disagrees, the RTL wins and this doc is wrong.

**Verilog-2001 only.**  Every module compiles clean under
`iverilog -g2001 -t null` standalone (verified 2026-05-03).
No SystemVerilog constructs.

**Carry-over RTL files** (no logic change in A6 — only re-validation):

| File | Module | Role |
|---|---|---|
| `rtl/umac/tetra_ul_demand_reassembly.v` | `tetra_ul_demand_reassembly` | UL Frag-1+Frag-2 → 129-bit MM body for A2's TmaSap-RX |
| `rtl/umac/tetra_ul_mac_access_parser.v` | `tetra_ul_mac_access_parser` | UL MAC-ACCESS PDU field extractor (feeds reassembly) |
| `rtl/umac/tetra_dl_signal_queue.v` | `tetra_dl_signal_queue` | DL SCH/F+SCH/HD coded-PDU queue (DEPTH=4, prio MLE>CMCE>SDS) |
| `rtl/umac/tetra_dl_signal_scheduler.v` | `tetra_dl_signal_scheduler` | One-frame-ahead DL slot arbiter (TN=3 trigger) |
| `rtl/umac/tetra_mac_resource_dl_builder.v` | `tetra_mac_resource_dl_builder` | TM-SDU + header → 268-bit MAC-RESOURCE DL PDU |
| `rtl/umac/tetra_mac_resource_bl_ack_builder.v` | `tetra_mac_resource_bl_ack_builder` | Standalone BL-ACK MAC-RESOURCE builder |
| `rtl/umac/tetra_lmac.v` | `tetra_lmac` | LMAC container — NOT directly TmaSap-bound, listed for completeness |

---

## Common conventions

- **Bit ordering.** Multi-bit buses are MSB-first **unless otherwise noted**.
  For a bus `wire [W-1:0] x`, `x[W-1]` is the first on-air bit (the bit
  decoded earliest by the deinterleaver).  This convention is consistent
  across all UMAC modules and matches `rtl/lmac/tetra_sch_f_encoder.v`
  (`coded_bits[431]` = first on-air bit) and
  `rtl/lmac/tetra_sch_hd_encoder.v`.
- **Resets.** Every UMAC block uses **active-low asynchronous reset**
  (`rst_n` or `rst_n_sys`).  Hold low for at least 4 cycles before driving
  inputs.
- **Clock.** Single clock per module — either `clk_sys` (PHY-side, AD9361-
  derived) or `clk` (PS-side AXI).  All UMAC blocks listed are `clk_sys`-
  domain except the queue + scheduler + builders, which are `clk_sys`
  (per their `clk_sys`/`rst_n_sys` ports).  `tetra_dl_signal_queue` and
  `tetra_mac_resource_*_builder` use `clk`/`rst_n` (no `_sys` suffix) for
  legacy reasons; the RTL is functionally equivalent.  See
  `rtl/umac/tetra_dl_signal_queue.v:38-45` for the rationale.
- **Pulse semantics.**  Outputs documented as "1-cycle pulse" assert HIGH for
  exactly one `clk` cycle on the event edge and are unconditionally cleared
  the next cycle.  Caller must sample on the cycle the pulse is HIGH; no
  sticky-handshake is provided.  `pdu_valid_sys`, `coded_valid`,
  `reassembled_valid_sys`, `valid` all follow this convention.
- **Counters.**  `*_count_*` outputs are sticky monotonic counters that
  saturate at `16'hFFFF` (no wrap).  Used for AXI-Lite debug-register reads.

---

## 1. `tetra_ul_demand_reassembly`

UL Frag-1 (MAC-ACCESS frag=1, 44-bit fragment) + Frag-2 (MAC-END-HU,
85-bit continuation) → 129-bit reassembled MM body.  The output is the
direct input to A2's TmaSap-RX framer.

| Direction | Width | Name | Semantics |
|---|---|---|---|
| in  | 1   | `clk_sys`              | system clock |
| in  | 1   | `rst_n_sys`            | async active-low reset |
| in  | 4   | `t0_frames_sys`        | T0 timeout in frames; 0 → `T0_FRAMES_DEFAULT`=2 |
| in  | 1   | `frame_tick_sys`       | 1-cycle pulse per TDMA frame (~56.67 ms) |
| in  | 1   | `frag1_pulse_sys`      | 1-cycle pulse: MAC-ACCESS frag=1 latched (= `pdu_valid_sys & ul_frag_flag_sys` from parser) |
| in  | 24  | `frag1_ssi_sys`        | SSI of the fragment-1 PDU |
| in  | 44  | `frag1_bits_sys`       | UL#0 bits[48..91], MSB-first (bit 43 = first on-air bit of the fragment) |
| in  | 1   | `end_hu_pulse_sys`     | 1-cycle pulse: MAC-END-HU continuation latched (= `ul_continuation_valid_sys`) |
| in  | 24  | `end_hu_ssi_sys`       | SSI carried by the parser (last MAC-ACCESS frag=1 SSI on the slot) |
| in  | 85  | `end_hu_bits_sys`      | UL#1 bits[7..91], MSB-first (bit 84 = first on-air bit of the continuation) |
| **out** | 1   | `reassembled_valid_sys`  | **1-cycle pulse: 129-bit body ready (A2-RX trigger)** |
| **out** | 129 | `reassembled_body_sys`   | **MM body, MSB-first (bit 128 = first on-air bit of the body, = UL#0 bit 48)** |
| **out** | 24  | `reassembled_ssi_sys`    | **SSI of the joined PDU (A2 forwards as `main_address.ssi`)** |
| out | 16  | `reassembled_cnt_sys`  | sticky reassembly counter (saturates at 0xFFFF) |
| out | 16  | `drop_cnt_sys`         | sticky drop counter (T0 timeout or 2-slot full) |
| out | 2   | `busy_slots_sys`       | one-hot: occupied 2-slot buffer view (debug) |

**Buffer.** 2-slot in-flight, register-based (no BRAM).  Same-SSI Frag-1
re-arrival replaces in-place (no drop).  T0 default = 2 frames (≈113 ms,
ETSI tolerated).

**Bit-exact gate.** `tb/rtl/tb_ul_demand_reassembly` proves 0/129 diff against
Gold-Ref M2 vector and 0/129 diff against MTP3550 vector.

**A2 wiring.** A2's TmaSap-RX framer consumes the **bold** outputs above
(`reassembled_valid_sys` as the frame-trigger, `reassembled_body_sys` as the
TMAS-frame payload, `reassembled_ssi_sys` as `main_address.ssi`).  Bit-width
must match exactly.

---

## 2. `tetra_ul_mac_access_parser`

UL MAC-ACCESS PDU field extractor (ETSI EN 300 392-2 §21.4.3.3).  Consumes
the 92-bit info-bus delivered by the SCH/HU decoder (1-cycle pulse with
CRC-OK), splits into MAC-ACCESS (mac_pdu_type=0) and MAC-END-HU
(mac_pdu_type=1) paths, exposes per-field outputs.

| Direction | Width | Name | Semantics |
|---|---|---|---|
| in  | 1   | `clk_sys`              | system clock |
| in  | 1   | `rst_n_sys`            | async active-low reset |
| in  | 92  | `info_bits_sys`        | SCH/HU info bits, `[0]` = first on-air bit (parser's MSB-first convention; **note: this module bucks the global "[W-1]=first" rule** because the SCH/HU decoder hands bits LSB-aligned at index 0). |
| in  | 1   | `info_valid_sys`       | 1-cycle pulse: new 92-bit block ready |
| in  | 1   | `crc_ok_sys`           | block CRC-OK (gates output update; if 0, parser ignores the block) |
| out | 1   | `pdu_type_sys`         | bit\[0\]: 0=MAC-ACCESS, 1=MAC-END-HU |
| out | 1   | `fill_bit_sys`         | bit\[1\]: fill_bit |
| out | 1   | `encryption_mode_sys`  | bit\[2\]: encryption flag |
| out | 2   | `ul_addr_type_sys`     | bits\[3..4\]: 00=Ssi/ISSI, 01=EventLabel, 10=Ussi, 11=Smi |
| out | 24  | `ul_issi_sys`          | bits\[5..28\]: 24-bit address (SSI/Ussi/Smi) |
| out | 10  | `ul_event_label_sys`   | bits\[5..14\]: 10-bit EventLabel (when `addr_type==01`) |
| out | 1   | `optional_field_flag_sys` | bit\[29\] |
| out | 1   | `ul_frag_flag_sys`     | bit\[31\] when opt=1 & length_or_cap=1 (fragment continuation pending) |
| out | 4   | `ul_reservation_req_sys` | bits\[32..35\] when cap_req mode |
| out | 5   | `ul_length_ind_sys`    | bits\[31..35\] when length_ind mode |
| out | 4   | `mm_pdu_type_sys`      | direct-MM mm_pdu_type (TL-SDU\[0..3\]) — legacy compatibility |
| out | 3   | `loc_upd_type_sys`     | direct-MM loc_upd_type (TL-SDU\[4..6\]) — legacy |
| out | 92  | `raw_info_bits_sys`    | full 92-bit info as latched (debug + reassembly downstream) |
| out | 1   | `pdu_valid_sys`        | **1-cycle pulse: MAC-ACCESS PDU latched** (NOT pulsed for MAC-END-HU) |
| out | 16  | `pdu_count_sys`        | sticky count of MAC-ACCESS PDUs |
| out | 1   | `bl_ack_valid_sys`     | 1-cycle pulse: parsed PDU is LLC BL-ACK |
| out | 1   | `bl_ack_nr_sys`        | N(R) extracted from BL-ACK |
| out | 16  | `bl_ack_count_sys`     | sticky BL-ACK count |
| out | 1   | `ul_llc_is_bl_data_sys`| 1 = BL-DATA or BL-ADATA |
| out | 1   | `ul_llc_is_bl_ack_sys` | 1 = BL-ACK |
| out | 1   | `ul_llc_has_fcs_sys`   | LLC has_fcs bit |
| out | 1   | `ul_llc_ns_valid_sys`  | N(S) field valid |
| out | 1   | `ul_llc_ns_sys`        | N(S) value |
| out | 1   | `ul_llc_nr_valid_sys`  | N(R) field valid |
| out | 1   | `ul_llc_nr_sys`        | N(R) value |
| out | 1   | `ul_llc_is_mle_mm_sys` | 1 = LLC payload carries MLE PD=001 (MM) |
| out | 4   | `ul_llc_mm_pdu_type_sys` | MM pdu_type wrapped in LLC BL-DATA/ADATA |
| out | 3   | `ul_llc_mm_loc_upd_type_sys` | MM body[0..2] when MM=2 (U-LOC-UPDATE-DEMAND) |
| out | 4   | `ul_llc_pdu_type_sys`  | raw 4-bit LLC pdu_type = {link_type, has_fcs, bl_pdu_type[1:0]} |
| out | 3   | `ul_mle_disc_sys`      | 3-bit MLE protocol discriminator |
| out | 1   | `ul_pdu_is_continuation_sys` | level signal: 1 = last latched PDU was MAC-END-HU |
| out | 1   | `ul_continuation_valid_sys` | **1-cycle pulse: MAC-END-HU latched (= reassembly's `end_hu_pulse_sys`)** |
| out | 85  | `ul_continuation_bits_sys`  | 85-bit fragment 2 (= `info_bits_sys[7..91]`, MSB-first) |
| out | 24  | `ul_continuation_ssi_sys`   | latched SSI of the most recent MAC-ACCESS frag=1 (passed through to reassembly) |
| out | 16  | `ul_continuation_count_sys` | sticky count of MAC-END-HU PDUs |

**A2 wiring.** A2 does NOT directly read the parser; it reads the
reassembly's output.  The parser's `pdu_valid_sys` may be exposed in TMAR
report frames for debug.

**Caveat — info_bits_sys bit-order.**  Unlike most UMAC buses,
`info_bits_sys[0]` is the *first on-air bit*.  This matches the SCH/HU
decoder upstream.  TBs and reassembly downstream both use this convention
internally.

---

## 3. `tetra_dl_signal_queue` (DEPTH=4)

Coded-PDU queue.  Three producer write ports (MLE / CMCE / SDS, strict
producer-priority), single consumer pop port.

| Direction | Width | Name | Semantics |
|---|---|---|---|
| in  | 1   | `clk`                  | clock |
| in  | 1   | `rst_n`                | async active-low reset |
| in  | 1   | `wr_mle_valid`         | MLE write request |
| in  | 432 | `wr_mle_coded`         | SCH/F (full 432) or SCH/HD (LSB-aligned in [215:0]) coded PDU |
| in  | 2   | `wr_mle_pdu_type`      | 00 = SCH_F, 01 = SCH_HD |
| in  | 2   | `wr_mle_target_tn`     | TN 0..3 |
| in  | 1   | `wr_mle_second_pdu_present` | telemetry: SCH/F carries concat BL-ACK PDU #2 |
| in  | 1   | `wr_mle_second_pdu_nr` | telemetry: BL-ACK NR bit |
| in  | …   | `wr_cmce_*`            | symmetric (no `second_pdu_*`); priority 1 |
| in  | …   | `wr_sds_*`             | symmetric; priority 2 |
| in  | 1   | `pop`                  | pop strobe — 1 cycle high while sampling head |
| out | 1   | `head_valid`           | 1 = combinational head present |
| out | 432 | `head_coded`           | head PDU coded bits |
| out | 2   | `head_pdu_type`        | 00=SCH_F, 01=SCH_HD |
| out | 2   | `head_target_tn`       | head TN |
| out | 2   | `head_prio`            | head priority (00=MLE, 01=CMCE, 10=SDS) |
| out | 1   | `head_second_pdu_present` | telemetry pass-through |
| out | 1   | `head_second_pdu_nr`   | telemetry pass-through |
| out | 4   | `depth_valid_mask`     | one-hot per slot |
| out | 3   | `depth_count`          | 0..4 |
| out | 16  | `drop_cnt`             | sticky (overflow + producer-collision losers) |

**Arbitration.**  Pop = strict-prio + tie-break = lower slot index.
Write = strict producer-prio MLE > CMCE > SDS; same-cycle losers count as
drops.  Drop on full queue = drop-newest.

**TB.**  `tb/rtl/tb_dl_signal_queue` — 15 PASS checks (priority order, FIFO
within prio, drop-on-full counter, producer-collision counter).

**A2 wiring.**  A2's TmaSap-TX framer pushes coded PDUs as the **MLE**
producer.  A2 must drive `wr_mle_*` only; CMCE/SDS ports are owned by S4 /
future.

---

## 4. `tetra_dl_signal_scheduler`

One-frame-ahead arbiter.  Triggers on `slot_pulse_sys && tn_sys==3`,
pops one PDU from the queue, drives the four per-TN signalling block bundles
consumed by `tetra_slot_content_mux`.

| Direction | Width | Name | Semantics |
|---|---|---|---|
| in  | 1   | `clk_sys`              | system clock |
| in  | 1   | `rst_n_sys`            | async active-low reset |
| in  | 2   | `tn_sys`               | current slot number 0..3 |
| in  | 1   | `slot_pulse_sys`       | 1-cycle pulse at slot start |
| out | 1   | `pop_sys`              | 1-cycle pop strobe to queue |
| in  | 1   | `head_valid_sys`       | (from queue) |
| in  | 432 | `head_coded_sys`       | (from queue) |
| in  | 2   | `head_pdu_type_sys`    | (from queue) |
| in  | 2   | `head_target_tn_sys`   | (from queue) |
| in  | 2   | `head_prio_sys`        | (from queue, kept for ILA visibility) |
| in  | 1   | `head_second_pdu_present_sys` | (from queue) |
| in  | 1   | `head_second_pdu_nr_sys` | (from queue) |
| out | 1   | `popped_second_pdu_present_sys` | latched per-pop telemetry |
| out | 1   | `popped_second_pdu_nr_sys` | latched per-pop telemetry |
| in  | 216 | `null_pdu_bits_sys`    | SCH/HD-coded NULL-PDU idle filler |
| in  | 216 | `sig_companion_sys`    | SYSINFO/BNCH companion half (SCH/HD slots) |
| out | 216 | `sched_blk1_tn0_sys`   | TN0 BKN1 |
| out | 216 | `sched_blk2_tn0_sys`   | TN0 BKN2 |
| out | 216 | `sched_blk1_tn1_sys`   | TN1 BKN1 |
| out | 216 | `sched_blk2_tn1_sys`   | TN1 BKN2 |
| out | 216 | `sched_blk1_tn2_sys`   | TN2 BKN1 |
| out | 216 | `sched_blk2_tn2_sys`   | TN2 BKN2 |
| out | 216 | `sched_blk1_tn3_sys`   | TN3 BKN1 |
| out | 216 | `sched_blk2_tn3_sys`   | TN3 BKN2 |
| out | 4   | `sched_ndb2_sys`       | per-TN NDB-flag bundle (1=SCH/HD, 0=SCH/F) |
| out | 4   | `sched_active_sys`     | per-TN one-hot "real PDU present this frame" |
| out | 16  | `override_cnt_sys`     | sticky — frames carrying real PDU |
| out | 16  | `pop_cnt_sys`          | sticky — pops issued |

**Trigger timing.** `pop_trigger = slot_pulse_sys && (tn_sys == 2'd3)` —
identical edge to the schedule-BRAM refresh in slot_content_mux.  Once per
frame.

**A2 wiring.** None — the scheduler outputs feed `tetra_slot_content_mux` in
the LMAC chain.  A2's TmaSap-TX framer interacts with the queue (input
side), not the scheduler (output side).

---

## 5. `tetra_mac_resource_dl_builder`

Wraps an MM PDU into a 268-bit MAC-RESOURCE DL PDU per ETSI §21.4.3.1 +
bluestation `mac_resource.rs::to_bitbuf`.  Supports concat second
MAC-RESOURCE PDU (Option B BL-ACK alongside Accept).

| Direction | Width | Name | Semantics |
|---|---|---|---|
| in  | 1   | `clk`                          | clock |
| in  | 1   | `rst_n`                        | async active-low reset |
| in  | 1   | `start`                        | 1-cycle start strobe |
| in  | 24  | `ssi`                          | MS SSI |
| in  | 3   | `addr_type`                    | 001=SSI, 011=USSI (other types not yet supported) |
| in  | 1   | `ns`                           | LLC N(S) |
| in  | 1   | `nr`                           | LLC N(R) |
| in  | 4   | `llc_pdu_type`                 | 0=BL-ADATA, 1=BL-DATA, 8=AL-SETUP, 14=L2SigPdu |
| in  | 1   | `random_access_flag`           | MAC-RESOURCE RandAccFlag |
| in  | 1   | `power_control_flag`           | mandatory presence flag |
| in  | 4   | `power_control_element`        | when pc_flag=1 |
| in  | 1   | `slot_granting_flag`           | mandatory presence flag |
| in  | 8   | `slot_granting_element`        | {cap_alloc[3:0], granting_delay[3:0]} when sg_flag=1 |
| in  | 1   | `chan_alloc_flag`              | mandatory presence flag |
| in  | 32  | `chan_alloc_element`           | left-aligned in chan_alloc_element_len bits |
| in  | 5   | `chan_alloc_element_len`       | 21..27 bits typical |
| in  | 1   | `second_pdu_valid`             | concat PDU #2 |
| in  | 6   | `second_pdu_length_ind`        | … |
| in  | 1   | `second_pdu_random_access_flag`| … |
| in  | 3   | `second_pdu_addr_type`         | … |
| in  | 24  | `second_pdu_ssi`               | … |
| in  | 80  | `second_pdu_tl_sdu`            | left-aligned, len in `second_pdu_tl_sdu_len` |
| in  | 7   | `second_pdu_tl_sdu_len`        | … |
| in  | 1   | `second_pdu_pc_flag`           | … |
| in  | 4   | `second_pdu_pc_element`        | … |
| in  | 1   | `second_pdu_sg_flag`           | … |
| in  | 8   | `second_pdu_sg_element`        | … |
| in  | 1   | `second_pdu_ca_flag`           | … |
| in  | 32  | `second_pdu_ca_element`        | … |
| in  | 5   | `second_pdu_ca_element_len`    | … |
| in  | 128 | `mm_pdu_bits`                  | left-aligned MM body, MSB at \[127\] |
| in  | 8   | `mm_pdu_len_bits`              | actual MM body length in bits |
| out | 268 | `pdu_bits`                     | **MAC-RESOURCE 268-bit PDU; \[267\] = first on-air bit** |
| out | 1   | `valid`                        | **1-cycle pulse: pdu_bits ready** |

**Latency.** ~6 cycles IDLE → ASSEMBLE_INNER → LLC_HEAD → MAC_HEAD → PAD → DONE.

**Bit-exact gate.** `tb/rtl/tb_mac_resource_dl_builder` proves 0/268 diff
against Python reference (`scripts/ref_mac_resource_dl.py`) for a Gold-Ref
DL#735-shaped scenario (SSI=0x282FF4, BL-ADATA, sg_flag=1, no second PDU,
80-bit MM body).

**A2 wiring.** A2's TmaSap-TX framer feeds this builder via:
- `mm_pdu_bits` ← TMAS frame payload (MSB-aligned, len in next field)
- `mm_pdu_len_bits` ← TMAS frame body length
- `ssi`, `nr`, `ns`, `random_access_flag`, all flag-element pairs ← TMAS
  meta-fields per ARCHITECTURE.md frame-format.
The builder's `pdu_bits` then feeds the SCH/F encoder (see §6).

---

## 6. `tetra_mac_resource_bl_ack_builder`

Standalone MAC-RESOURCE BL-ACK builder for the Accept-detached BL-ACK path
(used by some FSM states; the concat path in `tetra_mac_resource_dl_builder`
is preferred for Option B BL-ACK-alongside-Accept).

| Direction | Width | Name | Semantics |
|---|---|---|---|
| in  | 1   | `clk`                | clock |
| in  | 1   | `rst_n`              | async active-low reset |
| in  | 1   | `start`              | 1-cycle start strobe |
| in  | 24  | `ssi`                | MS SSI |
| in  | 3   | `addr_type`          | 001=SSI |
| in  | 1   | `random_access_flag` | RandAccFlag |
| in  | 1   | `nr`                 | LLC N(R) |
| out | 268 | `pdu_bits`           | 268-bit MAC-RESOURCE PDU |
| out | 1   | `valid`              | 1-cycle pulse |

Layout: 43-bit MAC header (LI=6) + 5-bit LLC BL-ACK = 48 bits = 6 octets,
zero-padded to 268 bits.

---

## 7. `tetra_lmac` — listed for completeness only

Container instantiating the LMAC channel-coding submodules
(`tetra_scrambler`, `tetra_deinterleaver`, `tetra_depuncture_r23`,
`tetra_viterbi_decoder`, `tetra_crc16`, `tetra_reed_muller`,
`tetra_steal_detect`, `tetra_rcpc_encoder`, `tetra_interleaver`).
**Not directly TmaSap-bound.**  A2 does not connect to `tetra_lmac` — the
TmaSap-RX path goes through the parser → reassembly → A2-RX framer, and
the TmaSap-TX path goes through A2-TX framer → `tetra_mac_resource_dl_builder`
→ `tetra_sch_f_encoder` → `tetra_dl_signal_queue` → scheduler → slot_content_mux
→ … → LMAC chain.

Listed here so A5 (top-level) knows the carry-over UMAC tree.

---

## A2 lock-down summary

The minimum A2-to-UMAC interface surface (the actual `IF_UMAC_TMASAP_v1`):

**TmaSap-RX path** (FPGA → SW):

```verilog
// from tetra_ul_demand_reassembly:
wire        umac_to_tmasap_rx_valid    = reassembled_valid_sys;
wire [128:0] umac_to_tmasap_rx_pdu     = reassembled_body_sys;
wire [10:0]  umac_to_tmasap_rx_pdu_len = 11'd129;        // fixed for U-LOC-UPDATE-DEMAND
wire [23:0]  umac_to_tmasap_rx_ssi     = reassembled_ssi_sys;
wire [2:0]   umac_to_tmasap_rx_ssi_type= 3'b000;         // SSI/ISSI
// + slot meta from a parallel signal source (TN, FN, MN, scrambling_code)
// and ready/valid handshake added by A2 framer.
```

**TmaSap-TX path** (SW → FPGA):

```verilog
// to tetra_mac_resource_dl_builder:
//    .ssi                   = tmasap_tx_ssi
//    .addr_type             = tmasap_tx_addr_type
//    .ns / .nr              = tmasap_tx_ns / tmasap_tx_nr
//    .llc_pdu_type          = tmasap_tx_llc_pdu_type
//    .random_access_flag    = tmasap_tx_ra_flag
//    .power_control_flag    = tmasap_tx_pc_flag (etc.)
//    .mm_pdu_bits           = tmasap_tx_mm_bits
//    .mm_pdu_len_bits       = tmasap_tx_mm_len
//    .start                 = tmasap_tx_start

// builder.pdu_bits → tetra_sch_f_encoder.info_bits
// encoder.coded_bits → tetra_dl_signal_queue.wr_mle_coded
// + wr_mle_pdu_type / wr_mle_target_tn / wr_mle_second_pdu_present from A2 frame meta.
```

A2 must NOT alter UMAC port widths or semantics.  Any change to
`IF_UMAC_TMASAP_v1` requires re-locking via Kevin and updating this doc.

---

## Verification status (2026-05-03)

| TB | Source | DUT | Bit-exact gate | Status |
|---|---|---|---|---|
| `tb_ul_demand_reassembly` | `tb/rtl/tb_ul_demand_reassembly/` | `tetra_ul_demand_reassembly` | 0/129 vs Gold-Ref M2 + MTP3550 | **PASS** |
| `tb_mac_access_parser`    | `tb/rtl/tb_mac_access_parser/`    | `tetra_ul_mac_access_parser` | 21 fields vs `reference_gold_attach_bitexact.md` UL#0 | **PASS** |
| `tb_dl_signal_queue`      | `tb/rtl/tb_dl_signal_queue/`      | `tetra_dl_signal_queue` | priority/FIFO/drop spec | **PASS** |
| `tb_dl_signal_scheduler`  | `tb/rtl/tb_dl_signal_scheduler/`  | `tetra_dl_signal_scheduler` | idle/active TN-target spec | **PASS** |
| `tb_sch_f_encoder`        | `tb/rtl/tb_sch_f_encoder/`        | `tetra_sch_f_encoder` (lmac, included for completeness) | 0/432 vs `scripts/ref_sch_f_encode.py` | **PASS** |
| `tb_mac_resource_dl_builder` | `tb/rtl/tb_mac_resource_dl_builder/` | `tetra_mac_resource_dl_builder` | 0/268 vs `scripts/ref_mac_resource_dl.py` (DL#735-shaped) | **PASS** |

Iverilog 12.0 (stable), `-g2001 -Wall`, exit-0 on PASS.

**No bit-bugs were found in the carry-over UMAC RTL during A6.**  All
modules that were verifiable against Gold-Ref bit-vectors (reassembly,
parser) reproduced the reference exactly.  No `<!-- BUG: -->` markers were
added.
