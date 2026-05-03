---
name: CMCE Group-Call PDU Bit-Layouts (U/D-SETUP, D-CALL-PROCEEDING, D-CONNECT, D-TX-GRANTED, U-TX-DEMAND, U/D-RELEASE)
description: Bit-genaue Reference für M3 Group-Call-Implementation. Quelle bluestation `tetra-pdus/src/cmce/pdus/*.rs` + ETSI EN 300 392-2 §14. Inkl. Sequenz-Diagramm BS-Initiated und MS-Initiated Group-Call.
type: reference
originSessionId: b85ff25e-2a77-47a3-a8ad-a1f569ece2e0
---
**VERBINDLICHE BIT-REFERENZ für M3 (Phase G).** CMCE-PDU-Layouts pro bluestation + ETSI §14.7.

## CMCE PDU-Type-Tabelle (5 bit, §14.8.28)

### DL (BS → MS)

| Code | Name | Inhalt | Verwendung |
|------|------|--------|-----------|
| 0  | D-ALERT                  | call ringing | nach D-SETUP wenn called party-MS verfügbar |
| 1  | D-CALL-PROCEEDING        | "call wird verarbeitet" | Antwort auf U-SETUP, vor D-CONNECT |
| 2  | D-CONNECT                | "through-connect" | Voice-Slot ist allocated |
| 3  | D-CONNECT-ACKNOWLEDGE    | Bestätigung | optional |
| 4  | D-DISCONNECT             | call beendet | |
| 5  | D-INFO                   | Mid-Call-Info | |
| 6  | D-RELEASE                | endgültiges Aufräumen | |
| 7  | D-SETUP                  | "Anruf für dich" | BS pusht zu called-MS |
| 8  | D-STATUS                 | precoded-status | SDS-Vorgänger |
| 9  | D-TX-CEASED              | "Sender hat aufgehört" | nach U-TX-CEASED |
| 10 | D-TX-CONTINUE            | extend talk-time | |
| 11 | **D-TX-GRANTED**         | "du darfst senden" | nach U-TX-DEMAND oder im Setup |
| 12 | D-TX-WAIT                | grant queued | |
| 13 | D-TX-INTERRUPT           | grant verloren | |
| 14 | D-CALL-RESTORE           | call restoration | |
| 15 | D-SDS-DATA               | Short-Data-Service | |
| 16 | D-FACILITY               | facility extension | |
| 31 | CMCE-FUNC-NOT-SUPPORTED  | catch-all reject | |

### UL (MS → BS)

| Code | Name | Inhalt | Verwendung |
|------|------|--------|-----------|
| 0  | U-ALERT                  | MS klingelt | |
| 2  | U-CONNECT                | "ich nehme an" | nach D-SETUP von called-MS |
| 4  | U-DISCONNECT             | hang-up | |
| 5  | U-INFO                   | mid-call-info | |
| 6  | U-RELEASE                | release-ack | nach D-DISCONNECT |
| 7  | **U-SETUP**              | "ich rufe an" | MS-initiated Group-Call-Start |
| 8  | U-STATUS                 | precoded-status | |
| 9  | U-TX-CEASED              | MS hat aufgehört zu senden | |
| 10 | **U-TX-DEMAND**          | "ich will senden" | jeder MS-Push-to-Talk |
| 14 | U-CALL-RESTORE           | | |
| 15 | U-SDS-DATA               | | |
| 16 | U-FACILITY               | | |
| 31 | CMCE-FUNC-NOT-SUPPORTED  | | |

## Type-3 Element-IDs (CMCE Optional Fields, `type3_elem_id.rs`)

| ID | Name |
|----|------|
| 0  | ExtSubscriberNum |
| 1  | Facility |
| 2  | DmMsAddr |
| 3  | Proprietary |

## Common-Felder

### TransmissionGrant (2 bit, §14.8.42)
- 0 Granted
- 1 NotGranted
- 2 RequestQueued
- 3 GrantedToOtherUser

