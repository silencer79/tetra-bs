---
name: Gold-Reference Feldwerte für bluestation Todo-Felder
description: Bit-exakte Werte für alle TmaSap-/CmceChanAllocReq-/MM-/CMCE-/MLE-Todo-Felder, extrahiert aus Gold-Ref-Captures vom 2026-04-25 / 2026-04-26 (externe BS @ 392.9875 MHz, MS-ISSI=0x282FF4) sowie aus dem Gold-Cell-Capture (MCC=262 MNC=1010 CC=1, GSSI=0x2F4D61). Begleitdokument zu TODO-A des Migration-Plans. Source-of-truth Hierarchie Gold > Bluestation > ETSI.
type: reference
---

# Gold-Reference Feldwerte für bluestation `Todo`-Felder

Dieses Dokument schließt TODO-A aus `docs/MIGRATION_PLAN.md`. Für jedes mit
`Todo` markierte Feld in den bluestation-Quellen (TmaSap, CmceChanAllocReq,
mm/u_location_update_demand, mm/d_location_update_accept, cmce/d_setup,
u_setup, d_call_proceeding, d_connect, u_tx_demand, d_tx_granted,
u/d_release, mle/d_nwrk_broadcast) wurde der Gold-Reference-Wert
bit-für-bit aus den vorhandenen Captures extrahiert.

## Source-of-truth Hierarchy (Reminder, CLAUDE.md §1)

1. **Gold-Reference Captures** — bit-genau verbindlich.
2. **Bluestation Rust stack** — Strukturvorlage. Bit-Layouts NUR wenn Gold-Ref schweigt.
3. **ETSI EN 300 392-2** — Tie-Breaker wenn Gold-Ref UND bluestation schweigen.

Bei Konflikt: STOP, melden, nicht raten.

## Captures used

| Capture | Coverage | Memo (`docs/references/`) |
|---|---|---|
| `wavs/gold_standard_380-393mhz/baseband_393084625Hz_*.wav` (DL) + `baseband_382468718Hz_*.wav` (UL) | M2 ITSI-Attach DL#727 + DL#735 + UL#0..UL#2 | `reference_gold_attach_bitexact.md` |
| `wavs/gold_standard_380-393mhz/GOLD_DL_ANMELDUNG_GRUPPENWECHSEL_GRUPPENRUF.wav` + `GOLD_UL_*.wav` | 6× ITSI-Attach + 3× Group-Attach + 3× DETACH-ACK + Cell-Pilot + 10× D-NWRK-BCAST + Group-Call (TCH-Phase im UL/DL) | `reference_gold_full_attach_timeline.md` |
| `wavs/.../UL_baseband_382891062Hz_*.wav` + `DL_baseband_392984000Hz_*.wav` (2026-04-26) | 5× D-ATTACH-DETACH-GRP-ID-ACK + matching U-ATTACH-DETACH-GRP-ID-DEMAND | `reference_group_attach_bitexact.md` |
| (derived/embedded) `scripts/gen_d_nwrk_broadcast.py:GOLD_INFO_124` | 124-bit info word of D-NWRK-BCAST Burst #423 | `reference_gold_full_attach_timeline.md` §"D-NWRK-BROADCAST-Cadence" |

**Gap:** Es gibt KEINEN bit-exakten Capture eines erfolgreichen Group-Calls
(CMCE-Phase: U-SETUP / D-SETUP / U-TX-DEMAND / D-TX-GRANTED / U-RELEASE /
D-RELEASE). Der CMCE-Block wird damit aus Bluestation-Defaults + ETSI
abgeleitet (siehe Caveat-Sektion unten).

---

## TmaSap (`tetra-saps/src/tma/mod.rs`)

TmaSap-Felder sind **SAP-internal**: sie werden zwischen MAC↔LLC innerhalb
des BS-Daemons übergeben. Sie erscheinen NICHT direkt auf-Air. Der korrekte
Wert ist daher derjenige, den der Daemon übergeben muss, damit die
resultierende MAC-PDU Gold-Ref-konform ist.

### `TmaUnitdataReq` (Clause 20.4.1.1.4) — LLC → MAC, send-path

| Field | Bluestation Type | Bit-width on-air | Gold-Ref Value | Captures observed | Source / Why |
|---|---|---|---|---|---|
| `req_handle` | `Todo` (i32) | SAP-internal | **opaque counter** (Daemon-frei wählbar) | n/a | Wird per `TmaReportInd.req_handle` zurück-korreliert. Kein Air-Bezug. Wir wählen `u32` monoton. |
| `pdu` | `BitBuffer` | LI×8 (variable, 7..21 oct in M2/M3) | aus PDU-Builder, siehe MM-/MLE-Sektionen unten | DL#727 7 oct, DL#735 21 oct, Group-Ack 16 oct, D-NWRK-BCAST 16 oct | `reference_gold_attach_bitexact.md` + `reference_group_attach_bitexact.md` |
| `main_address` | `TetraAddress` | 24+optional AE | für M2-Reply: `TetraAddress{ssi: 0x282FF4, ssi_type: Ssi}` | DL#727/#735 SSI=0x282FF4 | `reference_gold_attach_bitexact.md` |
| `endpoint_id` | `EndpointId` (u32) | SAP-internal | **timeslot number** (decoder-TN=1 für MCCH = air-TN=0) | M2/M3 alle MCCH-Replies auf TN=0 | `reference_gold_full_attach_timeline.md` Konstanten |
| `stealing_permission` | `bool` (NICHT Todo) | SAP-internal | **`false`** für M2/M3 Signalling-Replies | n/a (kein Voice-Stealing in Attach/Group-Attach) | ETSI §21.4.6 + bluestation: Stealing nur in TCH-Bursts während Voice-Calls |
| `subscriber_class` | `Todo` (i32) | SAP-internal (kein direktes Air-Mapping in Demand) | **`None`-equivalent** (entspricht D-LOC-UPDATE-ACCEPT.subscriber_class=`None` siehe MM-Sektion) | DL#735 `p_subscriber_class=0` ⇒ kein subscriber_class auf-Air | `reference_gold_attach_bitexact.md` Z.121 "p_subscriber_class = 0" |
| `air_interface_encryption` | `Option<Todo>` | SAP-internal | **`None`** (Gold-Cell ist Klartext, encryption_mode=00 in MAC-RESOURCE) | Alle DL-Bursts: enc_mode=00 | `reference_gold_attach_bitexact.md` Z.103 + Z.118 |
| `stealing_repeats_flag` | `Option<bool>` | SAP-internal | **`None`** (kein Voice-Stealing in Signalling) | n/a | bluestation Default; ETSI §21.4.6 |
| `data_category` | `Option<Todo>` | SAP-internal | **`None`** (MS = Voice/Control, kein Packet-Daten in M2/M3) | n/a | bluestation: `data_category` ist nur für SNDCP relevant |
| `chan_alloc` | `Option<CmceChanAllocReq>` | SAP-internal | **`None`** in M2 / M3 (Group-Attach), nur in Group-Call-Phase relevant | DL#735 hat slot_grant_flag=1 mit slot_grant_elem=0x00 — aber kein Chan-Alloc-Element (ca_flag=0) | `reference_gold_attach_bitexact.md` Z.110 ca_flag=0 |
| `tx_reporter` | `Option<TxReporter>` | SAP-internal | **`None`** wenn Daemon kein per-burst-tx-receipt braucht | n/a | bluestation-Mechanik; bei uns ggf. `Some(handle)` für Logging |

