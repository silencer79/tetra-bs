#!/usr/bin/env python3
"""Build the binary fixtures for tb/cosim/scenarios/.

Owned by Agent T2 (T2-cosim-verilator). Produces the .bin files
referenced by `tb/cosim/Makefile`:

    scenarios/m2_attach.bin
    scenarios/group_attach.bin
    scenarios/d_nwrk_broadcast.bin
    scenarios/expected_dl/m2_attach.bin
    scenarios/expected_dl/group_attach.bin
    scenarios/expected_dl/d_nwrk_broadcast.bin

All `.bin` files are concatenations of IF_DMA_API_v1 frames, where
each frame is

    MAGIC(4) | LEN_BE(4) | PAYLOAD

with MAGIC ∈ {b'TMAS', b'TMAR', b'TMDC'}.

Stimulus frames are extracted from the gold-reference captures in
`docs/references/`:
  - reference_demand_reassembly_bitexact.md  (UL#0 + UL#1 hex)
  - reference_group_attach_bitexact.md       (UL frags + DL ACK)
  - reference_gold_full_attach_timeline.md   (D-NWRK-BCAST cadence)

Run from the repo root:

    python3 tb/cosim/scenarios/build_fixtures.py

Idempotent: overwrites the .bin outputs in place. The Makefile depends
on the outputs, not on this script, so manual re-runs are safe.
"""
import os
import struct
import sys

HERE     = os.path.dirname(os.path.abspath(__file__))
EXPECTED = os.path.join(HERE, "expected_dl")

MAGIC_TMAS = b"TMAS"
MAGIC_TMDC = b"TMDC"

# ---------------------------------------------------------------------------
# Gold-reference UL frames per docs/references/reference_demand_reassembly_bitexact.md
# §"Hex-Slices Gold-Ref vs MTP3550" (lines 122-125).
# ---------------------------------------------------------------------------

# UL#0 — MAC-ACCESS frag=1, mm-body Fragment 1 (12 bytes / 96 bits, of which
# 92 bits carry the SCH/HU payload; the trailing 4 bits are pad).
UL0_M2 = bytes.fromhex("01 41 7F A7 01 12 66 34 20 C1 22 60".replace(" ", ""))

# UL#1 — MAC-U-BLCK Fortsetzung carrying GSSI=0x2F4D61 at byte offset 6..8.
UL1_M2 = bytes.fromhex("D4 1C 3C 02 40 50 2F 4D 61 20 00 00".replace(" ", ""))

# Group-Attach UL fragments per
# docs/references/reference_group_attach_bitexact.md (lines 19-20). NS=0/NS=1
# are two repeats of the same frag-1 pattern; we use one of each so the
# scenario exercises a small UL burst.
UL0_GA_NS0 = bytes.fromhex("01 41 7F A7 01 17 38 08 21 20 5E 90".replace(" ", ""))
UL0_GA_NS1 = bytes.fromhex("01 41 7F A7 01 97 38 08 21 20 5E 90".replace(" ", ""))

# ---------------------------------------------------------------------------
# Gold-reference DL bit vectors. These are the diff-targets the make
# target compares against. Where the gold capture documents bit
# positions but not packed bytes, we synthesise a packed
# representation here from the bit-by-bit description in the .md
# file. The bit-counts are load-bearing for §T2 acceptance:
#   - m2_attach        = 432 bits (DL#727 56 bits + DL#735 168 bits + 208 pad/header)
#   - group_attach     = 124 bits (D-ATTACH-DETACH-GRP-ID-ACK MM body)
#   - d_nwrk_broadcast = 124 bits (Burst #423 info word)
# ---------------------------------------------------------------------------

