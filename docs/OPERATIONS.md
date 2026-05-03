# Operations

## Build

(TBD after Phase 2 implementation. Carry-over from `tetra-zynq-phy`
`scripts/deploy.sh` is starting point.)

Expected commands:

```bash
make rtl       # Vivado batch synth + place + route → build/system.bit
make sw        # Cross-compile SW daemon + WebUI CGI → build/sw/
make tb        # Run all RTL test benches
make sw-test   # Run SW unit tests on host
make cosim     # Verilator co-sim FPGA + SW
make deploy    # Push bitstream + SW to Board #1
```

### Build/CI

Top-level Makefile (T0 build-skeleton, 2026-05-03) is the green-CI entry
point — every Phase-2 agent's PR lands through it. `make help` lists every
target with one-line descriptions. Default green-CI quartet (also run by
`.github/workflows/ci.yml` on `ubuntu-24.04`):

- `make tb` — iverilog 12.0 over every per-block dir under `tb/rtl/<block>/`.
- `make sw-test` — `/usr/bin/gcc` 13.3 + vendored Unity (`sw/external/unity/`,
  pinned `v2.6.0`) over every `tb/sw/<block>/`.
- `make sw-build` — `/usr/bin/arm-linux-gnueabihf-gcc` 13.3 cross-compile;
  full path is mandatory because Vitis 11.2 wraps the same name (HARDWARE.md
  §2 PATH-precedence trap). Output: `build/arm/`.
- `make cosim` / `make synth` — stubs, filled by T2 / A5; not in CI.

Bootstrap a clean host with `scripts/build/setup-host.sh` (idempotent;
installs `verilator`, `libjansson-dev`, `gcc-arm-linux-gnueabihf`, etc.,
per HARDWARE.md §10 follow-ups, then prints a probed-vs-expected table).
Per-block TB Makefiles only declare `SRCS` and include the shared
`tb/{rtl,sw}/Makefile.inc`.

## Deploy

(TBD. See old project `scripts/deploy.sh` for pattern.)

## WebUI