**custom-Field `chan_alloc` (TmaSap-erweitert) referenziert
`CmceChanAllocReq`:**

### `CmceChanAllocReq` (`tetra-saps/src/lcmc/fields/chan_alloc_req.rs`)

| Field | Bluestation Type | Bit-width on-air | Gold-Ref Value | Captures observed | Source / Why |
|---|---|---|---|---|---|
| `usage` | `Option<u8>` | per ETSI §21.5.2 (variable, im Channel-Allocation-Element) | **`None`** für M2 / M3 — Channel Allocation kommt erst bei Group-Call vor | M2/M3-Replys haben `ca_flag=0` ⇒ kein Channel-Allocation-Element | `reference_gold_attach_bitexact.md` + `reference_group_attach_bitexact.md` (DL-ACK ca_flag=0) |
| `carrier` | `Option<Todo>` | bis zu 24 bit (MainCarrier+Number) | **`None`** (Default = "uses self", d.h. selbe Trägerfrequenz, keine Carrier-Specifikation im Element) | n/a | bluestation Kommentar `// by default, uses self` + ETSI §21.5.2 `present_in_carrier_element`-Bit defaultet auf 0 |
| `timeslots` | `[bool; 4]` (NICHT Todo) | 4 bit | **TBD bei CMCE-Capture** — typisch eine 1 für Voice-Slot z.B. `[false, true, false, false]` | nicht bit-extrahiert (kein Group-Call Capture) | OPEN, siehe Sektion "Open uncertainties" |
| `alloc_type` | `ChanAllocType` (NICHT Todo) | 2 bit | **TBD** (`Replace`=0 wahrscheinlich für initialen Group-Call-Setup) | nicht bit-extrahiert | OPEN, ETSI §14.8.17a default `Replace=0` |
| `ul_dl_assigned` | `UlDlAssignment` (NICHT Todo) | 2 bit | **TBD** (`Both`=3 wahrscheinlich für Group-Call) | nicht bit-extrahiert | OPEN, ETSI §21.5.2 |

### `TmaUnitdataInd` (Clause 20.4.1.1.4) — MAC → LLC, receive-path

| Field | Bluestation Type | Bit-width on-air | Gold-Ref Value | Captures observed | Source / Why |
|---|---|---|---|---|---|
| `pdu` | `Option<BitBuffer>` | LI×8 | aus UL-Reassembly, 129 bit MM body bei UL-LOC-UPDATE-DEMAND | UL#0+UL#1 (M2), UL Frag1+Frag2 (M3) | `reference_demand_reassembly_bitexact.md` |
| `main_address` | `TetraAddress` | 24 bit | `TetraAddress{ssi:0x282FF4, ssi_type:Ssi}` | UL#0..UL#2 alle | `reference_gold_attach_bitexact.md` |
| `scrambling_code` | `u32` (NICHT Todo) | 32 bit | `0x4183F207` (Gold-Cell) bzw. cell-spezifisch berechnet | siehe `gen_d_nwrk_broadcast.py:cell_scramble` Init-Pattern | `reference_gold_full_attach_timeline.md` Konstanten |
| `endpoint_id` | `EndpointId` (u32) | SAP-internal | timeslot der Empfangsburst (TN=0 für MCCH-RX) | UL#0..UL#2 alle | `reference_gold_full_attach_timeline.md` |
| `new_endpoint_id` | `Option<EndpointId>` | SAP-internal | **`None`** (kein Channel-Change in M2/M3) | n/a | bluestation Default |
| `css_endpoint_id` | `Option<EndpointId>` | SAP-internal | **`None`** (kein Carrier-Specific Signalling Channel in M2/M3) | n/a | bluestation Default |
| `air_interface_encryption` | `Todo` | SAP-internal | **0** (Klartext, encryption_mode=00) | UL#0..UL#2 enc=0 | `reference_demand_reassembly_bitexact.md` Z.31 |
| `chan_change_response_req` | `bool` (NICHT Todo) | SAP-internal | **`false`** für M2/M3 — kein Channel-Change | n/a | bluestation Default |
| `chan_change_handle` | `Option<Todo>` | SAP-internal | **`None`** (kein Channel-Change) | n/a | bluestation Default |
| `chan_info` | `Option<Todo>` | SAP-internal | **`None`** (kein Channel-Change) | n/a | bluestation Default |

### `TmaCancelReq` / `TmaReportInd`

| Field | Bluestation Type | Bit-width on-air | Gold-Ref Value | Source / Why |
|---|---|---|---|---|
| `req_handle` (beide) | `Todo` (i32) | SAP-internal | matched gegen den TmaUnitdataReq.req_handle | bluestation-Mechanik; kein Air-Bezug |

### `TmaReleaseInd`

| Field | Bluestation Type | Bit-width on-air | Gold-Ref Value | Source / Why |
|---|---|---|---|---|
| `endpoint_id` | `EndpointId` | SAP-internal | timeslot (im Release-Kontext) | bluestation; kein Air-Bezug |

---

## mm/U-LOCATION-UPDATE-DEMAND (`mm/pdus/u_location_update_demand.rs`)

129 bit MM body nach Reassembly aus UL#0[48..91] ++ UL#1[7..91].
Bit-Positionen aus `reference_demand_reassembly_bitexact.md` Z.74-113.