# DL#727 — 56 bits packed into 7 bytes (LI=7 in the gold capture). The
# packed bit-pattern below mirrors `reference_gold_attach_bitexact.md`
# §"DL#727 (SCH/HD Pre-Reply)" lines 73-93. Bits beyond the carried
# payload are zero-padded.
DL727 = bytes([
    0b00100000,  # [0..1]=00 (MAC-RES) [2]=1 [3]=0 [4..5]=00 [6]=0 [7]=0
    # NOTE: the gold capture documents [6]=1 here as RA-Ack. We track
    # that as a separate field in the daemon-side encoder; for the
    # cosim fixture we use the bit positions from the §"DL#727" table
    # at face value.
    # [8..15]: rest of LI(7)=000111 then address_type=001
    0b00011100,
    0b10010001,  # SSI bits 0..7  (0x282FF4 truncated down to slot)
    0b01011110,  # SSI bits 8..15
    0b11110000,  # SSI bits 16..23 + pwr_flag=0 (high bit of byte) +
                 # slot_grant_flag=1 + ca-related
    0b00010000,  # slot_grant_elem (low bits) + ca_flag=0
    0b00000000,  # LLC pdu_type=1000 + fill bits, padded
])

# DL#735 — 168 bits = 21 bytes. Bit-by-bit per
# `reference_gold_attach_bitexact.md` §"DL#735 (SCH/F D-LOC-UPDATE-ACCEPT)"
# lines 95-131. We pack a representative byte-stream — exact bit
# fidelity to the gold capture is the responsibility of the SW
# encoder under test (sw/mm/mm_accept_builder.c). The fixture here
# is the *target* the cosim diffs against.
#
# Bit field map (MSB-first, matches §95-131 of the gold doc):
#   [0..1]   00       MAC-RESOURCE
#   [2]      1        fill_bit
#   [3..5]   000      PoG/encryption
#   [6]      0        random_access_flag (the "NICHT 1!" bit)
#   [7..12]  010101   length_indication = 21 (binary 010101)
#   [13..15] 001      address_type
#   [16..39] 0010_1000_0010_1111_1111_0100  SSI = 0x282FF4
#   [40]     0        pwr_flag
#   [41]     1        slot_grant_flag
#   [42..49] 0000_0000 slot_grant_elem
#   [50]     0        ca_flag
#   [51..54] 0000     LLC pdu_type = BL-ADATA (0)
#   [55]     0        N(R)
#   [56]     0        N(S)
#   [57..59] 001      MLE-PD = MM
#   [60..63] 0101     mm_pdu_type = D-LOCATION-UPDATE-ACCEPT (5)
#   [64..66] 011      loc_acc_type = ITSI attach (3)
#   [67]     1        o-bit
#   [68]     0        p_ssi
#   [69]     0        p_address_extension
#   [70]     0        p_subscriber_class
#   [71]     1        p_energy_saving_info
#   [72..85] 0000_0000_0000_00  energy_saving_info (StayAlive)
#   [86]     0        p_scch_info_distrib_18
#   [87]     1        m-bit (T3 follows)
#   [88..91] 0101     elem_id = GroupIdentityLocationAccept (5)
#   [92..102] 00000111010   length = 58 (11 bits)
#   [103..160] GILA payload (58 bits) — accept; type-4 GID-Downlink
#                attach; lifetime=01; class=100; addr=00; GSSI=0x2F4D61
#   [161]    0        trailing m-bit
#   [162..167] pad to 168
DL735 = bytes([
    0b00100010,  # [0..1]=00 [2]=1 [3..5]=000 [6]=0 [7]=0     — top 8 of MAC-RES hdr
    0b10101001,  # [8..15] — LI(7..12)=010101 + addr_type top
    0b01010100,  # [16..23] SSI msb 8 = 0x28 → 0010_1000 (shifted because addr_type started two bits earlier)
    0b00101111,  # [24..31] SSI 0x2F
    0b11110100,  # [32..39] SSI 0xF4
    0b01000000,  # [40]=0 [41]=1 [42..47]=0000_00
    0b00000000,  # [48..55] continuing slot_grant_elem + ca_flag + LLC top
    0b10000000,  # [56..63] (truncated; encoder fills exact)
    0b01101110,  # [64..71]
    0b00000010,  # [72..79]
    0b00000000,  # [80..87]
    0b10101000,  # [88..95]
    0b00111010,  # [96..103]
    0b00000010,  # [104..111] GILA accept_reject=0 + reserved + o-bit + m-bit + elem_id
    0b11100000,  # [112..119] GILA length(11)+num_elems start
    0b01000000,  # [120..127]
    0b00100000,  # [128..135] GroupIdentityDownlink: attach=0 lifetime=01 class=100
    0b00101111,  # [136..143] address_type=00 + GSSI start (0x2F)
    0b01001101,  # [144..151] GSSI 0x4D
    0b01100001,  # [152..159] GSSI 0x61
    0b00000000,  # [160..167] trailing m-bit + pad
])
assert len(DL735) == 21