### CallTimeout (4 bit, §14.8.4)
- 0 Infinite
- 15 Reserved
- (1..14: timeout-Werte in 5-s-Schritten oder ähnlich, ETSI-spezifisch)

### CallTimeoutSetupPhase (3 bit)
- 0..7 setup-phase timeouts

### DisconnectCause (5 bit, §14.8.13)
- 0..23 (siehe Memory-Detail unten)

### CallStatus (3 bit, §14.8.6)
- progress states während Setup

### PartyTypeIdentifier (2 bit, §14.8.32)
- 0 Sna (Short-Number-Address, 8 bit)
- 1 Ssi (24 bit)
- 2 Tsi (= Ssi + Address-Extension, 24+24 bit)
- 3 reserved

### BasicServiceInformation (8..10 bit, §14.8.2)
```
[0..2]  circuit_mode_type        = 3 bit
        (CircuitModeType enum: TchS=0, TchSpeech, TchData_2.4kbps, ..., TchSpeech_TchS_Variant_etc)
[3]     encryption_flag           = 1 bit (0=clear, 1=E2EE)
[4..5]  communication_type        = 2 bit
        (0=p2p, 1=p2multi, 2=p2multi-acked, 3=broadcast)
if circuit_mode_type == TchS:
[6..7]  speech_service           = 2 bit (0=TETRA encoded speech, 3=proprietary)
else:
[6..7]  slots_per_frame          = 2 bit (0=1slot, 1=2slots, 2=3slots, 3=4slots)
```

## D-SETUP (DL → called MS) — Clause 14.7.1.12

```
[ 0..  4] pdu_type             = 00111 (=7)
[ 5.. 18] call_identifier      = 14 bit
[19.. 22] call_time_out        = 4 bit (CallTimeout)
[23]      hook_method_selection = 1 bit
[24]      simplex_duplex_selection = 1 bit
[25.. 32] basic_service_information = 8 bit (BSI)
[33.. 34] transmission_grant   = 2 bit
[35]      transmission_request_permission = 1 bit
[36.. 39] call_priority        = 4 bit
[40]      o-bit (optional fields) = 1 bit
            if o-bit:
[41]        m-bit
[42]        p_notification_indicator
            if p_ni: [..+5] notification_indicator (6 bit)
            p_temporary_address (Type-2)
            if p_ta: [..+23] temporary_address (24 bit)
            calling_party_type_identifier (2 bit, calling_party SSI/TSI/SNA)
            calling_party_address_ssi (24 bit if cpti==1 || cpti==2)
            calling_party_extension (24 bit if cpti==2)
            Type-3: ExtSubscriberNum, Facility, DmMsAddr, Proprietary
[ ..]     trailing m-bit = 0
```

## U-SETUP (UL → BS, MS-initiated) — Clause 14.7.2.10

```
[ 0..  4] pdu_type             = 00111 (=7)
[ 5..  8] area_selection       = 4 bit (0 = no SS-AS)
[ 9]      hook_method_selection = 1 bit
[10]      simplex_duplex_selection = 1 bit
[11.. 18] basic_service_information = 8 bit
[19]      request_to_transmit_send_data = 1 bit
[20.. 23] call_priority        = 4 bit
[24.. 25] clir_control         = 2 bit
[26.. 27] called_party_type_identifier = 2 bit (PartyTypeIdentifier)
            if cpti == Sna:    [28..35] called_party_short_number_address = 8 bit
            if cpti == Ssi:    [28..51] called_party_ssi = 24 bit (← die GSSI bei Group-Call)
            if cpti == Tsi:    [28..51] called_party_ssi + [52..75] extension = 24+24 bit
[ ..]     o-bit + Type-3 (ExtSubscriberNum, Facility, DmMsAddr, Proprietary)
[ ..]     trailing m-bit = 0
```

## D-CALL-PROCEEDING — Clause 14.7.1.2