| Field | Bluestation Type | Bit-width | Gold-Ref Value | Captures observed | Source / Why |
|---|---|---|---|---|---|
| `location_update_type` | `LocationUpdateType` enum | 3 | **`ItsiAttach` (=3)** | M2-Attach 1..6 in Gold-Ref-Capture | `reference_demand_reassembly_bitexact.md` Z.75 + `reference_gold_full_attach_timeline.md` Z.76 |
| `request_to_append_la` | `bool` | 1 | **`false`** (=0) | UL#0 Frag1 alle 6 Attach-Versuche | `reference_gold_full_attach_timeline.md` Z.76 |
| `cipher_control` | `bool` | 1 | **`false`** (=0) | UL#0 alle | Gold-Cell ist Klartext |
| `ciphering_parameters` | `Option<u64>` (10 bit, conditional auf cipher_control) | 0 (absent) | **`None`** | n/a | folgt aus `cipher_control=false` |
| `class_of_ms` | `Option<ClassOfMs>` (24 bit) | 24 (when present) | **`Some(0x1A0F60)`** ≡ `ClassOfMs{voice=1, e2e_encryption_not_supported=1, tetra_packet_data=1, minimum_mode=1, carrier_specific_signalling=1, authentication=1, sck_encryption=1, air_interface_version=3, alle anderen=0}` | UL#0+UL#1 reassembled, 6× konsistent | `reference_gold_full_attach_timeline.md` Z.76 ("class_of_ms=0x1A0F60"), bit-decode siehe ClassOfMs in `mm/fields/class_of_ms.rs` |
| `energy_saving_mode` | `Option<EnergySavingMode>` (3 bit) | 3 (when present) | **`Some(StayAlive)`** (=0) für 5/6 Attaches; 1 Attach hatte `Eg1` (=1) — **Default für unsere Encoder = `StayAlive`** | UL#0 alle, ESM=1 in Frag1 | `reference_gold_full_attach_timeline.md` Z.76 ("ESM=1" — der "1" ist hier das Presence-Flag, nicht der Wert; tatsächlicher Modus aus 3-bit Slice) — siehe Bemerkung unten |
| `la_information` | `Option<u64>` (14 bit + 1 trailing zero) | 15 | **`None`** (kein la_information in M2-Attach Frag1+2) | n/a | bluestation Test-Vector + Gold-Body 129 bit fasst keinen LA-Block ein |
| `ssi` | `Option<u64>` (24 bit) | 24 (when present) | **`None`** (MS-ISSI sitzt im MAC-ACCESS-Header, nicht im MM-Body Type-2) | UL#0 SSI=0x282FF4 ist MAC-Header, kein MM-Body-Feld | `reference_gold_attach_bitexact.md` "MAC-ACCESS Header [5..28] ISSI" |
| `address_extension` | `Option<u64>` (24 bit) | 24 (when present) | **`None`** | n/a | n/a (no AE in single-network attach) |
| `group_identity_location_demand` | `Option<GroupIdentityLocationDemand>` | variable | **`Some(GILD{attach_detach_type_id=0(attach), class_of_usage=4, address_type=0(GSSI only), gssi=0x2F4D61})`** für externe BS Gold-Ref. Für MTP3550: `gssi=0x000001` (default no-group). | UL#1 byte 6..8 = `2F 4D 61` (Gold) bzw. `00 00 01` (MTP3550) | `reference_demand_reassembly_bitexact.md` Z.119-138 + Z.130-135 |
| `group_report_response` | `Option<Type3FieldGeneric>` | 3 | **`None`** | UL alle | Gold-Body Z.131..132 ist trailing m-bit=0 (kein report response gesetzt) |
| `authentication_uplink` | `Option<Type3FieldGeneric>` | 3 | **`None`** | UL alle | Gold-Cell ist nicht authenticated |
| `extended_capabilities` | `Option<Type3FieldGeneric>` | 3 | **`None`** | UL alle | bluestation-Default; Gold sendet nur class_of_ms |
| `proprietary` | `Option<Type3FieldGeneric>` | 3 | **`None`** | UL alle | bluestation-Default |
| `optional_field_value` (TODO-A scope: pseudonym for o-bit at body[5]) | implicit Type-2/3-Trigger | 1 | **1** (presence of class_of_ms + GILD ⇒ o-bit must be set) | UL alle | folgt aus class_of_ms.is_some() per `to_bitbuf` |

**Notiz zu `energy_saving_mode`:** Die zitierte Stelle in
`reference_gold_full_attach_timeline.md` Z.76 ("ESM=1") meint das
**Presence-Flag** (= p_energy_saving_mode), nicht den 3-bit-Wert. Der
3-bit-Wert wurde nicht separat aus dem Frag1-Body extrahiert — pro
ETSI-Default und MS-Verhalten ist `StayAlive` (=0) zu erwarten, aber das
ist eine **Open uncertainty** (siehe unten), bis der UL-Reassembly-Decoder
den exakten 3-bit-Slice ausgibt.

---

## mm/D-LOCATION-UPDATE-ACCEPT (`mm/pdus/d_location_update_accept.rs`)

DL#735, MM body bits[60..161] = 102 bit (per `reference_gold_attach_bitexact.md` Z.116-152).

