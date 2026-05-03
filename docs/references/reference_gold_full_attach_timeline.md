---
name: Gold-Cell vollständige Attach- + DETACH- + Heartbeat-Timeline (UL+DL frame-genau)
description: Frame-genaue Korrelation von Gold-DL und Gold-UL über die 110-s-Capture (`GOLD_DL_…GRUPPENRUF.wav` + `GOLD_UL_…GRUPPENRUF.wav`, MCC=262 MNC=1010 CC=1, MS=0x282FF4=2633716). Pre-Attach-Sequenz, ITSI-Attach 3-Frag-Pattern, U-ITSI-DETACH-ACK Pärchen, Group-Attach, AACH-Transitions, Phase-H.7 D-NWRK-BCAST-Cadence. Empirische Time-Achse-Offset UL_t = DL_t + 1.611s (mtime-Werte unzuverlässig).
type: reference
originSessionId: b85ff25e-2a77-47a3-a8ad-a1f569ece2e0
---
## Konstanten

- **Gold-Cell:** MCC=262 MNC=1010 CC=1, scrambCode=0x4183F207, DL=392.9875 MHz
- **MS:** ISSI=0x282FF4 (2633716) — Motorola MTP3550 Gold-Ref Hardware
- **Capture-Files:** `wavs/gold_standard_380-393mhz/GOLD_DL_ANMELDUNG_GRUPPENWECHSEL_GRUPPENRUF.wav` (DL, 103.979s) + `GOLD_UL_…wav` (UL, 109.355s)
- **MCCH liegt auf air-TN=0** (decoder-TN=1 wegen +1-Slot-Shift); **Traffic-Slots TN=1..3** (decoder-TN=2..4)
- **Time-Achse:** UL_t = DL_t + 1.611s (UL-Capture started 1.611s **vor** DL-Capture, empirisch über 6 Attach-Cluster verifiziert; mtime-Differenz von 3.748s war wegen externer mtime-Modifikation falsch)

## Pre-Attach-Sequenz (vor dem ersten MS-Frag-1)

Vom Capture-Start (DL_t=0) bis zum **ersten MS-Frag-1** (DL_t=10.99 = UL_t=12.55):

```
0.000s …  6.005s   Pure Cell-Pilot, ZERO addressed Signaling:
                   • SB1 SYNC + SYSINFO jeden TDMA-Frame (decoder-TN=2/3/4 = air-TN=1/2/3)
                   • NDB2 NULL-PDU LI=2 addr=NULL auf decoder-TN=1 (= air-TN=0 = MCCH)
                   • AACH-Werte: traffic-slots 0x3000 CapAlloc f1=0 f2=0,
                                 MCCH idle 0x0249 [DL/UL-Assign DL=Common UL=Random f1=9 f2=9]

6.005s   ↓BS→MS  #423  decoder-TN=1 FN=04 MN=44  AACH 0x0249 idle bleibt
                   NDB1 SCH/F MAC-RESOURCE addr=SSI=0xFFFFFF (broadcast) LI=16
                   LLC=BL-UDATA  MLE=MLE/D-NWRK-BROADCAST     ← Cell-Heartbeat

6.005s … 10.99s   ≈5s Settle/Verarbeitung (nur SB1 + NULL-PDU)
                   In dieser Zeit liest die MS SYSINFO+CC, validiert die Cell.

10.99s   ↑MS→BS  UL#0 ITSI-Attach Frag-1 — Attach-1 startet
```

**Befund:** Erster nicht-trivialer DL-Event ist **D-NWRK-BROADCAST**. Vorher gibt es _keine_ adressierte Signaling. Die MS wartet **mindestens 1×** D-NWRK-BCAST ab, bevor sie attached.

## D-NWRK-BROADCAST-Cadence in Gold (alle 7 Bursts)

| Burst | DL_t   | MN  | Δ zum nächsten |
|-------|--------|-----|----------------|
| #423  | 6.005  | 44  | +9.97s |
| #1127 | 15.978 | 53  | +10.03s |
| #1835 | 26.008 | 03  | +9.97s |
| #2539 | 35.981 | 13  | +10.03s |
| #3247 | 46.011 | 23  | +9.97s |
| #3951 | 55.984 | 33  | +10.03s |
| #4659 | 66.014 | 43  | +10.03s |
| #5363 | 75.988 | 52  | +9.97s |
| #6071 | 85.998 | 02  | +10.0s  |
| #6775 | 95.998 | 12  | _(letzter im Capture)_ |

