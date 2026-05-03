---
name: Group-Attach (U/D-ATTACH-DETACH-GROUP-IDENTITY) — bit-exakt aus externer BS-Capture
description: 2026-04-26 Gold-Ref-Capture externe BS @ 392.9875 MHz (MCC=262 MNC=1010 CC=1, externe MS ISSI=0x282FF4 — Vendor unbekannt). 3 Group-Wechsel inkl. erstem deny-retry-Erfolg. UL: U-ATTACH-DETACH-GRP-ID frag=1 + MAC-END-HU + BL-ACK; DL: D-ATTACH-DETACH-GRP-ID-ACK single-burst SCH/F mit GroupIdentityDownlink-IE.
type: reference
originSessionId: b85ff25e-2a77-47a3-a8ad-a1f569ece2e0
---
**VERBINDLICH bit-exakt — Gold-Ref-Capture für Phase 7 F.7.**
Quelle: `UL_baseband_382891062Hz_17-04-31_26-04-2026.wav` + `DL_baseband_392984000Hz_17-04-30_26-04-2026.wav`. Dekodiert mit `--mcc 262 --mnc 1010 --cc 1`.

## Goldregel-Hierarchie (siehe `reference_gold_attach_bitexact.md`)

Gold-Ref > Bluestation > ETSI. Diese Datei ist die authoritative Quelle für F.7-Encoder.

## UL-Sequenz (MS → BS)

### UL-Demand Fragment 1 (MAC-ACCESS, frag=1, mm_type=7)

```
hex: 01 41 7F A7 01 17 38 08 21 20 5E 90    (NS=0)
hex: 01 41 7F A7 01 97 38 08 21 20 5E 90    (NS=1)
```

Bit-Layout (92 bit MAC-ACCESS-PDU):
```
[ 0]      pdu_type            = 0   (MAC-ACCESS)
[ 1]      fill_bit            = 0
[ 2]      encrypted           = 0
[ 3..4]   addr_type           = 00  (Ssi/ISSI)
[ 5..28]  ssi                 = 0x282FF4 (= 2 633 716, externe MS-ISSI)
[29]      opt_field           = 1
[30]      length_or_cap_req   = 1   (cap_req mode)
[31]      frag_flag           = 1   ← signalisiert Continuation
[32..35]  reservation_req     = 0000
[36..39]  LLC_type            = 0001 (BL-DATA)
[40]      NS                  = 0 oder 1 (alterniert pro Round)
[41..43]  MLE_disc            = 001 (MM)
[44..47]  mm_type             = 0111 (= 7, UAttachDetachGroupIdentity)
[48..91]  MM-Body Fragment 1  = 44 bit (siehe Reassembly)
```

### UL-Demand Fragment 2 (MAC-END-HU on SCH/HU)

```
hex: 8D 58 F0 5E 9A C4 41 41 7F A4 08 00    (Variante 1)
hex: 8D 58 B0 5E 9A C6 41 41 7F A4 08 00    (Variante 2 — andere Group)
hex: 8D 58 70 5E 9A C4 41 41 7F A4 08 00    (Variante 3 — andere Group)
```

Bit-Layout (92 bit MAC-END-HU per `umac/pdus/mac_end_hu.rs`):
```
[ 0]      mac_pdu_type            = 1   (1-bit, MAC-END-HU)
[ 1]      fill_bits               = 0
[ 2]      length_ind_or_cap_req   = 0   (length_ind branch)
[ 3..6]   length_ind              = 4 bit (Octets-Anzahl im payload, hier ~13)
[ 7..91]  MM-Body Fragment 2      = 85 bit
```

### UL Reassembly (Phase 7 F.1 greift)

```
full_mm_body[0..128] = ul0_bits[48..91] (44) ++ ul1_bits[7..91] (85) = 129 bit
```

### UL-Demand MM-Body Layout (129 bit, per `u_attach_detach_group_identity.rs`)

```
[ 0..  3] pdu_type              = 0111 (=7, redundant zu mm_type-Field oben)
[ 4]      group_identity_report      (1 bit)
[ 5]      attach_detach_mode         (1 bit, 0=amendment, 1=replace-all)
[ 6]      o-bit                      (1 bit, falls 1: optional fields folgen)
            if o-bit:
[ 7]        m_group_report_response  (Type-3 m-bit)
              if m: elem_id(4) + length(11) + payload (Type-3 generic)
[..]        m_group_identity_uplink  (Type-4 m-bit)
              if m: elem_id(4)=`1000` + length(11) + num_elems(6) + N×GIU-struct
[..]        m_proprietary            (Type-3 m-bit)
[..]      trailing m-bit             (= 0 normalerweise)
```