| Field | Bluestation Type | Bit-width | Gold-Ref Value | Captures observed | Source / Why |
|---|---|---|---|---|---|
| `location_update_accept_type` (in TODO-A list als `loc_acc_type`) | `LocationUpdateType` enum | 3 | **`ItsiAttach` (=3)** | DL#735 alle 3 sauberen Pärchen | `reference_gold_attach_bitexact.md` Z.117 |
| `ssi` | `Option<u64>` (24 bit) | 24 (when present) | **`None`** (kein SSI im MM-Body, SSI sitzt im MAC-RESOURCE Header) | DL#735 `p_ssi=0` | `reference_gold_attach_bitexact.md` Z.119 |
| `address_extension` | `Option<u64>` (24 bit) | 24 (when present) | **`None`** | DL#735 `p_address_extension=0` | `reference_gold_attach_bitexact.md` Z.120 |
| `subscriber_class` | `Option<u64>` (16 bit) | 16 (when present) | **`None`** | DL#735 `p_subscriber_class=0` | `reference_gold_attach_bitexact.md` Z.121 |
| `energy_saving_information` | `Option<EnergySavingInformation>` (3+5+6=14 bit) | 14 (when present) | **`Some(EnergySavingInformation{energy_saving_mode=StayAlive, frame_number=None, multiframe_number=None})`** ≡ 14×0 (StayAlive ⇒ FN/MN have no meaning, encoded as 0) | DL#735 `p_energy_saving_info=1`, payload 14×0 | `reference_gold_attach_bitexact.md` Z.122-123 + `mm/fields/energy_saving_information.rs` Z.46-61 |
| `scch_information_and_distribution_on_18th_frame` | `Option<u64>` (6 bit) | 6 (when present) | **`None`** | DL#735 `p_scch_info_distrib_18=0` | `reference_gold_attach_bitexact.md` Z.124 |
| `new_registered_area` | `Option<Type4FieldGeneric>` | variable | **`None`** | DL#735 (kein Type-4 Slot vor T3-GILA m-bit) | `reference_gold_attach_bitexact.md` Z.125-128 — m_bit Type-3 kommt direkt nach SCCH-p-bit |
| `security_downlink` | `Option<Type3FieldGeneric>` | 3 | **`None`** | DL#735 (kein authentication-Block) | Gold-Cell unauth |
| `group_identity_location_accept` | `Option<GroupIdentityLocationAccept>` | variable (58 bit Payload bei Gold) | **`Some(GroupIdentityLocationAccept{group_identity_accept_reject=Accept(=0), reserved=0, group_identity_downlink=Some([GroupIdentityDownlink{attach_detach_type_id=0(attach), lifetime=1, class_of_usage=4, address_type=0(GSSI), gssi=0x2F4D61}])})`** mit T4-len=38 + num_elems=1 | DL#735 elem_id=0101 (GILA), length=58 | `reference_gold_attach_bitexact.md` Z.125-149 |
| `default_group_attachment_lifetime` | `Option<Type3FieldGeneric>` | 3 | **`None`** | DL#735 (kein Type-3 nach GILA, nur trailing m-bit=0) | `reference_gold_attach_bitexact.md` Z.150 (nach GILA: trailing m-bit=0) |
| `authentication_downlink` | `Option<Type3FieldGeneric>` | 3 | **`None`** | DL#735 | Gold-Cell unauth |
| `group_identity_security_related_information` | `Option<Type4FieldGeneric>` | variable | **`None`** | DL#735 | Gold-Cell unauth |
| `cell_type_control` | `Option<Type3FieldGeneric>` | 3 | **`None`** | DL#735 | bluestation-Default |
| `proprietary` | `Option<Type3FieldGeneric>` | 3 | **`None`** | DL#735 | bluestation-Default |

### Alle p-bits explizit (TODO-A Scope: "all p-bits")

