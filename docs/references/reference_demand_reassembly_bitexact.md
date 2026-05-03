---
name: UL-Demand-Reassembly bit-exakt — UL#0 (MAC-ACCESS frag) + UL#1 (MAC-U-BLCK) → 132-bit MM body
description: Bit-genaues Layout der zwei-Burst-U-LOC-UPDATE-DEMAND-Sequenz. Reassembly-Formel + GroupIdentityLocationDemand-IE-Position. Quelle: bluestation `mm/pdus/u_location_update_demand.rs` + `mm/fields/group_identity_location_demand.rs` + Gold-Ref-Capture 2026-04-25.
type: reference
originSessionId: b85ff25e-2a77-47a3-a8ad-a1f569ece2e0
---
**VERBINDLICHE BIT-REFERENZ für UL-Demand-Reassembly.** Wird bei Phase F gebraucht (UL-MAC-Parser + MLE-FSM-Erweiterung).

## On-Air-Burst-Sequenz

| Burst | Channel | mac_pdu_type | Header-Größe | Air-Bits | MM-Body-Anteil |
|-------|---------|--------------|--------------|----------|----------------|
| UL#0 | SCH/HU  | 0 (MAC-ACCESS, frag=1) | 36 bit (durch TL-SDU-Anfang) | 92 | bits[48..91] = **44 bit** |
| UL#1 | SCH/HU  | 1 (MAC-END-HU)         | 7 bit                          | 92 | bits[ 7..91] = **85 bit** |

**Reassembly:** `full_mm_body[0..128] = ul0_bits[48..91] ++ ul1_bits[7..91]` → **129 bit** MM body.

**Wichtig (Korrektur 2026-04-26 nach Phase-7-F.1-Audit):** Frühere Annahme
"UL#1 = MAC-FRAG-UL/MAC-U-BLCK auf SCH/F mit pdu_type=01 und payload[4..91]=88 bit"
war FALSCH. Richtig per bluestation `umac/pdus/mac_end_hu.rs` + SCH/HU-Dispatcher
in `umac_bs.rs`: SCH/HU akzeptiert nur **MAC-ACCESS** (mac_pdu_type=0, 1-bit) oder
**MAC-END-HU** (mac_pdu_type=1, 1-bit). Hex-Verifikation Gold-Ref UL#1 byte 0:
`D4` = `1101_0100` MSB-first → bit[0]=1 → MAC-END-HU. fill=1, length_ind=10.
MAC-FRAG-UL und MAC-U-BLCK sitzen auf STCH/SCH-F-Channels und sind hier irrelevant.

## UL#0 Layout (92 bit MAC-ACCESS-PDU mit Demand-Anfang)

```
[ 0]      pdu_type             = 00      (MAC-ACCESS)
[ 1]      fill_bit             = 0
[ 2]      encrypted            = 0
[ 3..4]   addr_type            = 00      (Ssi/ISSI)
[ 5..28]  ssi                  = 24 bit  (MS-ISSI)
[29]      optional_field_flag  = 1
[30]      length_or_cap_req    = 1       (cap_req mode)
[31]      frag_flag            = 1       ← signalisiert Continuation in nächstem Burst
[32..35]  reservation_req      = 4 bit
[36..39]  LLC_type             = 0001    (BL-DATA)
[40]      NS                   = 1 bit
[41..43]  MLE_disc             = 001     (MM)
[44..47]  mm_type              = 0010    (= 2, U-LOC-UPDATE-DEMAND per MmPduTypeUl)
[48..91]  MM-Body Fragment 1   = 44 bit  ← siehe MM-Body-Layout unten
```

## UL#1 Layout (92 bit MAC-END-HU Continuation auf SCH/HU)

Per bluestation `umac/pdus/mac_end_hu.rs`, ETSI EN 300 392-2 Clause 21.4.2.2:

```
[ 0]      mac_pdu_type             = 1   (1-bit, MAC-END-HU)
[ 1]      fill_bits                = 1 bit
[ 2]      length_ind_or_cap_req    = 1 bit (0 = length_ind, 1 = reservation_req)
          if length_ind_or_cap_req == 0:
[ 3.. 6]    length_ind             = 4 bit (octets following the header)
          if length_ind_or_cap_req == 1:
[ 3.. 6]    reservation_req        = 4 bit
[ 7..91]  MM-Body Fragment 2       = 85 bit  ← Anschluss an UL#0[48..91]
```

Hex-Verifikation Gold-Ref UL#1 erstes Byte:
- `D4` = `1101_0100` (MSB-first)
- bit[0]=1 → MAC-END-HU ✓
- bit[1]=1 → fill_bits=1
- bit[2]=0 → length_ind branch
- bits[3..6]=`1010`=10 → 10 octets payload
- bit[7]=0

**Tail-Pad:** falls MM-Body kürzer als 85 bit, wird mit 0-Bytes aufgefüllt.

## 129-bit MM-Body — vollständig nach Reassembly

Per ETSI EN 300 392-2 §16.10.21 + bluestation `u_location_update_demand.rs`:

```
[  0..  2] location_update_type        = 3 bit  (3 = ITSI-Attach)
[  3]      request_to_append_la         = 1 bit
[  4]      cipher_control               = 1 bit
[  5]      o-bit (optional fields)      = 1 bit
            if o-bit == 1:
[  6]        p_class_of_ms              = 1 bit
              if p_class == 1:
[  7..30]    class_of_ms                = 24 bit
[ 31]        p_energy_saving_mode       = 1 bit
              if p_esm == 1:
[ 32..34]    energy_saving_mode         = 3 bit
[ 35]        p_la_information           = 1 bit
              if p_la == 1:
[ 36..49]    la_information             = 14 bit
[ 50]        p_ssi                      = 1 bit
              if p_ssi == 1:
[ 51..74]    ssi                        = 24 bit
[ 75]        p_address_extension        = 1 bit
              if p_ae == 1:
[ 76..99]    address_extension          = 24 bit
[100]        m-bit (more_optional_bits) = 1 bit
              if m-bit == 1:
[101]        p_proprietary              = 1 bit
                if p_prop == 1: type-3 generic
[102]        p_group_identity_location_demand = 1 bit
              if p_gild == 1:
                GroupIdentityLocationDemand-IE:
                [  +0]      attach_detach_type_id  = 1 bit (0 = attach)
                [  +1.. +3] class_of_usage         = 3 bit
                [  +4.. +5] address_type           = 2 bit
                              if address_type ∈ {0,1}:
                [  +6.. +29] gssi                  = 24 bit
                              if address_type == 1:
                [ +30.. +53] address_extension     = 24 bit
                              if address_type == 2:
                [  +6.. +29] vgssi                 = 24 bit
[ ..]        weitere optional fields (mm_attached_to_lai, dm_ms_address, ...)
[131]      trailing m-bit (Body-Ende) = 1 bit (= 0)
```

(Bit-Positionen sind Annäherungen weil Optional-Fields variable Längen haben — die p_*-Bits zwingen einen variable Encoder. Für M3 muss der RTL-Parser Bit für Bit traversieren, nicht mit festem Offset rechnen.)

## Hex-Slices Gold-Ref vs MTP3550 (Bit-Forensik 2026-04-26)

**Gold-Ref (externe BS, externe MS Vendor unbekannt, ITSI=0x282FF4, GSSI=0x2F4D61):**

```
UL#0 hex: 01 41 7F A7 01 12 66 34 20 C1 22 60   t=00:12:01.36, frag=1, mm-body Fragment 1
UL#1 hex: D4 1C 3C 02 40 50 [2F 4D 61] 20 00 00  t=00:12:01.41, MAC-U-BLCK Fortsetzung
                            ^^^^^^^^^^
                            GSSI = 0x2F4D61 (= 3100001) bei Byte-Offset 6..8 in UL#1
```

**MTP3550 (eigene WAV `baseband_428153562Hz_19-16-24_25-04-2026.wav`):**

```
UL#0 (5x repeat) hex: 01 41 7C 8F 01 12 66 34 20 C1 22 60   ssi=0x282F91, frag=1
UL#6 hex (Fortsetzung): D4 1C 3C 02 40 50 [00 00 01] 20 00 00
                                          ^^^^^^^^^^
                                          GSSI = 0x000001 (MTP3550 default "no group")
```

**Layout-Identität:** beide MS folgen exakt dem gleichen Encoder-Pfad. Nur die GSSI-Werte sind MS-Default-spezifisch.

## Reassembly-FSM (für Phase F-Implementation)

```
S_RA_WAIT_FRAG1 → MAC-ACCESS frag=1 empfangen → puffer ul0_bits[48..91], lat_ssi, lat_t0_start
                  → S_WAIT_FRAG2

S_WAIT_FRAG2    → MAC-U-BLCK von gleicher SSI → puffer ul1_bits[4..91],
                  reassemble 132-bit body, MM-Body-Parser starten
                  → S_PARSE_MM_BODY

T0-Timeout (ETSI ≈ 2 Frames = 113 ms): wenn UL#1 nicht innerhalb T0 nach UL#0 ankommt
                  → drop, zurück zu S_RA_WAIT_FRAG1, Counter inkrementieren
```

## Why
M2 hat ohne Reassembly funktioniert weil MTP3550 die BS-diktierte GSSI per GILA-im-Accept
akzeptiert (auch wenn die MS eigenes GSSI=1 wollte). Sobald wir aber:
- Multi-Group-MS (MS schickt mehrere GSSIs)
- Operator-Mode "respect MS wishes" (BS akzeptiert MS-Wunsch wenn EntityTable-permit OK)
- echte CMCE-Group-Calls (BS muss wissen welche GSSI das MS empfängt um U-SETUP zu pushen)

unterstützen wollen, ist Reassembly Pflicht.

## How to apply
- Vor Phase G (Group-Call) Phase F (Reassembly + IE-Parser) bauen.
- Bit-genaue TBs gegen die obigen Hex-Slices.
- `tb_ul_demand_reassembly` mit beiden bursts als Stimulus.
- `decode_ul.py` als Bit-Diagnose-Helper auch erweitern.