GroupIdentityUplink-Struct (per `mm/fields/group_identity_uplink.rs`):
```
[+0]      attach_detach_type_id      (1 bit, 0=attach, 1=detach)
            if attach (=0):
[+1..+3]    class_of_usage             (3 bit)
            if detach (=1):
[+1..+2]    group_identity_detachment  (2 bit)
[..]      address_type               (2 bit, 0=GSSI, 1=GSSI+AE, 2=VGSSI)
            if addr_t in {0,1}:
[..+24]     gssi                      (24 bit) ← die GSSI-Wunsch-Liste
            if addr_t == 1:
[..+24]     address_extension         (24 bit)
            if addr_t == 2:
[..+24]     vgssi                     (24 bit)
```

### UL-BL-ACK (nach DL-ACK von BS)

```
hex: 41 41 7F A4 63 C0 41 41 7F A4 08 00    (NR=1)
hex: 41 41 7F A4 63 40 41 41 7F A4 08 00    (NR=0)
```

MAC-ACCESS pdu_type=0 fill=1, addr=SSI ID=2633716, LI=6 (= LLC-BL-ACK). NR alterniert mit jedem Round-Trip.

## DL-Sequenz (BS → MS)

### D-ATTACH-DETACH-GRP-ID-ACK (single-burst SCH/F)

**5 Bit-Slices aus der Gold-Ref-Capture** (alle `accept_reject=0` ACCEPT, alterierende NR/NS):

```
#1 (#687  TN=1 FN=17 MN=22, NR=0 NS=1):
   0010000010000001001010000010111111110100010000000000000010011011001101110000010011000000100110000001011110100110101100011010

#2 (#1447 TN=1 FN=09 MN=33, NR=1 NS=0):
   0010000010000001001010000010111111110100010000000000000100011011001101110000010011000000100110000001011110100110101100010010

#3 (#2111 TN=1 FN=??     , NR=0 NS=1):
   0010000010000001001010000010111111110100010000000000000010011011001101110000010011000000100110000001011110100110101100001010

#4 (#2487 TN=1 FN=17 MN=47, NR=1 NS=0): identisch mit #2 (modulo MN/FN-State)
#5 (#2787 TN=1 FN=02 MN=52, NR=0 NS=1): identisch mit #1
```

### DL-ACK Layout (124 bit MAC-RESOURCE + LLC + MLE + MM body)

```
MAC-RESOURCE Header (43 bit + addr-block):
[ 0..1]   pdu_type            = 00      (MAC-RESOURCE)
[ 2]      fill_bit            = 1
[ 3]      PoG                 = 0
[ 4..5]   encryption          = 00
[ 6]      random_access_flag  = 0       (kein RA, regulärer Reply)
[ 7..12]  length_indication   = 16      (LI=16 octets = 128 bit)
[13..15]  address_type        = 001     (SSI)
[16..39]  SSI                 = 0x282FF4 (= MS-ISSI, gleicher der Demand)
[40]      pwr_flag            = 0
[41]      slot_grant_flag     = 1
[42..49]  slot_grant_elem     = 0x00    (kein Voice-Slot)
[50]      ca_flag             = 0
[51..54]  LLC_type            = 0000    (BL-ADATA)

LLC BL-ADATA:
[55]      NR                  = 0 oder 1 (Stop-and-Wait, alterniert)
[56]      NS                  = 1 oder 0 (alterniert, gegenphasig)

MLE:
[57..59]  MLE_disc            = 001     (MM)
[60..63]  mm_pdu_type         = 1011    (= 11, DAttachDetachGroupIdentityAcknowledgement)
                                        // KORRIGIERT 2026-04-26: Goldregel
                                        // — Bit-Forensik aller 5 Slices +
                                        // Bluestation MmPduTypeDl::DAttach
                                        // DetachGroupIdentityAcknowledgement
                                        // = 11. Vorherige Doku (=8) war Drift.

MM body (D-ATTACH-DETACH-GRP-ID-ACK, per `d_attach_detach_group_identity_acknowledgement.rs`):
[64]      group_identity_accept_reject = 0 (= ACCEPT)  oder 1 (= REJECT)
[65]      reserved                      = 0
[66]      o-bit                         = 1 (optional fields follow)
            if o-bit:
[67]        p_proprietary               (Type-3 p-bit)
              if p_prop=1: elem_id(4) + length(11) + Type-3 payload
[..]        m_group_identity_downlink   (Type-4 m-bit)
              if m_gid=1:
                elem_id(4) = 0111       (= 7, GroupIdentityDownlink)
                length(11)
                num_elems(6)
                N × GroupIdentityDownlink-struct
[..]        m_grp_security              (Type-4 m-bit)
[..]      trailing m-bit                = 0

GroupIdentityDownlink-struct (per `mm/fields/group_identity_downlink.rs`):
[+0]      attach_detach_type_id          (1 bit, 0=attach, 1=detach)
            if attach (=0):
[+1..+2]    lifetime                     (2 bit, GroupIdentityAttachment)
[+3..+5]    class_of_usage               (3 bit)
            if detach (=1):
[+1..+2]    group_identity_detachment    (2 bit)
[..]      address_type                   (2 bit, 0=GSSI, 1/3=+AE, 2=VGSSI)
            if addr_t in {0,1,3}:
[..+24]     gssi                         (24 bit)
            if addr_t in {1,3}:
[..+24]     address_extension            (24 bit)
            if addr_t in {2,3}:
[..+24]     vgssi                        (24 bit)
```

