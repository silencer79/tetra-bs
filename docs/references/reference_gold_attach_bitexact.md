---
name: Gold-Reference Attach Sequenz — bit-exakt
description: Externe TETRA-BS @ 392.9875 MHz, MS ISSI=0x282FF4, 2026-04-25 erfolgreicher Attach. Bit-genau verbindlich, NICHT abweichen
type: reference
originSessionId: b85ff25e-2a77-47a3-a8ad-a1f569ece2e0
---
**Capture:** `docs/references/captures_external_bs_2026-04-25/`
DL `baseband_393084625Hz_00-11-52_25-04-2026.wav` + UL `baseband_382468718Hz_00-11-50_25-04-2026.wav`

ISSI der externen MS = `0x282FF4` (= 2 633 716). Bluestation `MmPduTypeUl` ist authoritative für UL, `MmPduTypeDl` für DL.

## Goldregel (Kevin 2026-04-26): Gold > Bluestation > ETSI

Bei Widerspruch zwischen Spec-Quellen ist diese Capture (und alle
abgeleiteten `reference_*_bitexact.md`-Memory-Files) **authoritativ**.
Bluestation gilt nur, wenn Gold-Ref schweigt. ETSI als Tie-Breaker.

Begründung: M2 wurde gegen genau diese Real-World-Capture bit-genau
verifiziert und produziert auf-Air MS-Registration-OK. Andere Quellen
sind Plan, kein Beweis.

Bei Konflikt: Hex-Slice aus dieser Datei steht — **STOP**, melden,
nicht raten.

## UL-Sequenz (External MS → External BS)

### UL#0 (Demand-Fragment) — t=00:12:01.36, hex `01 41 7F A7 01 12 66 34 20 C1 22 60`

```
[0]      mac_pdu_type        = 0      (MAC-ACCESS)
[1]      fill_bit            = 0
[2]      encrypted           = 0
[3..4]   addr_type           = 00     (Ssi/ISSI, 24-bit)
[5..28]  ISSI                = 0x282FF4
[29]     opt_field_flag      = 1
[30]     choice              = 1      (1=frag+resreq, 0=length_ind)
[31]     frag_flag           = 1      ← Fragment, MAC-U-BLCK Fortsetzung folgt
[32..35] reservation_req     = 0      (4 bit)
[36..39] LLC pdu_type        = 0001   (= 1 BL-DATA, 4-bit ETSI Tab 22.1)
[40]     N(S)                = 0
[41..43] MLE-PD              = 001    (= 1, MM)
[44..47] MM_pdu_type         = 0010   (= 2 = U-LOCATION-UPDATE-DEMAND per MmPduTypeUl)
[48..91] MM body fragment 1  (48 bits — Demand-Body Fortsetzung in UL#1)
```

### UL#1 (MAC-U-BLCK) — t=00:12:01.41, hex `D4 1C 3C 02 40 50 2F 4D 61 20 00 00`

```
[0]    mac_pdu_type = 1     (MAC-FRAG/MAC-U-BLCK)
[1]    fill_bit     = 1
[2..3] sub-type     = 01    (top_nibble=3 → MAC-U-BLCK)
[4..91] 88 bits MM-Demand-Fortsetzung (von UL#0 Fragment)
```

→ Externe BS reassembliert UL#0[48..91] + UL#1[4..91] zum vollen U-LOCATION-UPDATE-DEMAND MM body.

### UL#2 (BL-ACK auf den DL Accept) — t=00:12:01.53, hex `41 41 7F A4 63 40 41 41 7F A4 08 00`

```
[0..28]  MAC-ACCESS Header analog UL#0, ISSI=0x282FF4
[29]     opt_field_flag      = 1
[30]     choice              = 0    (length_ind)
[31..35] length_ind          = 6    (5 bit)
[36..39] LLC pdu_type        = 0011 (= 3 = BL-ACK)
[40]     N(R)                = 0
[41..43] MLE-PD              = 100  (= 4 SNDCP / Padding)
```