**Gold-Cadence = exakt 10.0s ± 30ms** = 10 Multiframes, immer auf decoder-TN=1 NDB1 SCH/F mit AACH `0x0249` (idle).

**Phase-H.7 in unserer Cell** (siehe `project_h7_d_nwrk_broadcast.md`): Daemon-Tick = 7s, also ~30% schneller als Gold. Bit-Inhalt 0/432 diffs gegen Gold-Burst #423 (verifiziert).

## ITSI-Attach 3-Frag-Pattern (kanonisch)

**6 ITSI-Attach-Versuche** im Capture (Δ alle ~12-25s, nicht regelmäßig):

| # | UL_t | DL_t | MN  | Pattern |
|---|------|------|-----|---------|
| 1 | 12.55 | 10.99 | 49 | komplett 3-Frag |
| 2 | 37.20 | 35.64 | 13 | komplett 3-Frag |
| 3 | 59.76 | 58.20 | 35 | komplett 3-Frag |
| 5 | 76.81 | 75.25 | 52 | doppelt (#5311+#5335) |
| 6 | 81.57 | 80.01 | 56 | komplett |

Bit-genaue Sequenz pro Attach-Versuch (von Attach-1, repräsentativ — ALLE sind byte-identisch außer NS-Bit):

```
t₀+0ms      ↑MS→BS  Frag-1   115sym SCH/HU
                    hex: 01 41 7F A7 01 12 66 34 20 C1 22 60
                    MAC-ACCESS pdu=0 fill=0 enc=0 SSI=0x282FF4 frag=1 res_req=0
                    LLC: BL-DATA(NS=0)
                    MLE: MM/U-LOC-UPD-DEMAND  type=ITSI-Attach
                         class_of_ms=0x1A0F60 ESM=1 (more optionals follow in Frag-2)

t₀+57ms     ↑MS→BS  Frag-2   115sym SCH/HU                          [Δ = 1 Frame]
                    hex: D4 1C 3C 02 40 50 2F 4D 63 20 00 00
                    MAC-FRAG/END pdu=1 fill=1 mac_top_nibble=3 (Continuation)
                    Inhalt: GroupIdLocDemand-IE GSSI=0x002F4D63 (44+88-bit MM-Body)

t₀+57ms     ↓BS→MS  Pre-Reply  decoder-TN=1 NDB2 BKN1 SCH/HD       [Δ = 1F = gleicher TDMA-Frame wie Frag-2]
                    AACH: DL=Unalloc UL=Unalloc CC=9 f1=0 f2=9   (raw 0x0009)
                    bits: 0010001 000111001 001010000010111111110100 010 0000000 010000000000000001 0000 1000 ...
                    MAC-RESOURCE pdu=00 fill=1 pog=0 enc=00 ra=1 LI=7
                                 addr=SSI=0x282FF4 flags=000 (kein slot_grant)
                    LLC: AL-SETUP (type=8) — 7-octet wrapper, kein MM-Body inline

t₀+114ms    ↑MS→BS  Frag-3   115sym SCH/HU                         [Δ = 2F nach Frag-1]
                    hex: 41 41 7F A4 63 C0/40 41 41 7F A4 08 00
                    MAC-RESOURCE addr=SSI=0x282FF4 LI=6
                    LLC: BL-ACK(NR=1 oder 0, alterniert je Versuch)
                    MLE: SNDCP / U-MM-STATUS=24 oder =8

t₀+114ms    ↓BS→MS  Final Accept  decoder-TN=1 NDB1 SCH/F          [Δ = 2F nach Pre-Reply]
                    AACH: 0x0009 (= same as pre-reply)
                    bits: 0010000 010101001 001010000010111111110100 010 0000000 0000000 1010 1011 0001 0000...
                    MAC-RESOURCE LI=21 fill=1 SSI=0x282FF4 flags=010 (slot_grant=1!)
                    LLC: BL-ADATA(NR=0 NS=1 oder NS=0)
                    MLE: MM/D-LOC-UPD-ACCEPT
                         LocUpdAccept: ITSI-Attach ESI=0x0000 T3id=5 T3len=58
```

### Sequenznummern-Tabelle (NR/NS-Alternation pro Attach-Versuch)

| Attach | DL Pre-Reply (LI=7) | UL BL-ACK NR | DL ACCEPT (LI=21) NR | DL ACCEPT NS |
|--------|---------------------|--------------|----------------------|--------------|
| 1 (#775/#783) | identisch | NR=1 | NR=0 | NS=1 |
| 2 (#2515/#2523) | identisch | NR=0 | NR=0 | NS=0 |
| 3 (#4107/#4115) | identisch | NR=1 | NR=0 | NS=1 |

→ **NR und NS alternieren konsistent zwischen Versuchen**, BS pflegt LLC-Sequenznummern pro MS-SSI über Time hinweg.

## U-ITSI-DETACH-ACK Pärchen (LI=6 — vorher fälschlich als „Heartbeat" identifiziert)

Tritt auf, **nachdem** die MS ein U-ITSI-DETACH gesendet hat (kein neuer Attach).

```
T₀+0ms     ↑MS→BS   single 114-sym SCH/HU
                    hex: 41 41 7F A4 71 91 40 01 41 7F A4 00
                    MAC-RESOURCE LI=7 SSI=0x282FF4
                    LLC: BL-DATA(NS=1)
                    MLE: MM/U-ITSI-DETACH

T₀+57ms    ↓BS→MS   decoder-TN=1 NDB2 BKN1 SCH/HD                   [Δ=1F]
                    AACH: DL=Common UL=Random f1=9 f2=9 (raw 0x0249)  ← idle bleibt!
                    LI=6 LLC=AL-SETUP (type=8) addr=SSI=0x282FF4

T₀+114ms   ↓BS→MS   decoder-TN=1 NDB2 BKN1 SCH/HD                   [Δ=2F = 1F nach AL-SETUP]
                    AACH: 0x0249 idle bleibt!
                    LI=6 LLC=BL-ACK(NR=1)
```

3 DETACH-Versuche im Capture beobachtet:
- T=20.82 → Pärchen #1359 + #1363
- T=44.63 → Pärchen #3039 + #3043
- T=84.86 → Pärchen #5879 + #5883

**Schlüsseldifferenz zur Attach-Reply:** AACH **bleibt** auf idle `0x0249` während DETACH-ACK. Beim Attach-Reply wechselt AACH auf signalling-active `0x0009`.

## Group-Attach (4. Attach-Versuch in Capture)

```
69.62  ↑MS→BS  Frag-1  hex 01 41 7F A7 01 97 ...     (byte 5 = 0x97 statt 0x12!)
              MAC-ACCESS frag=1 BL-DATA(NS=0) MLE=MM/U-ATTACH-DETACH-GRP-ID-DEMAND
69.67  ↑MS→BS  Frag-2  hex 8D 59 30 5E 9A C6 ...     (Continuation, andere Bytes als ITSI-Attach)
69.67  ↓BS→MS  #4803   AACH=0x0009  LI=7 AL-SETUP (gleicher Pattern wie ITSI-Attach Pre-Reply)
69.78  ↑MS→BS  Frag-3  hex 41 41 7F A4 63 40 ...     BL-ACK NR=0
69.78  ↓BS→MS  #4811   AACH=0x0009  LI=16 (NICHT 21!)
              SCH/F MAC-RESOURCE addr=SSI=0x282FF4 LI=16
              LLC=BL-ADATA(NR=1 NS=0)
              MLE=MM/D-ATTACH-DETACH-GRP-ID-ACK
```

**Unterschied zu ITSI-Attach:**
- LI=21 → **LI=16** im Final-Reply
- D-LOC-UPD-ACCEPT → **D-ATTACH-DETACH-GRP-ID-ACK**
- NR/NS sind anders (NR=1 NS=0 statt NR=0 NS=1)

## AACH-Modes (kompletter Repertoire-Befund Gold)

| AACH raw | Decoder-Label | Wann verwendet |
|----------|---------------|----------------|
| `0x3000` | CapAlloc f1=0 f2=0 | Default Traffic-Slots TN!=0 (decoder-TN=2..4) |
| `0x0249` | DL/UL-Assign DL=Common UL=Random f1=9 f2=9 | MCCH (decoder-TN=1) **idle** + DETACH-ACK + D-NWRK-BCAST |
| `0x0009` | DL/UL-Assign DL=Unalloc UL=Unalloc f1=0 f2=9 | MCCH **signalling-active** (Attach Pre-Reply + Final Accept + Group-Attach Reply) |
| `0x22C9` | Reserved f1=11 f2=9 | TN=2 NDB2 SCH/HD während D-OTAR-Session (Group-Call-Phase, nach t=86s) |
| `0x2049` | Reserved f1=1 f2=9 | TN=2 NDB2 SCH/HD im D-OTAR-Pattern (vereinzelt) |
| `0x304B` | CapAlloc f1=1 f2=11 | TN=2 NDB1 SCH/F D-OTAR mit BL-ACK + slot_grant |
| `0x2249` | Reserved f1=9 f2=9 | TN=2 NDB1 SCH/F (D-OTAR-Variante) |

**MCCH-Transition Gold (decoder-TN=1):** idle `0x0249` ↔ signalling-active `0x0009` ↔ idle `0x0249`. Wechsel passiert pro Frame, gehalten über die **gesamte Dauer der Attach-Reply** (3 Frames für 3-Frag-Pattern).

## Was unsere Cell zwingend pro Frame haben muss (Gold-bit-genaues Spec)

1. **Periodische D-NWRK-BROADCAST alle ~10s** auf decoder-TN=1 NDB1 SCH/F, AACH `0x0249`, LI=16, addr=BCAST=0xFFFFFF, MLE/D-NWRK-BROADCAST  
   → Phase-H.7 implementiert, Tick auf 7s — **anpassen auf 10s**
2. **AACH-Wechsel auf `0x0009`** für die Frames, in denen unser MLE-FSM eine Pre-Reply oder ein ACCEPT in die DL-Signal-Queue popft  
   → Bereits implementiert für decoder-TN=1 (signalling_active=1)
3. **AL-SETUP LI=7 SCH/HD pre-reply** mit MAC-Header `pdu=00 fill=1 pog=0 enc=00 ra=1 LI=7 addr=SSI flags=000` (NO slot_grant)  
   → MLE-FSM-Builder muss exakt diesen MAC-Header emittieren
4. **D-LOC-UPD-ACCEPT LI=21 BL-ADATA SCH/F** mit MAC-Header `flags=010` (slot_grant=1!) + LLC NR alterniert + MLE LocUpdAccept ESI=0x0000 T3id=5 T3len=58  
   → Bit-genau gegen `reference_gold_attach_bitexact.md` halten
5. **Δ = 2 Frames** zwischen LI=7 und LI=21 auf decoder-TN=1  
   → ⚠️ `tetra_mle_registration_fsm.v:1257` wartet derzeit nur 3 slot_pulses (~0.75 Frames) — Code-Kommentar sagt "two frames later" aber Wert passt nicht. **Überprüfen + ggf. auf gap_slot_count=8 erhöhen** für saubere 2-Frame-Lücke
6. **AACH bleibt auf `0x0249` (idle) bei DETACH-ACK + D-NWRK-BCAST** — kein Wechsel zu `0x0009`  
   → Aktuelle AACH-Encoder-Logik berücksichtigt das nicht: signalling_active=1 wechselt aktuell für **alle** Pop-Events. Müsste pro Pop-Event-Typ unterschieden werden (Attach-Reply → 0x0009, DETACH-ACK + BCAST → 0x0249).

## Korrekturen zu existierenden Memories

- **`reference_gold_attach_bitexact.md`** sagt "1 frame Δ" zwischen AL-SETUP und ACCEPT — falsch. Korrekt: **2 frames Δ** (verifiziert über 3 saubere Pairs #775/#783, #2515/#2523, #4107/#4115).
- **`project_h7_d_nwrk_broadcast.md`** sagt Daemon-Tick=7s — Gold ist 10s. Soll auf 10s angepasst werden für Bit/Cadence-genaue Replikation.
- **`project_gold_vs_ours_dl_struct_findings.md`** ist mit dieser Time-Korrelation jetzt vollständig: alle 6 LI=6-Bursts auf decoder-TN=1 sind **DETACH-ACKs**, nicht „Heartbeats".

## Time-Achse — wie zukünftig korrelieren

UL+DL-Captures müssen **frame-genau zusammengelegt** werden. Workflow:
1. Decode DL mit `--dump-burst -2` für sample-positions-only (`/tmp/gold_dl_pos.log`).
2. Decode UL mit `--max-bursts N --dump-bits` (lightweight burst-only via `/tmp/ul_bursts_only.py` falls schnell nötig).
3. Empirischen OFFSET aus Cluster-Match bestimmen (wie hier 1.611s, mtime ist NICHT zuverlässig wenn UL-Datei mtime „glatt" ist wie `*.000000000`).
4. UL-Achse als Master, DL-Events um +OFFSET shiften.
5. Per Attach-Cluster Δ verifizieren: BS-LI=7-Reply muss exakt 1F nach MS-Frag-2 stehen, BS-LI=21-Reply 2F nach LI=7.