```
[ 0..  4] pdu_type             = 00001 (=1)
[ 5.. 18] call_identifier      = 14 bit
[19.. 21] call_time_out_set_up_phase = 3 bit
[22]      hook_method_selection = 1 bit
[23]      simplex_duplex_selection = 1 bit
[24]      o-bit
            if o-bit:
              p_basic_service_information (Type-2, 8 bit if present)
              p_call_status (Type-2, 3 bit)
              p_notification_indicator (Type-2, 6 bit)
              Type-3: Facility, Proprietary
[ ..]     trailing m-bit = 0
```

## D-CONNECT — Clause 14.7.1.4

```
[ 0..  4] pdu_type             = 00010 (=2)
[ 5.. 18] call_identifier      = 14 bit
[19.. 22] call_time_out        = 4 bit
[23]      hook_method_selection = 1 bit
[24]      simplex_duplex_selection = 1 bit
[25.. 26] transmission_grant   = 2 bit
[27]      transmission_request_permission = 1 bit
[28]      call_ownership       = 1 bit
[29]      o-bit
            if o-bit:
              p_call_priority (Type-2, 4 bit)
              p_basic_service_information (Type-2, 8 bit)
              p_temporary_address (Type-2, 24 bit)
              p_notification_indicator (Type-2, 6 bit)
              Type-3: Facility, Proprietary
[ ..]     trailing m-bit = 0
```

## D-TX-GRANTED — Clause 14.7.1.15

```
[ 0..  4] pdu_type             = 01011 (=11)
[ 5.. 18] call_identifier      = 14 bit
[19.. 20] transmission_grant   = 2 bit
[21]      transmission_request_permission = 1 bit
[22]      encryption_control   = 1 bit
[23]      reserved             = 1 bit (= 0)
[24]      o-bit
            if o-bit:
              p_notification_indicator (Type-2, 6 bit)
              p_transmitting_party_type_identifier (Type-2, 2 bit)
              p_transmitting_party_address_ssi (24 bit if tpti==1||2)
              p_transmitting_party_extension (24 bit if tpti==2)
              Type-3: ExtSubscriberNum, Facility, DmMsAddr, Proprietary
[ ..]     trailing m-bit = 0
```

## U-TX-DEMAND — Clause 14.7.2.12

```
[ 0..  4] pdu_type             = 01010 (=10)
[ 5.. 18] call_identifier      = 14 bit
[19.. 20] tx_demand_priority   = 2 bit
[21]      encryption_control   = 1 bit
[22]      reserved             = 1 bit (= 0)
[23]      o-bit
            Type-3: Facility, DmMsAddr, Proprietary
[ ..]     trailing m-bit = 0
```

## U/D-RELEASE — Clause 14.7.2.9 / 14.7.1.9

```
[ 0..  4] pdu_type             = 00110 (=6)
[ 5.. 18] call_identifier      = 14 bit
[19.. 23] disconnect_cause     = 5 bit (DisconnectCause)
[24]      o-bit
            Type-3: Facility, Proprietary
[ ..]     trailing m-bit = 0
```

## Sequenz-Diagramme

### Group-Call MS-Initiated (PTT)

```
CallingMS                    BS                        CalledGroup-MSes
   |                          |                                |
   | --- U-SETUP (called=GSSI, basic_service=TchS) -->         |
   |                          |                                |
   |                          | --- D-SETUP (call_id, BSI, transmission_grant=Granted) -> (broadcast to all GSSI members)
   |                          | --- D-CONNECT (transmission_grant=Granted) ---> (broadcast)
   |                          |                                |
   | <-- D-CONNECT (call_id, transmission_grant=Granted) ----- |
   |                          |                                |
   | -- Voice-Bursts (NUB, TCH/S, on allocated DL+UL slot) --- |
   |                          |                                |
   |                          | -- Voice-Relay UL→DL ---------> |  (Empfänger hören)
   |                          |                                |
   | --- U-TX-CEASED -------->|                                |
   |                          | --- D-TX-CEASED ------------>  |
   |                          |                                |
   | --- U-RELEASE (cause=UserRequested) -->                   |
   |                          | --- D-RELEASE -------------->  |
   |                          |                                |
```

