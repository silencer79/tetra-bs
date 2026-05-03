# sw/cmce/

Circuit Mode Control Entity (CMCE) — Phase 2 deliverable from agent
S4-sw-cmce per `docs/MIGRATION_PLAN.md` §"Agent Topology" §S4.

Locked under `IF_CMCE_v1` (see `include/tetra/cmce.h`).

## Files

- `cmce.c`            — main entity (msgbus glue + dispatcher)
- `cmce_pdu.c`        — bit-exact encoder/decoder per
  `docs/references/reference_cmce_group_call_pdus.md`
- `cmce_fsm.c`        — per-call state machine
- `cmce_nwrk_bcast.c` — periodic D-NWRK-BROADCAST emitter (Gold cadence
  ≈10 s per `docs/references/reference_gold_full_attach_timeline.md`
  §"D-NWRK-BROADCAST-Cadence")
- `include/tetra/cmce.h` — public API

## Caveats

CMCE Group-Call PDU field values are PROVISIONAL — bluestation+ETSI
defaults, no Gold-Ref. See `cmce_pdu.c` header banner and
`docs/references/gold_field_values.md` §"Open uncertainties" #5.

D-NWRK-BROADCAST is the SOLE CMCE-adjacent PDU with Gold-Ref backing
(Burst #423 → `GOLD_INFO_124` from
`scripts/gen_d_nwrk_broadcast.py`). Conservative encoder default
(`tetra_network_time = None`, o-bit = 0) is documented in
`gold_field_values.md` §"Konservativer Default" — surfaces a 1-bit
divergence at gold position 79 + 25 trailing-bit divergences in the open-
uncertainty region [80..123]. Tests in `tb/sw/cmce/` quantify the diff.

## Tests

`make sw-test` runs all three test binaries:
- `tb/sw/cmce/test_cmce_d_nwrk_broadcast.c` — Gold #423 bit-diff +
  periodic-driver tests
- `tb/sw/cmce/test_cmce_pdu.c`              — round-trip for 8 CMCE PDU types
- `tb/sw/cmce/test_cmce_fsm.c`              — M3 happy-path FSM