### AACH-Pattern um den Reply-Frame

Beim D-ATTACH-DETACH-GRP-ID-ACK-Burst (TN=1):
```
AACH [DL/UL-Assign] DL=Unalloc UL=Unalloc CC=9 f1=0 f2=9
```

→ **Kein dedizierter Slot-Grant** für UL-Continuation. MS sendet Fragment 2 im RA-Slot ohne Grant. F.7 braucht keine AACH-Erweiterung.

## Sequenz-Diagramm (3-Way-Handshake pro Group-Switch)

```
MS (extern)              BS (extern)
   │                          │
   │ ── UL#0 frag=1 mm=7 ──>  │  (MAC-ACCESS, NS=0)
   │ ── UL#1 MAC-END-HU ────> │  (Continuation, GroupIdentityUplink-IE)
   │                          │
   │ <── DL D-ATTACH-DETACH-GRP-ID-ACK (SCH/F, NR=0 NS=1, accept_reject=0)
   │      mit GroupIdentityDownlink-Liste
   │                          │
   │ ── UL BL-ACK NR=1 ────>  │  (bestätigt DL-ACK, RX-Window-Update)
   │                          │
   │   AST.group_list_local update; nächster Switch:
   │                          │
   │ ── UL#0 frag=1 mm=7 ──>  │  (NS=1 dieses Mal)
   │ ── UL#1 MAC-END-HU ────> │
   │ <── DL D-ATTACH-DETACH-GRP-ID-ACK (NR=1 NS=0)
   │ ── UL BL-ACK NR=0 ────>  │
```

T0-Timeout zwischen UL#0 und UL#1: ≤ 2 Frames (113 ms) — F.1 default reicht.
Zeit zwischen UL#1 und DL-ACK: typisch < 1 Frame.
Zeit zwischen DL-ACK und UL-BL-ACK: typisch 1-2 Frames.

## Why
M2 Attach (mm_type=2) und Group-Switch (mm_type=7) nutzen den gleichen
2-Burst-Reassembly-Mechanismus, aber unterschiedliche MM-Body-Layouts +
Reply-Path. Phase 7 F.7 muss diesen zweiten Pfad bauen ohne den Phase 7
F-Stand für mm_type=2 zu verändern.

## How to apply
- Memory `reference_demand_reassembly_bitexact.md` Reassembly-Formel ist mm-type-agnostisch und bleibt verbindlich.
- F.7 IE-Parser-Erweiterung: nach Reassembly Body[0..3] checken, falls = 0111 → ATTACH-DETACH-Body-Walker (anderes Layout als U-LOC-UPDATE-DEMAND).
- F.7 D-ATTACH-DETACH-GRP-ID-ACK-Encoder: produziert 124-bit MM-RESOURCE-wrapped SCH/F, accept_reject aus AST-State, GroupIdentityDownlink-Liste aus akzeptierter GSSI-Liste (= post-permit-Loop in MLE-FSM).
- NR/NS pro AST-Slot tracken (alterniert; M2-Stand hatte das schon, einfach bei mm_type=7 mit verwenden).
- Air-Test: MTP3550 auf eigener Cell, EntityTable vorbereiten mit Profile.permit für die gewünschten GSSIs, MMI-Group-Switch durchschalten — sollte alle als grün/bereit zeigen wenn DL-ACK rechtzeitig kommt.