### Group-Call BS-Pulled (Dispatcher) — selten, nicht für M3 Pflicht

```
Dispatcher → BS → D-SETUP an alle GSSI-Mitglieder → CalledMS antwortet U-CONNECT
→ D-TX-GRANTED an Dispatcher → Voice-Relay
```

### Standby-State (kein Call aktiv)

```
MS bleibt eingebucht (AST.state = REG, last_seen wird auf jeder UL-Aktivität refresht).
BS hat keinen call_id für diese MS aktiv.
TTL-Sweep evictet AST-Slot nach REG_AST_TTL_MULTIFRAMES (Phase C).
```

## Wichtige Implementations-Hinweise

1. **call_identifier**: 14 bit, BS allocated. Pro aktivem Call eindeutig. AST.state=2 (CALL_SETUP) oder =3 (VOICE) trägt call_id im Reserved-Feld (z.B. group_count-Bits umfunktionieren oder neue AST-Layout-Erweiterung in Phase H).
2. **basic_service_information.circuit_mode_type=TchS** (=0) für Sprache.
3. **Voice-Burst-Layout**: NUB 432-bit half-block, TCH/S Channel-Coding (siehe ETSI §8.4 + §9.4.4.3.2).
4. **Voice-Relay**: bit-transparent UL-NUB → DL-NUB, gleicher Slot, ein TDMA-Frame Latenz. ACELP-Decode/Encode NICHT nötig (siehe CLAUDE.md §12).
5. **AACH-Slot-Grant** für aktiven Call: AACH muss "Allocated, slot=N, ssi=GSSI" zeigen statt "Random/Common", damit MS weiß sie soll dort auf NUB hören.
6. **U-SETUP-Reassembly**: U-SETUP kann je nach optional fields > 92 bit werden. Typischer minimaler U-SETUP für GSSI-Call ist 32+24+padding ≈ 70 bit, passt in einen Burst. Aber sicherheitshalber Reassembly im UL-Parser haben (kommt eh aus Phase F).

## Offene Punkte für Phase F+G Spec-Inventur

- **ETSI §14.7.1.x Clauses für genaue Bit-Positionen** der p_*-Bits in Optional-Field-Sektionen — bluestation-Kommentare reichen für Standard-Path, aber Edge-Cases (Encrypted, Acknowledged-Group-Call) brauchen ETSI-Cross-Check.
- **Voice-Burst-Format Detail** (TCH/S half-block 432 bit, stealing-bit-Scheme, training-sequence-Position) — erfordert §9.4.4.3.2 Lesen.
- **Talker-Schedule** (wer talker ist während des Calls, wer audio-empfängt — entscheidet wo der Voice-Relay-FIFO seinen Eingang vs Ausgang hat).

## Why
Phase G (Group-Call) braucht alle obigen Layouts bit-identisch. M2 hat gezeigt: Bit-Forensik gegen Gold-Ref-Capture ist die einzige verbindliche Spec, weil ETSI in jedem Optional-Field-Detail Spielraum lässt und MS-Implementierungen leicht abweichen. Memory-Reference ist die "gefrorene Spec" gegen die TBs und RTL geprüft werden.

## How to apply
- Phase F (Reassembly + IE-Parser) ist Voraussetzung — siehe `reference_demand_reassembly_bitexact.md`.
- Phase G nutzt diese Reference als Source-of-Truth für alle CMCE-PDU-Encoder/Decoder + MLE-FSM-State-Erweiterungen.
- TBs für jeden PDU-Type bit-genau gegen die obigen Layouts.
- Air-Test: idealerweise mit zwei MTP3550-Geräten + Live-WAV-Capture beider DL+UL gleichzeitig, dann CMCE-PDUs in der Capture mit `decode_dl.py` / `decode_ul.py` (Phase G erweitert beide um CMCE-Decoder) verifizieren.