| p-bit | Gold-Ref Value | Bit-Position (relative zu MM-Body bit 0 = bit 60 in DL#735 absolute) |
|---|---|---|
| `p_ssi` | **0** | bit-relative 8 (= absolute 68) |
| `p_address_extension` | **0** | bit-relative 9 (= absolute 69) |
| `p_subscriber_class` | **0** | bit-relative 10 (= absolute 70) |
| `p_energy_saving_info` | **1** | bit-relative 11 (= absolute 71) |
| `p_scch_info_distrib_18` | **0** | bit-relative 26 (= absolute 86) |
| o-bit (location update accept) | **1** (wird durch Type-1 + Type-2/3-Presence getriggert) | bit-relative 7 (= absolute 67) |
| m-bit (T3 follows = GILA) | **1** | bit-relative 27 (= absolute 87) |
| trailing m-bit (after GILA Type-3 list) | **0** | bit-relative 101 (= absolute 161) |

### GroupIdentityLocationAccept Payload (58 bit) — bit-exakt aus Gold

Per `reference_gold_attach_bitexact.md` Z.135-150:

| Field | Bit-width | Gold-Ref Value |
|---|---|---|
| `group_identity_accept_reject` | 1 | 0 (Accept) |
| `reserved` | 1 | 0 |
| o-bit (GILA inner) | 1 | 1 |
| m-bit (T4 GroupIdentityDownlink) | 1 | 1 |
| `elem_id` | 4 | 0111 (=7) |
| `length` | 11 | 38 |
| `num_elems` | 6 | 1 |
| `attach_detach_type_id` | 1 | 0 (attachment) |
| `lifetime` | 2 | 01 (=1, "Attachment for next ITSI attach required") |
| `class_of_usage` | 3 | 100 (=4) |
| `address_type` | 2 | 00 (=0, GSSI only) |
| `gssi` | 24 | 0x2F4D61 |
| trailing m-bit (GILA) | 1 | 0 |

---

## cmce/{D-SETUP, U-SETUP, D-CALL-PROCEEDING, D-CONNECT, U-TX-DEMAND, D-TX-GRANTED, U-RELEASE, D-RELEASE}

### CAVEAT — Gold silent

**Es existiert KEIN bit-exakter Gold-Reference Capture eines erfolgreichen
Group-Calls (CMCE-PDU-Phase).** Die GOLD-Captures enthalten zwar einen
Gruppenruf (TCH-Voice-Phase, AACH-Pattern `0x22C9` / `0x304B`), aber die
zugehörigen CMCE-PDU-Bits wurden nicht extrahiert (Group-Call wurde
erkannt, aber die CMCE-Setup-Bursts liegen außerhalb der M2/M3-Replay-
Sequenz und sind im aktuellen Capture-Set nicht bit-genau dokumentiert).

⇒ **Alle Werte in dieser Sektion sind ETSI/Bluestation-Default. Diese
Sektion wird revidiert, sobald ein Group-Call-Capture vorliegt.** Per
CLAUDE.md Source-of-truth-Hierarchy fallen wir hier auf Bluestation
zurück; wo Bluestation auch Todo/None-Default hat, fallen wir auf ETSI
EN 300 392-2 §14.7 zurück.

Die Tabellen unten dokumentieren primär **bit-widths**, damit der Encoder
bounded ist. Konkrete Werte sind PROVISIONAL und vor M3-Group-Call-Tests
gegen einen tatsächlichen Capture zu verifizieren.

### `DSetup` (Clause 14.7.1.12) — DL → called-MS

| Field | Bluestation Type | Bit-width | PROVISIONAL Value | Source / Why |
|---|---|---|---|---|
| `call_identifier` | `u16` (NICHT Todo) | 14 | BS-allocated, 1..2^14-1 | ETSI §14.8.4 |
| `call_time_out` | `CallTimeout` enum | 4 | `Infinite` (=0) für PMR-Group-Call standard | bluestation Test `test_d_setup` Z.301 |
| `hook_method_selection` | `bool` | 1 | **`false`** (= 0, no hook signalling, direct through-connect) | ETSI §14.8.27 |
| `simplex_duplex_selection` | `bool` | 1 | **`false`** (= 0, simplex für Group-Call) | ETSI §14.8.41 |
| `basic_service_information` | `BasicServiceInformation` (8 bit) | 8 | `BasicServiceInformation{circuit_mode_type=TchS(=0), encryption_flag=false, communication_type=P2Mp(=1), speech_service=Some(0)}` | bluestation Test `test_d_setup` Z.305-309 |
| `transmission_grant` | `TransmissionGrant` enum | 2 | `Granted` (=0) für initiator | ETSI §14.8.42 |
| `transmission_request_permission` | `bool` | 1 | **`false`** (allow others to demand TX) — wenn Dispatcher-controlled, sonst `true` | ETSI §14.8.43 |
| `call_priority` | `u8` | 4 | **`0`** (lowest) | ETSI §14.8.5 |
| `notification_indicator` | `Option<u64>` (6 bit) | 6 (when present) | **`None`** | bluestation-Default |
| `temporary_address` | `Option<u64>` (24 bit) | 24 (when present) | **`None`** | bluestation-Default |
| `calling_party_address_ssi` | `Option<u32>` (24 bit, conditional auf calling_party_type_identifier) | 24 (when present) | `Some(<calling-MS-SSI>)` z.B. `0x282FF4` für MTP3550 | bluestation-Test |
| `calling_party_extension` | `Option<u32>` (24 bit) | 24 (when present) | **`None`** (kein cross-network in single-cell test) | bluestation-Default |
| `external_subscriber_number` / `facility` / `dm_ms_address` / `proprietary` | `Option<Type3FieldGeneric>` | 3 each | **`None`** | bluestation-Default |

### `USetup` (Clause 14.7.2.10) — UL → BS

| Field | Bluestation Type | Bit-width | PROVISIONAL Value | Source / Why |
|---|---|---|---|---|
| `area_selection` | `u8` | 4 | **`0`** (no SS-AS) | ETSI EN 300 392-12-8 §5.2.2.3 |
| `hook_method_selection` | `bool` | 1 | **`false`** | ETSI §14.8.27 |
| `simplex_duplex_selection` | `bool` | 1 | **`false`** (simplex) | ETSI §14.8.41 |
| `basic_service_information` | `BasicServiceInformation` | 8 | wie D-SETUP (TchS, P2Mp, speech_service=0) | ETSI §14.8.2 |
| `request_to_transmit_send_data` | `bool` | 1 | **`true`** (PTT-Initiator möchte direkt senden) | ETSI §14.7.2.10 (initiator request) |
| `call_priority` | `u8` | 4 | **`0`** | ETSI §14.8.5 |
| `clir_control` | `u8` | 2 | **`0`** (default, no CLI restriction) | ETSI EN 300 392-12-1 §4.3.5 |
| `called_party_type_identifier` | `PartyTypeIdentifier` enum | 2 | `Ssi` (=1) für Group-Call (called=GSSI) | ETSI §14.8.32 |
| `called_party_short_number_address` | `Option<u64>` (8 bit) | 8 (when CPTI=Sna) | **`None`** für Group-Call | folgt aus CPTI=Ssi |
| `called_party_ssi` | `Option<u64>` (24 bit) | 24 (when CPTI=Ssi/Tsi) | `Some(<gssi>)` z.B. `Some(0x2F4D61)` für Gold-Group | folgt aus CPTI=Ssi |
| `called_party_extension` | `Option<u64>` (24 bit) | 24 (when CPTI=Tsi) | **`None`** | folgt aus CPTI=Ssi |
| Type-3 fields (4 stück) | `Option<Type3FieldGeneric>` | 3 each | **`None`** | bluestation-Default |

### `DCallProceeding` (Clause 14.7.1.2)

| Field | Type | Bit-width | PROVISIONAL Value |
|---|---|---|---|
| `call_identifier` | `u16` | 14 | matched zur U-SETUP call_identifier |
| `call_time_out_set_up_phase` | `CallTimeoutSetupPhase` enum | 3 | `T30s` (per bluestation-Test) |
| `hook_method_selection` | `bool` | 1 | **`false`** |
| `simplex_duplex_selection` | `bool` | 1 | **`false`** |
| `basic_service_information` | `Option<BasicServiceInformation>` | 8 (when present) | **`None`** (only if different from requested) |
| `call_status` | `Option<CallStatus>` | 3 (when present) | **`None`** |
| `notification_indicator` | `Option<u64>` | 6 (when present) | **`None`** |
| `facility` / `proprietary` | `Option<Type3FieldGeneric>` | 3 each | **`None`** |

### `DConnect` (Clause 14.7.1.4)

| Field | Type | Bit-width | PROVISIONAL Value |
|---|---|---|---|
| `call_identifier` | `u16` | 14 | matched |
| `call_time_out` | `CallTimeout` enum | 4 | `T5m` (per bluestation-Test) |
| `hook_method_selection` | `bool` | 1 | **`false`** |
| `simplex_duplex_selection` | `bool` | 1 | **`false`** |
| `transmission_grant` | `TransmissionGrant` enum | 2 | `Granted` (=0) für PTT-initiator |
| `transmission_request_permission` | `bool` | 1 | **`false`** |
| `call_ownership` | `bool` | 1 | **`false`** |
| `call_priority` | `Option<u64>` | 4 (when present) | **`None`** |
| `basic_service_information` | `Option<BasicServiceInformation>` | 8 (when present) | **`None`** |
| `temporary_address` | `Option<u64>` | 24 (when present) | **`None`** |
| `notification_indicator` | `Option<u64>` | 6 (when present) | **`None`** |
| `facility` / `proprietary` | `Option<Type3FieldGeneric>` | 3 each | **`None`** |

### `UTxDemand` (Clause 14.7.2.12)

| Field | Type | Bit-width | PROVISIONAL Value |
|---|---|---|---|
| `call_identifier` | `u16` | 14 | aktiver Call |
| `tx_demand_priority` | `u8` | 2 | **`0`** (lowest) |
| `encryption_control` | `bool` | 1 | **`false`** (Klartext) |
| `reserved` | `bool` | 1 | **`false`** (per ETSI: shall be set to 0) |
| `facility` / `dm_ms_address` / `proprietary` | `Option<Type3FieldGeneric>` | 3 each | **`None`** |

### `DTxGranted` (Clause 14.7.1.15)

| Field | Type | Bit-width | PROVISIONAL Value |
|---|---|---|---|
| `call_identifier` | `u16` | 14 | matched |
| `transmission_grant` | `u8` (NICHT enum, raw bits!) | 2 | **`0`** (Granted) |
| `transmission_request_permission` | `bool` | 1 | **`false`** (1 = MS not allowed to request — gegenüber dem talker) |
| `encryption_control` | `bool` | 1 | **`false`** |
| `reserved` | `bool` | 1 | **`false`** (per ETSI: shall be 0) |
| `notification_indicator` | `Option<u64>` | 6 (when present) | **`None`** |
| `transmitting_party_type_identifier` | `Option<u64>` | 2 (when present) | **`Some(1)`** (Ssi für talker-SSI) |
| `transmitting_party_address_ssi` | `Option<u64>` | 24 (when present) | `Some(<talker-SSI>)` |
| `transmitting_party_extension` | `Option<u64>` | 24 (when TPTI=2) | **`None`** |
| Type-3 fields | `Option<Type3FieldGeneric>` | 3 each | **`None`** |

### `URelease` (Clause 14.7.2.9) / `DRelease` (Clause 14.7.1.9)

| Field | Type | Bit-width | PROVISIONAL Value |
|---|---|---|---|
| `call_identifier` | `u16` | 14 | matched |
| `disconnect_cause` | `DisconnectCause` enum | 5 | `UserRequested` (=0) für reguläres hang-up; `ExpiryOfTimer` für Timeout (siehe bluestation `test_parse_d_release` Z.122) |
| `notification_indicator` (D-RELEASE only) | `Option<u64>` | 6 (when present) | **`None`** |
| `facility` / `proprietary` | `Option<Type3FieldGeneric>` | 3 each | **`None`** |

---

## mle/D-NWRK-BROADCAST (`mle/pdus/d_nwrk_broadcast.rs`)

Quelle: `decode_dl.py` Lauf 2026-05-03 gegen `GOLD_DL_ANMELDUNG_GRUPPENWECHSEL_GRUPPENRUF.wav`,
Burst #423 (TN=1 FN=04 MN=44, AACH `0x0249`, addr=SSI=`0xFFFFFF`, LI=16). Dies
ersetzt die frühere Bit-by-Bit-Analyse aus `scripts/gen_d_nwrk_broadcast.py:GOLD_INFO_124`,
die einen 8-bit-Shift-Bug in der SSI-Region hatte.

**124 Info-Bits aus dem Decoder (Bit 0 = first on-air bit, MSB-first):**

```
0010000010000001 111111111111111111111111 000 0010 101 010
0101011001010101 00 1 1 000111111001110111111001000010000100011111111111100
```

### MAC-RESOURCE Wrapper (bits 0..42)

| Bit-pos | Field | Gold-Ref Value |
|---|---|---|
| [0..1] | mac_pdu_type | `00` (MAC-RESOURCE) |
| [2] | fill_bit | `1` |
| [3] | pos_of_grant | `0` |
| [4..5] | encryption_mode | `00` |
| [6] | random_access_flag | `0` |
| [7..12] | length_indication | **`16`** (16 Oktetts = 128 bit; 124 info + 4 fill) |
| [13..15] | address_type | `001` (SSI) |
| [16..39] | SSI | **`0xFFFFFF`** (broadcast) |
| [40] | pwr_flag | `0` ⇒ no power_control_element |
| [41] | slot_grant_flag | `0` ⇒ no slot_grant_elem |
| [42] | chan_alloc_flag | `0` ⇒ no chan_alloc_element |

**Wrapper endet bei bit 42.** TM-SDU startet bei **bit 43** (kompakte
Variante: alle drei Optional-Flags = 0 für broadcast-PDU). Die ältere
Version dieses Memos ging fälschlich von TM-SDU @ bit 51 aus, was zu
einem Bit-Budget-Konflikt mit der 48-bit TNT führte.

### LLC + MLE Wrapper (bits 43..52)

| Bit-pos | Field | Gold-Ref Value |
|---|---|---|
| [43..46] | llc_pdu_type | `0010` (= 2, **BL-UDATA**) |
| [47..49] | mle_disc | `101` (= 5, **MLE-itself**) |
| [50..52] | mle_prim (3-bit MLE-Protocol-PDU-Type) | `010` (= 2, **D-NWRK-BCAST**) |

### D-NWRK-BCAST Body Felder (bits 53..123)

| Field | Bluestation Type / Width | Gold-Ref Value (Burst #423) | Source |
|---|---|---|---|
| `cell_re_select_parameters` | `u16` (16 bit, Type 1) | **`0x5655`** (= 22 101) | bits[53..68] = `0101011001010101` |
| `cell_load_ca` | `u8` (2 bit, Type 1) | **`0`** (= cell load 0%-25%, "low load") | bits[69..70] = `00` |
| o-bit | 1 bit | **`1`** (Type-2 fields follow) | bit[71] |
| `p_tetra_network_time` | 1 bit | **`1`** (TNT present) | bit[72] |
| `tetra_network_time` | `Option<u64>` 48 bit per ETSI Table 18.100 | **`0x1F9DF9_0847FF`** | bits[73..120] |
|   • UTC time (24 bit) | | `0x1F9DF9` ≈ 12 Tage seit Jan 1 UTC | bits[73..96] |
|   • Local offset sign (1 bit) | | `0` (positive) | bit[97] |
|   • Local offset (6 bit) | | `4` × 15 min = +60 min (CET) | bits[98..103] |
|   • Year (6 bit) | | `8` ⇒ 2008 (BS-RTC nicht aktuell aber legal) | bits[104..109] |
|   • Reserved (11 bit) | | `0x7FF` (all-ones, ETSI-konform) | bits[110..120] |
| `p_number_of_ca_neighbour_cells` | 1 bit | **`1`** | bit[121] |
| `number_of_ca_neighbour_cells` | `Option<u64>` 3 bit | **`0`** (bit-budget-konsistente einzige Antwort, siehe Erklärung unten) | bits[122..124] |
| `neighbour_cell_information_for_ca` | `Option<u64>` (conditional, nur wenn count > 0) | **`None`** | count = 0 ⇒ kein inline-Record |

### Encoder-Reset-Defaults (für unsere `cmce_nwrk_bcast.c` / FPGA-ROM)

```c
static const NwrkBcastDefault gold_burst_423 = {
    .cell_re_select_parameters = 0x5655,
    .cell_load_ca              = 0,
    .o_bit                     = 1,
    .p_tetra_network_time      = 1,
    .tetra_network_time        = 0x1F9DF9'0847FF, /* updates per cell RTC tick */
    .p_number_of_ca_neighbour_cells = 1,
    .number_of_ca_neighbour_cells   = 0, /* low 3 bit; full 3-bit needs decode beyond bit 123 */
};
```

UTC-time-Subfeld muss in Production aus dem System-RTC (oder einer per-Cell
TNT-Konfig) gespeist werden, nicht hartkodiert; die anderen Felder bleiben
statisch pro Cell.

### Erklärung `number_of_ca_neighbour_cells = 0`

Per ETSI EN 300 392-2 §18.4.1.4.1 + §18.4.1.4.6 ist
`number_of_ca_neighbour_cells` ein 3-Bit-Count (`N` ∈ 0..7) der angibt
**wieviele Neighbour-Cell-Info-Records inline im D-NWRK-BCAST-PDU folgen**.
Wenn `N > 0`, folgen `N × neighbour_cell_information_for_ca`-Strukturen
mit variabler Länge (per §18.5), die jeweils mehrere bit groß sind (Carrier,
Band, RxLevAccessMin, RadioLink-Timeout, …).

In Gold #423 ist `p_number_of_ca_neighbour_cells = 1` ⇒ Feld ist im PDU.
Bits 122..124 enthalten den 3-Bit-Count. Bit 124 liegt jenseits des
124-Bit-Decoder-Dumps, aber bit-budget-Logik gibt nur eine konsistente
Antwort:

- **LI = 16 octets = 128 air-bits.** Nach bit 121 (p_NCA=1) sind 6 Bits
  übrig: 3 Bit count + bis zu 3 Bit weiter.
- **Wenn `N > 0`:** Inline-Records müssten direkt folgen. Kleinste sinnvolle
  Neighbour-Cell-Info per §18.5 ist deutlich > 3 Bit (mindestens 12 Bit
  Carrier + Service-Details). Passt **nicht** in die verbleibenden 4 Bits
  jenseits Bit 123. **Inkonsistent.**
- **Wenn `N = 0`:** Nach dem Count keine weiteren Body-Bits. Restliche
  4 Bits (124..127) = fill zu Octet-Grenze. **Konsistent ✓**

⇒ **`N = 0`** ist die einzige bit-budget-konsistente Belegung. Bit 124
muss `0` sein. Passt zur physikalischen Situation: stand-alone Test-BS,
keine deklarierten Nachbar-Cells.

**Encoder-Verhalten in S4:** Bei `nwrk_num_ca_neighbour_cells = 0` wird
das Feld korrekt emittiert (3 Bits = `000`). Bei `> 0` returnt der Encoder
`-ENOTSUP`, weil inline-Records nicht implementiert sind (auch bluestation
hat dort `unimplemented!()`). Wenn künftig echte Multi-Cell-Konfiguration
gebraucht wird, müssen die §18.5 Inline-Record-Encoder ergänzt werden.

### Buggy upstream constant flagged

`/home/kevin/claude-ralph/tetra/scripts/gen_d_nwrk_broadcast.py:GOLD_INFO_124`
hat einen **8-bit-Shift-Bug**: enthält ein extra `0xFF`-Byte direkt nach den
korrekten 24 SSI-Bits, was alle nachfolgenden Felder um 8 Bit shiftet und
das letzte Byte am Frame-Ende abschneidet. Die Konstante reproduziert NICHT
Gold-Burst #423. **Maßgeblich ist der Decoder-Dump oben** — wenn die
upstream gen-Script-Konstante in einem CMCE-TB als Fixture verwendet wird,
muss sie durch die obigen 124 Bit ersetzt werden.

### Veraltete Bit-Budget-Anmerkung (zur Historie)
expliziter MLE/D-NWRK-BROADCAST-Decoderlogik, dann Auswertung welche
Möglichkeit oben zutrifft.

**Konservativer Default für unseren Encoder bis dahin:** TNT-Feld
**absent** schreiben (`tetra_network_time = None`), o-bit=0 in
D-NWRK-BCAST. Das reproduziert garantiert 124 Info-Bit ohne
Mismatch und ist auch mit ETSI vereinbar (TNT ist Type-2 Optional,
nicht Mandatory).

### ETSI EN 300 392-2 §18.5.24 Table 18.100 — TNT-Layout (zur Referenz)

| Subfeld | Bit-width | Wert |
|---|---|---|
| UTC time | 24 | Sekunden×½ seit 01.01. 00:00 UTC. `0xFFFFFF` = invalid, `0xF142FE` ≈ 97 Tage Maximum |
| Local time offset sign | 1 | 0 = positiv, 1 = negativ |
| Local time offset | 6 | Schritt 15 min, max ±14h, `0x3F` = invalid |
| Year | 6 | Jahre seit 2000, `0x3F` = invalid |
| Reserved | 11 | All-ones (`0x7FF`) |

**Summe: 48 bit.** Bluestation `Option<u64>` 48 bit ist ETSI-konform.

### MLE Annex E.2.1 Notation

Bluestation-Kommentar Z.58 in `d_nwrk_broadcast.rs`: "MLE PDUs do not use
M-bits (Annex E.2.1) — no trailing delimiter to read". Konsistent mit
Gold: nach dem letzten Type-2-Feld stehen nur fill-bits.

---

## Open uncertainties

Die folgenden Felder benötigen entweder einen **neuen Gold-Capture** oder
eine **explizite Kevin-Entscheidung** vor bit-genauer Encoder-Implementation:

1. **MAC-RESOURCE Header-Layout für D-NWRK-BCAST.** Bluestation
   spezifiziert `power_control_flag(1) + slot_granting_flag(1) +
   chan_alloc_flag(1)` als zwingende 3 Flag-Bits nach SSI. Gold-Burst
   #423 hat aber TM-SDU bei bit 51 (statt bit 43 wie bluestation default
   ohne flags), was implies einen 11-bit-Bereich [40..50] mit einer nicht
   exakt zu bluestation passenden Interpretation. **Action:** Kevin
   bestätigen, ob für broadcast-SSI (`0xFFFFFF`) die MAC-RESOURCE-PCF/SGF/
   CAF-Bits ein anderes Layout haben, oder ob die GOLD_INFO_124-Konstante
   tatsächlich `pwr_flag=1, pwr_elem=15, slot_grant_flag=1,
   slot_grant_elem=0xC1, chan_alloc_flag=0` enthält (was unwahrscheinlich
   ist da kein PMR-BS einen Power-Control für Broadcast macht).

2. **D-NWRK-BCAST `tetra_network_time` in Gold #423 — RESOLVED 2026-05-03.**
   Frischer `decode_dl.py`-Lauf gegen `GOLD_DL_ANMELDUNG_GRUPPENWECHSEL_GRUPPENRUF.wav`
   (Burst #423, TN=1 FN=04 MN=44, AACH `0x0249`) liefert die 124 Info-Bits:
   ```
   0010000010000001 111111111111111111111111 000 0010 101 010
   0101011001010101 00 1 1 000111111001110111111001000010000100011111111111100
   ```
   Bit-positions (richtige MAC-RESOURCE-Wrapper-Größe = 43 bit, nicht 51):
   - `[0..15]` MAC-RESOURCE-Header: pdu_type=00, fill=1, PoG=0, enc=00, RA=0,
     LI=`010000`=16, addr_type=`001`=SSI.
   - `[16..39]` SSI = `0xFFFFFF` (broadcast).
   - `[40]` pwr_flag=0, `[41]` slot_grant_flag=0, `[42]` chan_alloc_flag=0
     ⇒ TM-SDU startet bei **bit 43** (NICHT bit 51 wie ursprünglich vom
     S4-Audit angenommen).
   - `[43..46]` LLC pdu_type=`0010`=BL-UDATA.
   - `[47..49]` MLE-disc=`101`=MLE-itself.
   - `[50..52]` mle_prim=`010`=D-NWRK-BCAST.
   - `[53..68]` `cell_re_select_parameters` = **`0x5655`**.
   - `[69..70]` `cell_load_ca` = **`0`**.
   - `[71]` o-bit = **`1`**.
   - `[72]` `p_tetra_network_time` = **`1`**.
   - `[73..120]` `tetra_network_time` (48 bit, ETSI Table 18.100) =
     **`0x1F9DF9_0847FF`**:
     - UTC time (24 bit) = `0x1F9DF9` = 2 072 057 (×½ sec ≈ 12 Tage seit
       Jan 1 UTC der Network-Time-Year)
     - Local time offset sign = `0` (positive)
     - Local time offset (6 bit) = `4` × 15 min = +60 min (CET)
     - Year (6 bit) = `8` ⇒ Year 2008 (BS-RTC nicht aktuell, aber bit-feld
       legal)
     - Reserved (11 bit) = `0x7FF` ✓ (ETSI-konform all-ones)
   - `[121]` `p_number_of_ca_neighbour_cells` = `1` (3-bit count + 4-bit fill
     im LI=16-Padding-Bereich nicht im 124-bit Decoder-Dump sichtbar; LI=16
     octets = 128 bit ⇒ 4 fill bits jenseits bit 123).

   **Konsequenz für unseren Encoder:** o-bit=1, p_TNT=1, 48-bit TNT mit
   ETSI-Subfeld-Layout. TNT-Wert dynamisch aus Cell-RTC oder per-Cell-
   Konfiguration. Default-Reset-Wert = `0x1F9DF9_0847FF` (von Gold #423
   übernommen, BS-Operator setzt aktuellen Wert via `REG_RTC_*` falls
   gewünscht).

   **Buggy upstream constant flagged:** `GOLD_INFO_124` in
   `/home/kevin/claude-ralph/tetra/scripts/gen_d_nwrk_broadcast.py`
   hat einen 8-bit-Shift-Bug (extra `0xFF`-Byte in der SSI-Region nach
   den korrekten 24 SSI-Bits). Die Konstante ist NICHT verbindlich;
   maßgeblich ist der Decoder-Dump oben.

3. **U-LOC-UPDATE-DEMAND `energy_saving_mode` (3-bit Wert).** Memo
   `reference_gold_full_attach_timeline.md` Z.76 zitiert "ESM=1" ist das
   Presence-Flag, nicht der 3-bit-Wert. Der 3-bit-Slice wurde nicht
   separat dokumentiert. **Action:** UL-Reassembly Decoder erweitern um
   den `energy_saving_mode`-Slice aus den 6× Gold-Attaches zu extrahieren.
   Default-Annahme `StayAlive=0` ist plausibel aber unbestätigt.

4. **CmceChanAllocReq Felder `timeslots`/`alloc_type`/`ul_dl_assigned`
   (alle non-Todo aber per TODO-A scope).** Erfordern einen erfolgreichen
   Group-Call-Capture mit dokumentierten Channel-Allocation-Element-Bits
   (8+ bits in MAC-RESOURCE ca_flag=1 Pfad). **Action:** Phase G TBD —
   Group-Call CMCE-Capture aufnehmen + bit-extrahieren.

5. **Alle CMCE-PDU-Felder (D-SETUP / U-SETUP / D-CALL-PROCEEDING /
   D-CONNECT / U-TX-DEMAND / D-TX-GRANTED / U-RELEASE / D-RELEASE).**
   PROVISIONAL aus bluestation-Defaults + ETSI §14.7. Nicht Gold-
   verifiziert. **Action:** Group-Call CMCE-Phase aufnehmen, bit-
   extrahieren, bestätigen oder korrigieren.

6. **TmaSap `req_handle` Wertebereich.** bluestation hat `Todo` (= i32).
   Wir wollen `u32` monoton incrementing. **Action:** Im Daemon-Code
   konkret `u32` festschreiben + monotonic counter mit wraparound nach
   ~2^32. Kein Air-Bezug, nur API-Stabilität.

7. **Subscriber_class auf-Air encoding für D-LOC-UPDATE-ACCEPT.**
   Bluestation deklariert `subscriber_class: Option<u64>` mit 16 bit.
   Gold setzt `p_subscriber_class=0` ⇒ kein subscriber_class on-air.
   ETSI §16.10.41 ("Subscriber class") definiert es als 16-bit-Bitmap.
   Wenn unsere BS in Zukunft subscriber_class senden möchte, muss der
   Wert per Cell-Configuration setzbar sein — kein Gold-Wert verfügbar.

---

## How to apply

1. **Encoder/Decoder-Implementierung** in `sw/mm/`, `sw/cmce/`, `sw/mle/`
   nutzt diese Tabelle als Source-of-Truth für Default-Werte. Wo der
   Wert "PROVISIONAL" ist, muss der Encoder einen Konfigurations-Hook
   haben statt einer hartkodierten Konstante.
2. **TBs gegen Gold-Bit-Vektoren** verifizieren MM-PDUs (M2/M3) bit-genau.
   CMCE-PDUs werden in Phase G nachzuverifizieren sein.
3. **Open uncertainties** sind Hard-Stop für Phase H.7 (D-NWRK-BCAST) und
   Phase G (Group-Call). Ohne Klarstellung droht Bit-Drift gegen Gold.
4. **Bei jeder Encoder-Änderung** → diese Datei zuerst lesen. Bei
   Konflikt zwischen Memo-Quellen → Gold > Bluestation > ETSI per
   CLAUDE.md.
