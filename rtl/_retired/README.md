# rtl/_retired/ — bluestation-non-conformant carry-over relocations

Files relocated here per `docs/MIGRATION_PLAN.md` §"FPGA modules to delete from
carry-over". They are kept under `rtl/_retired/` (not deleted from the repo)
so git history remains traceable, but they are NOT picked up by Vivado synth
(no Vivado-Tcl target globs `rtl/_retired/`) and NOT compiled by `make tb`.

Owner: Agent A5 (`A5-fpga-top-xdc-cleanup`).

## Inventory

The following carry-over RTL files from `tetra-zynq-phy` were on the
delete-list. For each: status at the time of A5 execution.

| File | Function | Replacement | Status in tetra-bs `rtl/` |
|---|---|---|---|
| `tetra_ul_demand_ie_parser.v` | UL IE-Parser (GILD, demand fields) | SW Agent S3 (`sw/mm/mm_iep.c`) | absent (never carried over) |
| `tetra_mle_registration_fsm.v` (+ submodules) | MLE registration FSM | SW Agent S3 (`sw/mle/mle_fsm.c`) | absent |
| `tetra_entity_table.v` | Subscriber Entity table | SW Agent S5 (`sw/persistence/db.c`) | absent |
| `tetra_profile_table.v` | Subscriber Profile table | SW Agent S5 (`sw/persistence/db.c`) | absent |
| `tetra_active_session_table.v` | Active Session Table (AST) | SW Agent S5 (`sw/persistence/ast_snapshot.c`) | absent |
| `tetra_d_location_update_encoder.v` | D-LOCATION-UPDATE-ACCEPT builder | SW Agent S3 (`sw/mm/mm_accept_builder.c`) | absent |
| `tetra_d_location_update_reject_encoder.v` | D-LOCATION-UPDATE-REJECT builder | SW Agent S3 (`sw/mm/mm_accept_builder.c`) | absent |
| AXI register modules `REG_SHADOW_*`, `REG_PROFILE_*`, `REG_DB_POLICY` | Subscriber-DB AXI register decoder | SW Agent S5 (DB pure SW) | absent |
| AXI register `REG_AACH_GRANT_HINT` | SW-side AACH override hint | dropped — UMAC scheduler determines AACH internally (CLAUDE.md "No SW-Override-Pfade für AACH"; AACH grant-hint forbidden) | absent |

All entries are **absent** from `rtl/` in the tetra-bs carry-over snapshot.
A5 verified that no module under `rtl/{phy,lmac,umac,infra}/` references any
of these files (grep clean as of 2026-05-03).

The `rtl/_retired/` directory therefore holds no Verilog files at this time.
This README itself is the only artefact; it documents the verified-absent
state so a future re-introduction (e.g. accidental copy from `tetra-zynq-phy`)
gets caught by review.

## Re-introduction prevention

A5's `tetra_top.v` does NOT instantiate any of the retired modules and the
`tetra_axi_lite_regs.v` register decoder explicitly returns 0 for the
forbidden register names per `docs/ARCHITECTURE.md` §"Carry-over registers
explicitly NOT carried":

- `REG_SHADOW_*`, `REG_PROFILE_*`, `REG_DB_POLICY` → reads return 0, writes
  dropped.
- `REG_AACH_GRANT_HINT` → reads return 0, writes dropped.

If a future agent attempts to add one of these names back to the register
decoder, the `aach_hint` field in the `SLOT_TABLE` window
(`docs/ARCHITECTURE.md` §"SLOT_TABLE window") is the only legal SW influence
on AACH selection (and even that is "hint, not override" per CLAUDE.md).
