# sw/llc/ — Logical Link Control

Owned by S2 (`S2-sw-llc`). Locked under interface contract `IF_LLC_v1`.

ETSI EN 300 392-2 §22 + bluestation `llc/`.

## Files

- `include/tetra/llc.h` — public API (locked under `IF_LLC_v1`)
- `llc.c` — entity (state machine + retransmission + msgbus glue)
- `llc_pdu.c` — bit-exact PDU encode/decode + CRC-32 FCS

## PDU types implemented

| Code | Name | NR | NS | FCS | Notes |
|------|------|----|----|-----|-------|
| 0x0  | BL-ADATA      | yes | yes | no  | Gold-Ref DL#735 layout |
| 0x1  | BL-DATA       | no  | yes | no  | Gold-Ref UL#0 layout |
| 0x2  | BL-UDATA      | no  | no  | no  | broadcast (D-NWRK-BCAST) |
| 0x3  | BL-ACK        | yes | no  | no  | Gold-Ref UL#2 layout |
| 0x4  | BL-ADATA+FCS  | yes | yes | yes | +FCS variant |
| 0x5  | BL-DATA+FCS   | no  | yes | yes | +FCS variant |
| 0x6  | BL-UDATA+FCS  | no  | no  | yes | +FCS variant |
| 0x7  | BL-ACK+FCS    | yes | no  | yes | +FCS variant |
| 0x8  | AL-SETUP      | n/a | n/a | no  | DL#727 wrapper, no body |

## CRC-32 polynomial

`0x04C11DB7` (IEEE 802.3 generator), init `0xFFFFFFFF`, xorout `0xFFFFFFFF`,
NOT reflected (bits feed MSB-first to match BitBuffer / on-air bit order).

The §22 ETSI PDF is not in `docs/references/`. The Gold-Ref M2 captures
all use no-FCS variants of LLC PDUs (DL#735 BL-ADATA = "kein FCS",
UL#0 BL-DATA, UL#2 BL-ACK), so we have no on-air bit-vector to validate
the FCS polynomial. FCS verification is **round-trip-only** until the
§22 PDF is added — see `<-- TODO: confirm polynomial -->` markers in
`llc_pdu.c` and `include/tetra/llc.h`.

## Test gate

`tb/sw/llc/test_llc_pdu.c` — 20 Unity test cases, all PASS:
- BL-DATA NS=0/NS=1 round-trip
- BL-ACK NR=0/NR=1 round-trip (incl. with MLE-PD body)
- BL-ADATA round-trip + BL-ADATA+FCS round-trip
- BL-UDATA round-trip (byte-aligned 120-bit) + 124-bit (D-NWRK-BCAST size)
- AL-SETUP wrapper round-trip (bit-pattern verified vs Gold-Ref DL#727)
- BL-DATA gold-ref UL#0 first-12-bits layout match
- NS counter wraps modulo-2; in-sequence RX advances NR; out-of-sequence
  RX re-ACKs without forwarding
- CRC-32 known-vector + empty-input
- Bad-args + short-frame + invalid-pdu_type negative paths
- Endpoint slot allocation/saturation