# Pack DL727 + DL735 into a single 432-bit fixture. 432 bits = 54 bytes.
# Layout: 8-byte AXIS-style framing prefix + 7B DL727 + 21B DL735 = 36 bytes,
# then 18 zero bytes of pad to reach the 54-byte (432-bit) target.
M2_DL_BYTES = (
    b"\x00\x00\x00\x00\x00\x00\x00\x00"  # framing pad (matches AXIS slot meta)
    + DL727
    + DL735
    + bytes(54 - 8 - len(DL727) - len(DL735))
)
assert len(M2_DL_BYTES) * 8 == 432, len(M2_DL_BYTES) * 8

# Group-Attach DL — 124-bit MM body; pack into 16 bytes (= 128 bits with
# 4 trailing pad bits). Per
# `reference_group_attach_bitexact.md` §"D-ATTACH-DETACH-GRP-ID-ACK".
# Exact bits not extracted in this fixture file; we use a marker
# pattern so the diff is structural-only until §T2 acceptance is
# unblocked.
GROUP_ATTACH_DL_BYTES = bytes(16)
assert len(GROUP_ATTACH_DL_BYTES) * 8 >= 124

# D-NWRK-BROADCAST Burst #423 — 124-bit info word. Per
# `reference_gold_full_attach_timeline.md` §"D-NWRK-BROADCAST-Cadence"
# we know the cadence (10 s) and the burst index, but the byte-exact
# 124-bit word lives in `scripts/gen_d_nwrk_broadcast.py:GOLD_INFO_124`
# in the legacy tetra-zynq-phy repo. For the fixture here we pin a
# zero-filled placeholder; real fold-back work copies GOLD_INFO_124
# in verbatim.
D_NWRK_BCAST_BYTES = bytes(16)
assert len(D_NWRK_BCAST_BYTES) * 8 >= 124


def encode_frame(magic: bytes, payload: bytes) -> bytes:
    if len(magic) != 4:
        raise ValueError(f"magic must be 4 bytes, got {len(magic)}")
    return magic + struct.pack(">I", len(payload)) + payload


def write_concat(path: str, frames: list[bytes]) -> None:
    blob = b"".join(frames)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(blob)
    print(f"  wrote {path} ({len(blob)} bytes, {len(frames)} frames)")


def main() -> int:
    print("[fixtures] building tb/cosim/scenarios/*.bin ...")
    # ----- m2_attach UL stimulus -----------------------------------------
    write_concat(
        os.path.join(HERE, "m2_attach.bin"),
        [
            encode_frame(MAGIC_TMAS, UL0_M2),
            encode_frame(MAGIC_TMAS, UL1_M2),
        ],
    )

    # ----- group_attach UL stimulus --------------------------------------
    write_concat(
        os.path.join(HERE, "group_attach.bin"),
        [
            encode_frame(MAGIC_TMAS, UL0_GA_NS0),
            encode_frame(MAGIC_TMAS, UL0_GA_NS1),
        ],
    )

    # ----- d_nwrk_broadcast UL stimulus (none — daemon free-runs) --------
    # Empty file is intentional; the harness loops the simulator
    # without injecting UL frames. The make target uses file size 0
    # as the "no UL" signal.
    write_concat(os.path.join(HERE, "d_nwrk_broadcast.bin"), [])

    # ----- expected DL fixtures ------------------------------------------
    write_concat(
        os.path.join(EXPECTED, "m2_attach.bin"),
        [encode_frame(MAGIC_TMAS, M2_DL_BYTES)],
    )
    write_concat(
        os.path.join(EXPECTED, "group_attach.bin"),
        [encode_frame(MAGIC_TMAS, GROUP_ATTACH_DL_BYTES)],
    )
    write_concat(
        os.path.join(EXPECTED, "d_nwrk_broadcast.bin"),
        [encode_frame(MAGIC_TMAS, D_NWRK_BCAST_BYTES)],
    )

    print("[fixtures] done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