The WebUI is the single operator entry point on Board #1. It replaces the
bluestation `net_brew`/`net_telemetry`/`net_control` triplet (decision #14) and
the legacy `tetra-zynq-phy` CGI corpus wholesale.

**Runtime contract** (from HARDWARE.md §8 and decision #9):

- HTTP server: `busybox httpd` 1.30.1, document root `/www/`, CGI dir
  `/www/cgi-bin/`, no auth today.
- Each CGI binary is a thin socket client: parse query / body, build a JSON
  envelope, `connect(/run/tetra_d.sock)`, write request, read response, emit
  `Content-Type: application/json` and the response body. **No business logic
  in CGIs.**
- Daemon `tetra_d` is authoritative for state, DB writes, FPGA AXI access, and
  long-running jobs.
- BusyBox httpd default exit timeout ≈ 3 s. CGIs MUST NOT block. Long-running
  ops dispatch async via the job queue (see "Capture trigger / bitstream switch").
- Socket: `/run/tetra_d.sock`, `SOCK_STREAM` UNIX, mode `0660`, owned
  `root:tetra` (group used to allow CGI uid in if BusyBox is dropped from root,
  not used today).

**Naming convention.** Each CGI is named `<noun>.cgi` and dispatches one or
more `op`s on the daemon. Several CGIs share the same daemon op family (e.g.
`profiles.cgi` covers `profile.list`, `profile.get`, `profile.put`,
`profile.delete`). Daemon op names are dotted `<entity>.<verb>` and form the
stable wire-API independent of the URL layout.

**Heading map.** §1 Live Status, §2 Subscriber DB, §3 Debug, §4 Configuration,
§5 Tools, §6 JSON envelope, §7 Auth model, §8 Decoder upload, §9 Async jobs.

---

### 1. Live Status

Read-only. Drives the dashboard view that auto-refreshes (suggested period
1 s for counters, 250 ms for AACH timeline if explicitly opened).

**Views**

| View | Powered by endpoints |
|---|---|
| Dashboard summary | `status.cgi?scope=summary`, `status.cgi?scope=layers` |
| Layer states drilldown | `status.cgi?scope=layers`, `status.cgi?scope=msgbus_depth` |
| AST live | `sessions.cgi?op=list` (read-only mirror of in-memory AST) |
| Active calls / groups | `status.cgi?scope=calls`, `status.cgi?scope=groups` |
| RX/TX stats | `status.cgi?scope=phy_stats`, `status.cgi?scope=dma_stats` |
| TX queue depth (per priority) | `status.cgi?scope=txq` |
| AACH last-N slots | `debug.cgi?op=aach_recent&n=N` (also Live, but data plane same as Debug §3) |
| FPGA register dump | `status.cgi?scope=fpga_regs` |

**Endpoint table**

| Method | Path | Request (query / JSON) | Response data | Daemon op | Errors | Side-effect | Touches |
|---|---|---|---|---|---|---|---|
| GET | `/cgi-bin/status.cgi?scope=summary` | — | `{cell_synced, mcc, mnc, cc, la, rx_freq_hz, tx_freq_hz, uptime_s, fpga_bitstream_id, sw_version}` | `status.summary` | — | read | global cfg snapshot |
| GET | `/cgi-bin/status.cgi?scope=layers` | — | `{phy:{...}, lmac:{...}, umac:{...}, llc:{...}, mle:{...}, mm:{...}, cmce:{...}, sndcp:{...}}` each with `state, last_event_ts, error_count` | `status.layers` | — | read | per-layer FSM state |
| GET | `/cgi-bin/status.cgi?scope=phy_stats` | — | `{burst_detect_rate, sync_lock, crc_pass, crc_fail, viterbi_be, agc_gain_db, rssi_dbm}` | `status.phy_stats` | `EFPGA` if AXI read fails | read | PHY status regs <!-- TODO: confirm reg name --> |
| GET | `/cgi-bin/status.cgi?scope=dma_stats` | — | `{ch1_rx_frames, ch1_rx_bytes, ch2_tx_frames, ch2_tx_bytes, ch3_rx_frames, ch4_tx_frames, ch1_overruns, ch2_underruns, ...}` | `status.dma_stats` | — | read | DMA descriptor counters |
| GET | `/cgi-bin/status.cgi?scope=txq` | — | `{immediate:{depth, peak}, normal:{...}, low:{...}}` | `status.txq` | — | read | message-bus queues |
| GET | `/cgi-bin/status.cgi?scope=msgbus_depth` | — | `{queue_depth_per_priority, dispatch_rate_hz, drop_count}` | `status.msgbus` | — | read | message-bus internal |
| GET | `/cgi-bin/status.cgi?scope=calls` | — | `[{call_id, ssi_caller, ssi_called, gssi, state, slot, started_ts}]` | `status.calls` | — | read | CMCE active call table |
| GET | `/cgi-bin/status.cgi?scope=groups` | — | `[{gssi, member_count, members:[issi,...]}]` | `status.groups` | — | read | MLE multi-lookup view |
| GET | `/cgi-bin/status.cgi?scope=fpga_regs` | — | `{name: {addr, value_hex}, ...}` for the named-register set ARCHITECTURE.md exposes | `status.fpga_regs` | `EFPGA` | read | full AXI register window |
| GET | `/cgi-bin/status.cgi?scope=aach_current` | — | `{slot, frame, multiframe, aach_word_hex, dl, ul, f1, f2}` | `status.aach_current` | — | read | UMAC scheduler last-emit |

---

### 2. Subscriber DB

Layouts: `docs/references/reference_subscriber_db_arch.md`. Profile fields:
`max_call_duration`, `hangtime`, `priority`, `permit_voice`, `permit_data`,
`permit_reg`, plus implicit `valid` and `id` (slot index 0..5).

Profile 0 is the read-only invariant `0x0000_088F` (architecture §"Subscriber-DB").
Daemon refuses Profile 0 mutation regardless of WebUI guard.

**Views**

| View | Powered by endpoints |
|---|---|
| Profile list | `profiles.cgi?op=list` |
| Profile editor | `profiles.cgi?op=get&id=N`, `profiles.cgi` POST `op=put`, POST `op=delete` |
| Entity list (paged, 256 entries) | `entities.cgi?op=list&offset=X&limit=Y` |
| Entity editor | `entities.cgi?op=get&id=N`, POST `op=put`, POST `op=delete` |
| Session live AST | `sessions.cgi?op=list` |
| Session detail | `sessions.cgi?op=get&issi=0x...` |
| Bulk import | `db.cgi` POST `op=import` (multipart upload of `db.json`) |
| Bulk export | `db.cgi?op=export` (returns full `db.json`) |
| DB policy | `policy.cgi?op=get`, POST `op=put` |

**Endpoint table — Profiles**

| Method | Path | Request | Response data | Daemon op | Errors | Side-effect | Touches |
|---|---|---|---|---|---|---|---|
| GET | `/cgi-bin/profiles.cgi?op=list` | — | `[{id, max_call_duration, hangtime, priority, permit_voice, permit_data, permit_reg, valid, locked}]` (`locked=true` for id=0) | `profile.list` | — | read | DB profiles[] |
| GET | `/cgi-bin/profiles.cgi?op=get&id=N` | — | `{id, ...same fields}` | `profile.get` | `ENOENT` | read | DB profiles[N] |
| POST | `/cgi-bin/profiles.cgi` | `{op:"put", id:N, max_call_duration, hangtime_100ms, priority, permit_voice, permit_data, permit_reg}` | `{id:N, packed_word_hex}` | `profile.put` | `EINVAL` (range), `EROFS` (id=0), `ENOSPC` | write DB + fsync | DB profiles[N], JSON file |
| POST | `/cgi-bin/profiles.cgi` | `{op:"delete", id:N}` | `{id:N, valid:false}` | `profile.delete` | `EROFS` (id=0), `EBUSY` (referenced by entity) | write DB | DB profiles[N] |
| POST | `/cgi-bin/profiles.cgi` | `{op:"reset_factory"}` | `{count:6}` | `profile.reset` | — | full overwrite | DB profiles[] |

**Endpoint table — Entities**

| Method | Path | Request | Response data | Daemon op | Errors | Side-effect | Touches |
|---|---|---|---|---|---|---|---|
| GET | `/cgi-bin/entities.cgi?op=list&offset=O&limit=L` | — | `{total, items:[{slot, entity_id, entity_type, profile_id, valid}]}` | `entity.list` | `ERANGE` | read | DB entities[] |
| GET | `/cgi-bin/entities.cgi?op=get&slot=N` | — | `{slot, entity_id, entity_type, profile_id, valid}` | `entity.get` | `ENOENT` | read | DB entities[N] |
| GET | `/cgi-bin/entities.cgi?op=lookup&id=0xNNNNNN&type=ISSI\|GSSI` | — | `{slot, ...}` or `{found:false}` | `entity.lookup` | — | read | DB entities[] |
| POST | `/cgi-bin/entities.cgi` | `{op:"put", slot:N\|null, entity_id, entity_type, profile_id}` | `{slot, packed_word_hex}` (slot=null → daemon picks free slot) | `entity.put` | `EINVAL`, `ENOSPC` (table full), `EEXIST` (id+type duplicate), `EREF` (profile_id invalid) | write DB | DB entities[N] |
| POST | `/cgi-bin/entities.cgi` | `{op:"delete", slot:N}` | `{slot:N, valid:false}` | `entity.delete` | `ENOENT` | write DB; also flushes any AST entry pointing at this slot | DB entities[N], AST cross-ref |
| POST | `/cgi-bin/entities.cgi` | `{op:"clear_all"}` | `{count}` | `entity.clear_all` | — | wipe | DB entities[] |

**Endpoint table — Sessions (AST, read-only)**

AST is in-memory only; persisted only on clean SIGTERM (decision #10). All
endpoints here are read-only mirrors.

| Method | Path | Request | Response data | Daemon op | Errors | Side-effect | Touches |
|---|---|---|---|---|---|---|---|
| GET | `/cgi-bin/sessions.cgi?op=list` | — | `[{slot, issi, last_seen_multiframe, shadow_idx, state, group_count, group_list:[gssi,...]}]` (only slots with `valid=1`) | `session.list` | — | read | AST in-memory |
| GET | `/cgi-bin/sessions.cgi?op=get&issi=0xNNNNNN` | — | `{slot, ...}` or `{found:false}` | `session.get` | — | read | AST in-memory |
| GET | `/cgi-bin/sessions.cgi?op=stats` | — | `{capacity:64, used, free, ttl_sweeps, last_sweep_ts}` | `session.stats` | — | read | AST stats |

**Endpoint table — Bulk DB**

| Method | Path | Request | Response data | Daemon op | Errors | Side-effect | Touches |
|---|---|---|---|---|---|---|---|
| GET | `/cgi-bin/db.cgi?op=export` | — | full `db.json` body, `Content-Type: application/json` | `db.export` | — | read | DB file |
| POST | `/cgi-bin/db.cgi` | multipart `op=import`, file=db.json, optional `merge:bool` | `{accepted:bool, profiles_loaded, entities_loaded}` | `db.import` | `EINVAL` (schema), `ENOSPC` | atomic-rename write | full DB |
| POST | `/cgi-bin/db.cgi` | `{op:"reset_factory"}` | `{ok:true}` | `db.reset` | — | wipe to defaults (Profile 0 = `0x0000_088F`, all entities cleared, AST flushed) | full DB + AST |
| GET | `/cgi-bin/policy.cgi?op=get` | — | `{accept_unknown, auto_enroll_default_profile, ast_ttl_multiframes, mle_detach_cnt}` | `policy.get` | — | read | DB policy struct (shadow of `0x1A4`/`0x1A8`/`0x1AC`) <!-- TODO: confirm reg names match `docs/references/reference_subscriber_db_arch.md` (those regs were FPGA-side in the old project; now SW-side per migration plan §"FPGA modules to delete from carry-over") --> |
| POST | `/cgi-bin/policy.cgi` | `{op:"put", accept_unknown, auto_enroll_default_profile, ast_ttl_multiframes}` | echo of new values | `policy.put` | `EINVAL` | write DB policy | DB policy struct |

---

### 3. Debug

Read-mostly diagnostics. Some endpoints are streaming (long-poll or
Server-Sent-Events from a dedicated `tetra_d` listener — busybox httpd
3-s timeout still applies, so streamers run as their own CGI that holds the
socket open and emits chunked SSE; daemon side pushes events via socket).

**Views**

| View | Powered by endpoints |
|---|---|
| PDU trace (signalling) | `debug.cgi?op=pdu_recent&dir=rx&n=N`, SSE stream `debug.cgi?op=pdu_stream` |
| AACH timeline (ring buf) | `debug.cgi?op=aach_recent&n=N`, SSE `debug.cgi?op=aach_stream` |
| Slot schedule | `debug.cgi?op=slot_schedule&frames=F` |
| DMA counters | `status.cgi?scope=dma_stats` (same as Live) |
| IRQ counters | `debug.cgi?op=irq_counters` |
| Message-bus tap | SSE `debug.cgi?op=msgbus_stream&filter=src=mle` |
| Layer log tail | `debug.cgi?op=log_tail&layer=mle&n=N` |
| FPGA register read | `debug.cgi?op=reg_read&addr=0x...` |
| TmaSap frame inspector | `debug.cgi?op=tmasap_recent&dir=rx&n=N` |
| TmdSap voice frame counters | `debug.cgi?op=tmdsap_stats` |

**Endpoint table**

| Method | Path | Request | Response data | Daemon op | Errors | Side-effect | Touches |
|---|---|---|---|---|---|---|---|
| GET | `/cgi-bin/debug.cgi?op=pdu_recent&dir=rx\|tx&n=N` | `n` ≤ 1024 | `[{ts_us, dir, sap, ssi, ssi_type, endpoint_id, pdu_len_bits, pdu_hex, decoded:{type, ...}}]` | `debug.pdu_recent` | `ERANGE` | read | PDU ring buffer (per-direction) |
| GET (SSE) | `/cgi-bin/debug.cgi?op=pdu_stream` | optional `filter=` | event stream of same shape | `debug.pdu_stream` | — | read; client holds socket | live tap |
| GET | `/cgi-bin/debug.cgi?op=aach_recent&n=N` | `n` ≤ 4096 | `[{slot, frame, multiframe, aach_word_hex, dl, ul, f1, f2, scheduler_reason}]` | `debug.aach_recent` | — | read | AACH ring buffer |
| GET (SSE) | `/cgi-bin/debug.cgi?op=aach_stream` | — | per-slot stream | `debug.aach_stream` | — | read | live AACH |
| GET | `/cgi-bin/debug.cgi?op=slot_schedule&frames=F` | `F` ≤ 18 | `[{frame, slot1, slot2, slot3, slot4}]` each cell `{kind:MCCH\|TCH\|UNALLOC, owner_ssi?, alloc_handle?}` | `debug.slot_schedule` | — | read | UMAC scheduler current schedule |
| GET | `/cgi-bin/debug.cgi?op=irq_counters` | — | `{dma_ch1, dma_ch2, dma_ch3, dma_ch4, slot_tick, error}` | `debug.irq_counters` | — | read | kernel `/proc/interrupts` parse + daemon counters |
| GET (SSE) | `/cgi-bin/debug.cgi?op=msgbus_stream&filter=...` | filter syntax `src=NAME,dest=NAME,sap=NAME,prio=immediate\|normal\|low` | live `SapMsg{src,dest,sap,msg_type,bytes_hex,ts_us}` | `debug.msgbus_stream` | `EINVAL` (filter parse) | read | message-bus tap |
| GET | `/cgi-bin/debug.cgi?op=log_tail&layer=NAME&n=N` | layer ∈ {phy,lmac,umac,llc,mle,mm,cmce,sndcp,daemon} | `[{ts, level, msg}]` | `debug.log_tail` | `EINVAL` | read | per-layer ring log |
| GET | `/cgi-bin/debug.cgi?op=reg_read&addr=0x...` | addr in allow-listed AXI window | `{addr, value_hex}` | `debug.reg_read` | `EPERM` (out-of-allowlist), `EFPGA` | read | one AXI read |
| POST | `/cgi-bin/debug.cgi` | `{op:"reg_write", addr, value}` (gated, see §5 Tools) | `{addr, prev_hex, new_hex}` | `debug.reg_write` | `EPERM` | **write** AXI | dangerous, see Tools |
| GET | `/cgi-bin/debug.cgi?op=tmasap_recent&dir=rx\|tx&n=N` | — | `[{ts_us, dir, frame_hex, magic, ssi, endpoint_id, req_handle?, report_code?}]` (TMAS+TMAR) | `debug.tmasap_recent` | — | read | TmaSap raw-frame ring |
| GET | `/cgi-bin/debug.cgi?op=tmdsap_stats` | — | `{rx_frames_per_slot:[s1,s2,s3,s4], tx_frames_per_slot, last_acelp_hex}` | `debug.tmdsap_stats` | — | read | TmdSap counters |

---

### 4. Configuration

Writes here mutate live FPGA AXI registers via the daemon **and** persist to
the on-disk config (`/var/lib/tetra/config.json`). Every write returns the
old + new value so the WebUI can show a confirm-diff.

**Field → register map.** Names are pinned in `docs/ARCHITECTURE.md`
§"AXI-Lite Live-Config Register Window" (chapter added 2026-05-03,
Pre-Phase-2 Decision §D-2). All offsets here are relative to the
AXI-Lite slave base `0x4000_0000`. The daemon C-API mirrors these
offsets in `sw/include/tetra/axi_regmap.h` (S0/S7 deliverable).

**Views**

| View | Powered by endpoints |
|---|---|
| Cell identity form | `config.cgi?op=get&group=cell`, POST `op=put&group=cell` |
| RF form (frequencies + power) | `config.cgi?op=get&group=rf`, POST `op=put&group=rf` |
| Cipher / scrambler form | `config.cgi?op=get&group=cipher`, POST |
| Training sequences form | `config.cgi?op=get&group=training`, POST |
| Slot table editor | `config.cgi?op=get&group=slot_table`, POST |
| AD9361 advanced | `config.cgi?op=get&group=ad9361`, POST |
| Apply / Discard pending | `apply.cgi?op=status`, POST `op=apply`, POST `op=discard` |

`config.cgi` is staged: writes go to a "pending" overlay; `apply.cgi` commits
pending → live + persists. This matches the legacy `apply.cgi` and avoids
half-applied multi-field changes.

**Endpoint table**

| Method | Path | Request | Response data | Daemon op | Errors | Side-effect | Touches |
|---|---|---|---|---|---|---|---|
| GET | `/cgi-bin/config.cgi?op=get&group=cell` | — | `{mcc, mnc, cc, la}` (live + pending if differ) | `config.cell.get` | — | read | cfg cell |
| POST | `/cgi-bin/config.cgi` | `{op:"put", group:"cell", mcc, mnc, cc, la}` | `{pending:{...}, diff:[...]}` | `config.cell.put` | `EINVAL` | stage | pending cfg |
| GET | `/cgi-bin/config.cgi?op=get&group=rf` | — | `{rx_freq_hz, tx_freq_hz, tx_power_dbm, rx_gain_db, agc_mode}` | `config.rf.get` | — | read | cfg rf |
| POST | `/cgi-bin/config.cgi` | `{op:"put", group:"rf", rx_freq_hz, tx_freq_hz, tx_power_dbm, rx_gain_db, agc_mode}` | staged diff | `config.rf.put` | `EINVAL` (range), `EPERM` (license-band check, future) | stage | pending cfg |
| GET | `/cgi-bin/config.cgi?op=get&group=cipher` | — | `{cipher_mode, ksg, dck_present, scrambler_init_hex}` | `config.cipher.get` | — | read | cfg cipher |
| POST | `/cgi-bin/config.cgi` | `{op:"put", group:"cipher", cipher_mode, ksg, scrambler_init_hex}` | staged diff | `config.cipher.put` | `EINVAL` | stage | pending cfg |
| GET | `/cgi-bin/config.cgi?op=get&group=training` | — | `{ts_n_hex, ts_p_hex, ts_q_hex, ts_x_hex, ts_y_hex}` (Normal, Pilot, Sync, Extended-1/2) | `config.training.get` | — | read | cfg training |
| POST | `/cgi-bin/config.cgi` | `{op:"put", group:"training", ts_n_hex, ts_p_hex, ts_q_hex, ts_x_hex, ts_y_hex}` | staged diff | `config.training.put` | `EINVAL` | stage | pending cfg |
| GET | `/cgi-bin/config.cgi?op=get&group=slot_table` | — | `{slots:[{slot, frame_filter:"all"\|"odd"\|"even"\|"every-N", purpose:"MCCH"\|"TCH"\|"UNALLOC"}]}` | `config.slot_table.get` | — | read | UMAC scheduler config |
| POST | `/cgi-bin/config.cgi` | `{op:"put", group:"slot_table", slots:[...]}` | staged diff | `config.slot_table.put` | `EINVAL`, `ESCHED` (invalid combination) | stage | pending cfg |
| GET | `/cgi-bin/config.cgi?op=get&group=ad9361` | — | `{rx_bw_hz, tx_bw_hz, rx_sample_rate, tx_sample_rate, rx_rf_port, tx_rf_port, rx_lna, dcxo_trim}` | `config.ad9361.get` | — | read | AD9361 cfg via libiio |
| POST | `/cgi-bin/config.cgi` | `{op:"put", group:"ad9361", ...}` | staged diff | `config.ad9361.put` | `EINVAL`, `EIO` (libiio) | stage | pending cfg |
| GET | `/cgi-bin/config.cgi?op=get&group=msgbus` | — | `{queue_caps:{immediate, normal, low}, dispatch_log_level}` | `config.msgbus.get` | — | read | daemon msgbus cfg |
| POST | `/cgi-bin/config.cgi` | `{op:"put", group:"msgbus", ...}` | staged diff | `config.msgbus.put` | `EINVAL` | stage | pending cfg |
| GET | `/cgi-bin/apply.cgi?op=status` | — | `{has_pending:bool, pending_groups:[...], diff:[...]}` | `apply.status` | — | read | pending overlay |
| POST | `/cgi-bin/apply.cgi` | `{op:"apply"}` | `{applied_groups:[...], requires_ms_powercycle:bool}` | `apply.apply` | `EINVAL` (schema), `EFPGA` (AXI write fail; partial-rollback attempted) | **write** FPGA + persist `config.json` | full live cfg |
| POST | `/cgi-bin/apply.cgi` | `{op:"discard"}` | `{discarded_groups:[...]}` | `apply.discard` | — | drop overlay | pending cfg only |
| GET | `/cgi-bin/config.cgi?op=schema&group=NAME` | — | JSON-Schema for the group (used by WebUI form generator) | `config.schema` | `EINVAL` | read | static |

**Field → AXI register map (informative; pinned in tetra_d source).**

| WebUI field | Group | Live FPGA AXI register | Offset | Width | Persistence |
|---|---|---|---|---|---|
| MCC | cell | `REG_CELL_MCC` | `0x000` | `[9:0]` | `config.json` |
| MNC | cell | `REG_CELL_MNC` | `0x004` | `[13:0]` | `config.json` |
| CC (colour code) | cell | `REG_CELL_CC` | `0x008` | `[5:0]` | `config.json` |
| LA (location area) | cell | `REG_CELL_LA` | `0x00C` | `[13:0]` | `config.json` |
| RX freq | rf | `REG_RX_CARRIER_HZ` (daemon also shadow-writes AD9361 LO via libiio) | `0x010` | `[31:0]` | `config.json` |
| TX freq | rf | `REG_TX_CARRIER_HZ` (daemon also shadow-writes AD9361 LO via libiio) | `0x014` | `[31:0]` | `config.json` |
| TX power dBm | rf | `REG_TX_ATT` (AD9361 attenuator, 0.25 dB units) + `REG_TX_GAIN_TRIM` (signed dB digital trim) | `0x08C` + `0x018` | `[7:0]` + signed `[7:0]` | `config.json` |
| Cipher mode | cipher | `REG_CIPHER_MODE` | `0x01C` | `[1:0]` | `config.json` |
| Scrambler init | cipher | `REG_SCRAMBLER_INIT` | `0x020` | `[31:0]` | `config.json` |
| Training sequence N | training | `REG_TS_N` (Normal Training Sequence) | `0x024` | `[11:0]` | `config.json` |
| Training sequence P | training | `REG_TS_P` (Pilot/Sync Training Sequence) | `0x028` | `[11:0]` | `config.json` |
| Training sequence Q | training | `REG_TS_Q` (Extended Training Sequence) | `0x02C` | `[11:0]` | `config.json` |
| Slot purpose table | slot_table | `SLOT_TABLE` window — 20 entries × 4 bytes; daemon writes the array as a single strided AXI burst. Per-entry layout: `[1:0]` slot_type (RA/Common/Unalloc/Allocated), `[25:2]` assigned_ssi (24-bit), `[29:26]` aach_hint (selector into AACH-Modes table; UMAC clamps invalid combinations) | `0x030..0x07F` | 80 B | `config.json` |
| AST TTL multiframes | policy | SW-side now (per migration plan §"FPGA modules to delete from carry-over"); ex `0x1A8` | — | — | DB |

All names above are pinned against `docs/ARCHITECTURE.md`
§"AXI-Lite Live-Config Register Window". Reset defaults (Gold-Cell:
MCC=262, MNC=1010, CC=1, scrambler=`0x4183_F207`, RX carrier
392_987_500 Hz, TX carrier 382_891_062 Hz) and the carrier-frequency
caveat between Gold captures live in that chapter.

---

### 5. Tools

Operator action surface. Two long-running ops live here (capture, bitstream
switch) and dispatch through the async-job pattern in §9.

**Views**

| View | Powered by endpoints |
|---|---|
| Manual PDU sender | `tools.cgi?op=pdu_send_form` (template), POST `op=pdu_send` |
| Reset counters | POST `tools.cgi?op=reset_counters` |
| Reset AST | POST `tools.cgi?op=reset_ast` |
| Reset DB to factory | POST `db.cgi?op=reset_factory` (cross-link to §2) |
| Bitstream list + switch | `tools.cgi?op=bitstream_list`, POST `op=bitstream_switch` (async) |
| Capture trigger | POST `tools.cgi?op=capture_start` (async), POST `op=capture_stop`, GET `op=capture_list` |
| Decoder upload + run | POST `tools.cgi?op=decoder_upload` (multipart), POST `op=decoder_run` (async), GET `op=decoder_list` |
| Stop daemon | POST `stop.cgi` (graceful SIGTERM, AST snapshot) |
| Restart daemon | POST `tools.cgi?op=restart` (async — systemctl restart) |
| Reg-write (advanced) | POST `tools.cgi?op=reg_write` (gated, defaults disabled) |
| Job queue view | GET `jobs.cgi?op=list`, GET `jobs.cgi?op=get&id=...` |

**Endpoint table**

| Method | Path | Request | Response data | Daemon op | Errors | Side-effect | Touches |
|---|---|---|---|---|---|---|---|
| GET | `/cgi-bin/tools.cgi?op=pdu_send_form&kind=NAME` | kind ∈ {d_setup, d_release, d_nwrk_broadcast, d_loc_update_accept, d_loc_update_reject, d_attach_detach_grp_ack, custom} | `{template:{...}, fields:[...]}` (form schema) | `tools.pdu_send_form` | `EINVAL` | read | static |
| POST | `/cgi-bin/tools.cgi` | `{op:"pdu_send", kind, fields:{...}, target:{ssi, ssi_type, endpoint_id?}, dry_run:bool}` | `{tma_frame_hex, req_handle, would_send:bool, sent_at_us?}` | `tools.pdu_send` | `EINVAL`, `EBUSY` (TX queue full), `EFPGA` | sends one TmaSap-TX (unless dry_run) | TmaSap-TX channel |
| POST | `/cgi-bin/tools.cgi` | `{op:"reset_counters", scopes:["dma","irq","phy","msgbus","layers"]}` | `{cleared:[...]}` | `tools.reset_counters` | — | zero counters; does NOT touch AST/DB | counters only |
| POST | `/cgi-bin/tools.cgi` | `{op:"reset_ast", confirm:true}` | `{flushed_slots:N}` | `tools.reset_ast` | `EPERM` (no confirm) | wipe in-memory AST + delete `ast.json` snapshot | AST |
| POST | `/cgi-bin/tools.cgi` | `{op:"reg_write", addr, value, confirm:"YES_I_AM_SURE"}` | `{addr, prev_hex, new_hex}` | `tools.reg_write` | `EPERM` (no confirm or out-of-allowlist), `EFPGA` | direct AXI write — bypasses staged config | **single AXI reg** |
| GET | `/cgi-bin/tools.cgi?op=bitstream_list` | — | `[{name, path, sha256, kind:"normal"\|"sniffer"\|"emulator"\|"latency", active}]` | `tools.bitstream_list` | — | read | `/lib/firmware/*.bit.bin` |
| POST | `/cgi-bin/tools.cgi` | `{op:"bitstream_switch", name}` | `{job_id}` | `tools.bitstream_switch` | `ENOENT`, `EBUSY` (other long-job running) | enqueues async job (see §9); job will: gracefully stop tetra_d, fpga_manager load, restart tetra_d | FPGA, daemon |
| POST | `/cgi-bin/tools.cgi` | `{op:"capture_start", source:"rx_iq"\|"rx_bits"\|"tx_iq"\|"aach_only", duration_s, name}` | `{job_id}` | `tools.capture_start` | `EBUSY`, `ENOSPC` | enqueues async job that streams to `/var/tetra/captures/<name>.wav` | DMA tap, disk |
| POST | `/cgi-bin/tools.cgi` | `{op:"capture_stop", job_id}` | `{job_id, state}` | `tools.capture_stop` | `ENOENT` | signals job to finalize | job |
| GET | `/cgi-bin/tools.cgi?op=capture_list` | — | `[{name, source, duration_s, size_bytes, ts}]` | `tools.capture_list` | — | read | `/var/tetra/captures/` |
| GET | `/cgi-bin/tools.cgi?op=capture_download&name=NAME` | — | binary stream, `Content-Type: application/octet-stream` | `tools.capture_download` | `ENOENT` | read | capture file |
| POST | `/cgi-bin/tools.cgi` | multipart `op=decoder_upload, file=decode_*.py` | `{name, sha256, size}` | `tools.decoder_upload` | `EINVAL` (name pattern), `E2BIG` | write `/var/tetra/decoders/decode_*.py` | decoders dir |
| GET | `/cgi-bin/tools.cgi?op=decoder_list` | — | `[{name, size, sha256, last_run_ts?}]` | `tools.decoder_list` | — | read | decoders dir |
| POST | `/cgi-bin/tools.cgi` | `{op:"decoder_run", name, capture_name, args?}` | `{job_id}` | `tools.decoder_run` | `ENOENT` | enqueues async job (see §8) | sandbox |
| GET | `/cgi-bin/tools.cgi?op=decoder_result&job_id=ID` | — | `{stdout, stderr, exit_code, artifacts:[...]}` | `tools.decoder_result` | `ENOENT` | read | job output |
| POST | `/cgi-bin/stop.cgi` | `{op:"stop"}` | `{ok:true, ast_snapshotted:bool}` | `daemon.stop` | — | graceful SIGTERM (clean shutdown flag set, AST snapshot to `ast.json`) | daemon lifecycle |
| POST | `/cgi-bin/tools.cgi` | `{op:"restart"}` | `{job_id}` | `daemon.restart` | — | enqueues async job: stop, restart via systemd | daemon lifecycle |
| GET | `/cgi-bin/jobs.cgi?op=list` | — | `[{id, op, state:"queued"\|"running"\|"done"\|"failed"\|"cancelled", started_ts, ended_ts?, progress_pct?}]` | `jobs.list` | — | read | job queue |
| GET | `/cgi-bin/jobs.cgi?op=get&id=ID` | — | full job record incl. log tail | `jobs.get` | `ENOENT` | read | job |
| POST | `/cgi-bin/jobs.cgi` | `{op:"cancel", id}` | `{id, state}` | `jobs.cancel` | `ENOENT`, `EINVAL` (already done) | signal job | job |

---

### 6. JSON request/response convention

Wire format between CGI and daemon over `/run/tetra_d.sock`. Length-prefixed
JSON: 4-byte big-endian length, then UTF-8 JSON body. Single envelope both
directions.

**Request envelope:**

```json
{
  "op":      "<entity>.<verb>",
  "args":    { "...": "..." },
  "req_id":  "<uuid-v4>",
  "client":  "cgi:<scriptname>"
}
```

**Response envelope (success):**

```json
{
  "ok":     true,
  "req_id": "<echo>",
  "data":   { "...": "..." }
}
```

**Response envelope (error):**

```json
{
  "ok":     false,
  "req_id": "<echo>",
  "error":  {
    "code":    "EINVAL",
    "message": "human-readable",
    "field":   "<optional path into args>",
    "detail":  { "...": "..." }
  }
}
```

**Error code catalog.**

| Code | Meaning | Example |
|---|---|---|
| `EINVAL` | Argument schema/range violation | profile.priority > 15 |
| `ENOENT` | Referenced object not found | entity slot empty, decoder file missing |
| `EEXIST` | Duplicate-key violation | entity (id, type) already mapped |
| `EROFS` | Write to read-only object | profile id=0 mutate |
| `EPERM` | Operation gated/forbidden | reg_write without confirm, out-of-allowlist addr |
| `EBUSY` | Resource busy | another async job running, TX queue full |
| `ENOSPC` | Capacity exhausted | entity table full (256 used), capture disk full |
| `ERANGE` | Pagination/length out of bounds | list offset > total |
| `EREF` | Foreign-key violation | entity references missing profile_id |
| `ESCHED` | Slot/scheduler conflict | invalid slot_table combination |
| `EFPGA` | AXI / DMA / FPGA error | reg read/write failed, DMA channel hung |
| `EIO` | Underlying I/O error | libiio AD9361 access |
| `E2BIG` | Upload too large | decoder/file > limit |
| `EINTERNAL` | Daemon bug / unexpected | catch-all; daemon also logs |

CGI HTTP mapping: success → `200 OK`. Any envelope error → `200 OK` with
`ok:false` body (NOT HTTP 4xx/5xx) so the WebUI JS sees a uniform shape.
Real HTTP errors only for: `400` malformed CGI input, `502` socket-connect
to daemon failed, `504` daemon op timeout, `413` upload exceeds CGI limit.

**Streaming (SSE).** When `op` ends in `_stream`, the response is
`Content-Type: text/event-stream`, daemon emits one `data: <json>\n\n` per
event. CGI forwards verbatim until client disconnects or daemon drops the
producer. SSE bypasses the 3-s busybox CGI limit because BusyBox httpd
1.30.1's CGI timeout only fires before the first byte; once the CGI has
written the SSE preamble, the stream stays open.

---

### 7. Auth model and future hardening

**Today:** none. Board #1 lives on the lab LAN
(`192.168.2.180`), no public exposure. BusyBox httpd has no auth configured
(HARDWARE.md §8). `tetra_d` accepts any local connection on the Unix socket;
socket mode `0660 root:tetra` is the only access gate.

**Future hooks** (do not implement now; document the slots):

1. **CGI-side origin check.** Add a thin shared wrapper `webui_auth_check()`
   called as the first line of every CGI; reads `REMOTE_ADDR`, optionally an
   `Authorization:` header, and a token file `/etc/tetra/webui.token`. If the
   token file does not exist, behave as today (allow). Once it exists, require
   bearer match.
2. **Daemon-side ACL.** `tetra_d` gains a config group `webui_acl` with
   per-op allow/deny lists, e.g. read-ops always allowed, mutating ops behind
   token. Slot: `config.webui_acl.{get,put}`.
3. **HTTPS.** BusyBox httpd 1.30.1 supports `-p 443` with a TLS wrapper
   (`stunnel` is the typical pattern on Debian-bullseye). Out of scope for
   migration; document as deploy step.
4. **Audit log.** Daemon already logs every write op; future hardening turns
   this into a separate append-only `webui_audit.log` with the request
   envelope + REMOTE_ADDR.
5. **Per-route rate limiting.** Daemon-side token bucket per (`client`,
   `op`-prefix). Today CGIs are trusted; once a token exists, rate-limit
   mutating ops.

Where each hook plugs in:

| Hook | File | When |
|---|---|---|
| `webui_auth_check()` | `sw/webui/cgi_common.c` | first call inside `main()` of every `*.cgi` |
| Daemon ACL | `sw/webui/socket_handler.c` | between envelope-parse and `op_dispatch` |
| HTTPS | deploy script (`scripts/deploy.sh`) and `httpd.conf` | post-cutover |
| Audit log | `sw/persistence/audit.c` | `tools.*`, `*.put`, `*.delete`, `apply.apply` |
| Rate limit | `sw/webui/ratelimit.c` | wraps `op_dispatch` |

---

### 8. Decoder-upload workflow

Tools tab feature: upload a `decode_*.py` script and re-decode a previously
captured WAV. Used to iterate decoder logic without redeploying the daemon.

**Constraints (enforced in daemon `tools.decoder_upload`):**

- **Filename:** must match `^decode_[a-z0-9_]{1,32}\.py$`. Reject otherwise.
- **Directory:** files land in `/var/tetra/decoders/` only. No path traversal.
- **Size:** ≤ 256 KiB per file (`E2BIG` otherwise).
- **MIME / first-bytes sniff:** must start with `#!` shebang or `#` comment,
  must not contain NUL bytes; reject ELF / archive magic.
- **Persistence:** stored as plain `.py` files, no compilation cache shipped.
- **Quota:** total `/var/tetra/decoders/` ≤ 16 MiB; oldest auto-evicted on
  upload if exceeded (FIFO by mtime).

**Execution model (`tools.decoder_run`):**

- Run as async job (§9) — never inline in the CGI 3-s window.
- Spawned process: `python3 /var/tetra/decoders/<name> --input /var/tetra/captures/<capture>.wav [--args ...]`.
- Sandbox: dropped privileges (uid `tetra:tetra`), `RLIMIT_CPU=120 s`,
  `RLIMIT_AS=512 MiB`, `RLIMIT_FSIZE=64 MiB`, `setpgid` so cancel kills the
  whole tree, `chroot` not used (Python stdlib is paths-heavy).
- Working dir = a per-job tmp dir under `/var/tetra/jobs/<job_id>/`,
  cleaned up after `decoder_result` is fetched once or after 1 hour, whichever
  is first.
- Stdout/stderr captured into the job record (capped at 1 MiB each, truncated
  with marker line).
- Artifacts: anything written to the job tmp dir is listed in the result
  envelope and downloadable via `tools.cgi?op=decoder_artifact&job_id=...&name=...`.
- Exit code surfaced verbatim; non-zero ≠ daemon error (job state stays
  `done`, not `failed`, unless the runtime itself blew up).

**Out of scope for migration:** Python venv per decoder, package install on
the board. Decoders must run with the board's stock Python 3.7 (Kuiper
bullseye) plus whatever the existing `scripts/decode_*.py` already used. If
a decoder needs new packages, that's a board image bump, not a runtime
upload.

---

### 9. Async jobs (capture trigger, bitstream switch, decoder run, daemon restart)

These ops cannot complete inside the busybox httpd 3-s exit window. The CGI
POSTs the request, the daemon enqueues a job, returns `{job_id}` immediately,
and the WebUI polls `jobs.cgi`.

**Job record (daemon-side, also returned by `jobs.get`):**

```json
{
  "id":           "<uuid>",
  "op":           "tools.bitstream_switch",
  "args":         { "...": "..." },
  "state":        "queued | running | done | failed | cancelled",
  "progress_pct": 0,
  "started_ts":   "...",
  "ended_ts":     "...",
  "result":       { "...": "..." },
  "error":        { "code": "...", "message": "..." },
  "log_tail":     [ "..." ]
}
```

**Lifecycle:**

1. CGI sends `{op:"tools.bitstream_switch", name:"sniffer.bit.bin"}` →
   daemon validates args, allocates job, returns `{job_id}` synchronously.
2. Daemon worker picks up next job (single worker for FPGA-mutating ops,
   serialized; separate worker for capture; separate worker for decoder runs).
3. Worker emits progress via `jobs.update` internal calls; UI polls every
   1 s on `jobs.cgi?op=get&id=...`.
4. Terminal state (`done`/`failed`/`cancelled`) is sticky; record retained
   for 1 hour or until WebUI calls `jobs.cancel` on a `done` job (acts as
   acknowledge + delete).

**Cancellation semantics per op:**

| Op | Cancellable? | Cancel effect |
|---|---|---|
| `tools.bitstream_switch` | only while `queued` | dropped; `running` is uninterruptible (would leave FPGA in bad state) |
| `tools.capture_start` | yes, anytime | finalize WAV with current data, mark `cancelled` |
| `tools.decoder_run` | yes, anytime | SIGTERM to process group, then SIGKILL after 5 s |
| `daemon.restart` | only while `queued` | dropped |

**Single-worker invariants for FPGA-mutating jobs:**

`tools.bitstream_switch` and `daemon.restart` cannot run concurrently with
**any** other op. They take a global `EBUSY`-lock. CGI returns `EBUSY` if
another such job is already running.

`tools.capture_start` does not take the global lock (capture coexists with
normal traffic). `tools.decoder_run` is fully off-board-FPGA — runs against
files only — and is parallelizable up to a small worker pool (suggested 2).

**Bitstream switch sequence (worker steps):**

1. `progress=5`: validate target file exists in `/lib/firmware/`.
2. `progress=10`: signal daemon's own clean-shutdown (AST snapshot, DB flush).
3. `progress=30`: `fpga_manager` writes new `.bit.bin`.
4. `progress=70`: re-init AXI-DMA descriptors against new bitstream's DT
   overlay (per HARDWARE.md §4).
5. `progress=90`: re-load DB from JSON, re-arm message bus.
6. `progress=100`: `state=done`, result includes new `bitstream_id` and
   `requires_ms_powercycle:true` (per memory `feedback_announce_ms_restart`).

**Capture-trigger sequence:**

1. `progress=0..1`: open output file, claim DMA tap.
2. `progress=N` updated each second based on `elapsed_s / duration_s`.
3. Cancel or `capture_stop` finalizes the WAV header, releases tap.
4. Result: `{path, size_bytes, sample_count}`.

---

## Operator Procedures

### After SW or RTL Deploy that affects DL signalling

Per memory `feedback_announce_ms_restart`:
**Power-cycle each MTP3550** before observing behaviour. MS otherwise stays
in backoff/failsafe and ignores correct changes.

### Capture a WAV from the board

(TBD — depends on board-side capture tool implementation.)

### Reset Subscriber DB to defaults

WebUI → Subscriber DB → Reset to factory. (Profile 0 always 0x0000_088F.)

## Monitoring

- WebUI Live Status auto-refreshes counters
- `tetra_d` daemon log: `/var/log/tetra_d.log`
- DMA error counters exposed via WebUI debug page
