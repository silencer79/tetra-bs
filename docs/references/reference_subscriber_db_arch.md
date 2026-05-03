---
name: Subscriber-DB / AST / Profile Architektur (Phase 6, post-M2)
description: Datenmodell + BRAM-Layouts + AXI-Regs + Operator-Schnittstelle für M2-Hardening und M3-Vorbereitung. Verbindlich.
type: reference
originSessionId: b85ff25e-2a77-47a3-a8ad-a1f569ece2e0
---
**Volle Spec:** `docs/ARCHITECTURE.md §9`. Hier nur die Schlüssel-Parameter
für schnellen Lookup.

## Drei Tabellen (Total 2 BRAM18k)

| Tabelle | Größe | Wer schreibt |
|---------|-------|--------------|
| Entity Table | 256 × 64 bit (1 BRAM18k) | ARM via AXI 0x180..0x18C indirect |
| Profile Table | 6 × 32 bit (LUT-RAM) | ARM via AXI |
| AST + Group-Cache | 64 × 256 bit (1 BRAM18k) | MLE-FSM volatile |

## Record-Layouts

### Entity Table (64 bit)
```
[63:40]  entity_id (ISSI oder GSSI)  24
[39]     entity_type (0=ISSI,1=GSSI)  1
[38:35]  profile_id                   4
[34: 1]  reserved                    34
[ 0]     valid                        1
```

### Profile Table (32 bit, 6 Slots)
```
[31:24]  max_call_duration  8 (sec, 0=unlimited)
[23:16]  hangtime           8 (×100ms, max 25.5s)
[15:12]  priority           4
[11: 4]  reserved           8
[ 3]     permit_voice       1
[ 2]     permit_data        1
[ 1]     permit_reg         1
[ 0]     valid              1
```

Profile 0 = bit-exakt **`0x0000_088F`** (Default für Auto-Enroll, read-only enforced):
- `max_call_duration` = `0x00` (unlimited)
- `hangtime` = `0x00`
- `priority` = `0x0`
- `reserved` = `0x88` (carry-over invariant; do not zero)
- `permit_voice` = 1, `permit_data` = 1, `permit_reg` = 1, `valid` = 1

Frühere Doku-Glosse "permit_reg=1, alles andere 0" war nicht bit-exakt — die
in production verwendete Profile-0-Konstante ist `0x0000_088F` und schaltet
Voice + Data + Reg frei; der `reserved`-Bytewert `0x88` muss bit-exakt bleiben
(durchgängig in Carry-Over-Memos so geführt).

### AST (256 bit, 64 Slots)
```
[255:232]  ISSI                  24
[231:208]  last_seen_multiframe  24 (rollover ~197 Tage)
[207:200]  shadow_idx             8 (Backref Entity)
[199:196]  state                  4
[195:192]  group_count            4 (0..8)
[191:  0]  group_list[8]        192 (8 × 24 bit GSSI)
```

## AXI-Regs (neu für Phase 6)

| Reg | Felder | Default |
|-----|--------|---------|
| `0x1A4` | `[15:0]` mle_detach_cnt | 0 |
| `0x1A8` | `[31:0]` ast_ttl_multiframes | 84706 (≈24h, 1.02s/MF) |
| `0x1AC` | `[0]` accept_unknown<br>`[1]` auto_enroll_default_profile | 1, 0 |

TTL: 24h Default per Beobachtung 2026-04-25 (MTP3550 sendet keine
periodischen Updates → 24h ist konservativ + ETSI-T354-konform).

## Datenfluss

**Attach:** Entity.query(ISSI) → Profile.lookup → permit-check → AST.alloc/reuse
→ optional GSSI-Liste vom MS, jede einzeln Permit-checken → AST.write mit
group_list → D-LOC-UPDATE-ACCEPT mit GILA aus AST (nicht hardcoded).

**Detach:** AST.query(ISSI) → if hit → AST.write(valid=0) + counter.
Entity Table NICHT angefasst.

**TTL-Sweep:** `tetra_ast_ttl_sweeper.v`, jede Multiframe scan.
`(now - last_seen) > REG_AST_TTL_MULTIFRAMES` → invalidate.
last_seen wird bei JEDER UL-Aktivität dieser ISSI aktualisiert.

## Operator-Schnittstelle

**`tetra_web` (mongoose HTTP-Daemon auf ARM):**
- `GET/POST/DELETE /api/entities[/{ID}]`
- `GET/POST /api/profiles[/{ID}]`
- `GET /api/sessions` (live-AST-dump via AXI)
- `/` statisches index.html + JS

**Persistenz:** `/var/lib/tetra/entities.tsv` + `profiles.tsv`.
`inotify`-Watcher → bei Dateiänderung sofortiger BRAM-Sync (auch SSH-Edit).

## Roadmap

| Phase | Inhalt | Aufwand |
|-------|--------|---------|
| A | Shadow-Lookup in MLE-FSM, Permit-Check, Auto-Enroll | 1 Tag |
| B | Detach-Pfad + AST `last_seen` 8→24 bit | ½ Tag |
| C | TTL-Sweep FSM | ½ Tag |
| D | GILA-Encoder aus Lookup statt hardcoded | 1 Tag |
| E | WebUI + inotify | 1 Tag SW |

## Why
M2 hat Hardcoded-GILA und keine permit-Prüfung → reicht für 1 MTP3550, nicht
für Multi-MS oder Operator-getriebene Anlage. Subscriber-Shadow-BRAM und
indirect-window existieren bereits seit Commit `b22ffd8`, müssen nur in den
Attach-Pfad verdrahtet werden.

## How to apply
Bei jeder Phase A-E: Diese Spec ist verbindlich für Layout/Sizes/AXI-Regs.
Änderungen vorher mit Kevin abstimmen — er hat alle Open Decisions
bewusst entschieden (Profile-Felder, 24h TTL, Multi-GSSI, WebUI etc.).