→ Quittierung des DL BL-DATA NS=0 von der BS (DL#735).

## DL-Sequenz (External BS → External MS)

### DL#727 (SCH/HD Pre-Reply) — t=00:12:02.30, AACH=DL/UL-Assign Unalloc/Unalloc CC=9 f1=0 f2=9

```
MAC-RESOURCE Header (43 bit + addr-block + presence):
  [0..1]    pdu_type        = 00      (MAC-RESOURCE)
  [2]       fill_bit        = 1
  [3]       PoG             = 0
  [4..5]    encryption      = 00
  [6]       random_access_flag = 1    ← RA-Ack
  [7..12]   length_indication = 7     (LI = 7 octets)
  [13..15]  address_type    = 001     (SSI)
  [16..39]  SSI             = 0x282FF4
  [40]      pwr_flag        = 0
  [41]      slot_grant_flag = 1
  [42..49]  slot_grant_elem = 0x00    (cap_alloc=0, granting_delay=0)
  [50]      ca_flag         = 0
TM-SDU bei bit 51:
  [51..54]  LLC pdu_type    = 1000    (= 8 AL-SETUP, kein MLE/MM body)
  [55]      fill bit start = 1
+ pad zu 56 bit (= 7 octets ✓)
```

### DL#735 (SCH/F D-LOC-UPDATE-ACCEPT) — t=00:12:02.40, AACH=DL/UL-Assign Unalloc/Unalloc CC=9 f1=0 f2=9

```
MAC-RESOURCE Header:
  [0..1]    pdu_type        = 00
  [2]       fill_bit        = 1
  [3]       PoG             = 0
  [4..5]    encryption      = 00
  [6]       random_access_flag = 0    ← NICHT 1! (RA war via DL#727 schon ge-acked)
  [7..12]   length_indication = 21    (LI = 21 octets)
  [13..15]  address_type    = 001
  [16..39]  SSI             = 0x282FF4
  [40]      pwr_flag        = 0
  [41]      slot_grant_flag = 1
  [42..49]  slot_grant_elem = 0x00
  [50]      ca_flag         = 0
TM-SDU bei bit 51 (= 112 bits = 14 octets, davon LLC+MLE+MM):
  [51..54]  LLC pdu_type    = 0000    (= 0 BL-ADATA, kein FCS)
  [55]      N(R)            = 0
  [56]      N(S)            = 0
  [57..59]  MLE-PD          = 001     (MM)
  [60..63]  mm_pdu_type     = 0101    (= 5, D-LOCATION-UPDATE-ACCEPT per MmPduTypeDl)
  [64..66]  loc_acc_type    = 011     (= 3 ITSI attach)
  [67]      o-bit           = 1
  [68]      p_ssi           = 0       ← KEIN SSI im MM-Body
  [69]      p_address_extension = 0
  [70]      p_subscriber_class  = 0
  [71]      p_energy_saving_info = 1
  [72..85]  energy_saving_info = 14×0  (StayAlive: mode=0, FN=0, MN=0)
  [86]      p_scch_info_distrib_18 = 0
  [87]      m_bit (T3 follows)  = 1
  [88..91]  elem_id              = 0101 (= 5, GroupIdentityLocationAccept)
  [92..102] length              = 58 (11-bit, payload size in bits)
  [103..160] GILA payload (58 bit) — siehe ↓
  [161]     trailing m-bit (D-LOC-UPDATE-ACCEPT) = 0
+ pad zu 168 bit (21 octets, fill_bit_ind=1 setzt erstes fill bit auf 1)
```

### GroupIdentityLocationAccept payload (58 bit) — bit-exakt:

```
[0]      group_identity_accept_reject = 0   (accept)
[1]      reserved                     = 0
[2]      o-bit                        = 1   (Type-4 GroupIdentityDownlink list folgt)
[3]      m-bit (T4 GID-Downlink)      = 1
[4..7]   elem_id                      = 0111 (= 7, GroupIdentityDownlink)
[8..18]  length (= num_elems_bits + element_bits) = 38
[19..24] num_elems                    = 1   (6 bit)
[25..56] GroupIdentityDownlink entry  (32 bit):
  [25]      attach_detach_type_id     = 0   (group_identity_attachment)
  [26..27]  lifetime                  = 01  (= 1, "Attachment for next ITSI attach required")
  [28..30]  class_of_usage            = 100 (= 4)
  [31..32]  address_type              = 00  (= 0, gssi only — keine ae, keine vgssi)
  [33..56]  GSSI                      = 0x2F4D61 (= 3 100 001)
[57]     trailing m-bit (GILA)        = 0
```

**Total MM body bits = 102** (60..161 inkl., MM trailing m-bit consumed at [161]).

## Verbindliche Konsequenzen für unser RTL

| Feld | Gold-Ref-Wert | unser aktuelles RTL |
|------|---------------|---------------------|
| Accept MAC-RESOURCE `random_access_flag` | **0** | aktuell 1 (Bug, anpassen) |
| MM `p_ssi` | **0** (kein SSI im MM-Body — SSI steht im MAC-RESOURCE Header) | unser a092f90 hat 1 |
| MM `p_address_extension` | **0** | unser a092f90 hat 1 |
| MM `p_subscriber_class` | **0** | unser a092f90 hat 1 |
| MM `p_energy_saving_info` | **1** + `esi=0x0000` (StayAlive) | unser a092f90 hat 1 ✓ |
| MM `p_scch_info` | 0 | ✓ |
| MM Type-3 GILA | **PRÄSENT** mit GSSI=0x2F4D61, lifetime=1, class=4 | unser a092f90: nicht vorhanden |
| MM trailing m-bit | 0 | ✓ (immer 0) |
| MM body bits gesamt | **102** | unser a092f90: 100 |

## Wichtig: Reihenfolge in bluestation `to_bitbuf`

Bluestation `D-LOC-UPDATE-ACCEPT::to_bitbuf` ruft die Type-2-Felder mit `write_type2_generic`/`write_type2_struct` ALWAYS, aber:
- Wenn das Feld `None` → schreibt nur 1 P-bit = 0, KEINE 24/16/14 Daten-Bits.
- Wenn `Some` → schreibt P-bit=1 + Datenbits.

Ähnlich für Type-3/4: wenn `None` → schreibt NICHTS. Wenn `Some` → m-bit + id + length + payload.

**Daher: variable Bit-Position der nachfolgenden Felder!** Encoder muss das semantisch korrekt aufbauen, nicht mit festen Slot-Offsets.

## Why
Externe BS = einzige bekannte Real-World-Referenz mit nachweisbar erfolgreichem Attach. Eigene Improvisation hat 5+ Iterationen gekostet ohne Registration. Bit-exakt zur Gold-Ref ist die einzige verbindliche Spec für diesen Stack.

## How to apply
Bei jeder Änderung an D-LOC-UPDATE-ACCEPT, AL-SETUP-Pre-Reply, MAC-RESOURCE-Wrapper, oder UL-Parsing → diese Datei zuerst lesen. Nichts erfinden, nichts mischen, nichts „minimieren". Was hier steht ist auf-Air messbar gewesen und hat mit MS-Registration-OK korreliert.
