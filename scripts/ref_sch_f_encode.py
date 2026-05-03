#!/usr/bin/env python3
"""ref_sch_f_encode.py — Python reference for tetra_sch_f_encoder.v

Mirrors the FPGA's SCH/F encode chain bit-for-bit:
    268 type-1 info bits
      → CRC-16-CCITT (poly 0x1021, init 0xFFFF, ones-complement)
      → 288 type-3 = info(268) + ~CRC(16) + 4-bit zero tail
      → rate-1/4 mother (G1=10011, G2=11101) punctured to rate-2/3
        (even = g1+g2, odd = g1)  → 432 type-4 bits
      → multiplicative interleaver  j = 1 + (a*k) mod N, a=103, N=432
      → Fibonacci scrambler, taps at bits 0,6,9,10,16,20,21,22,24,25,
        27,28,30,31, init = scramble_init
      → 432 type-5 coded bits.

Bit convention (matches RTL): bits[0] is the first on-air bit.
Used by tb_sch_f_encoder to generate the bit-exact reference vector.
"""

import sys


def crc16_ccitt(bits):
    crc = 0xFFFF
    for b in bits:
        fb = b ^ ((crc >> 15) & 1)
        crc = ((crc << 1) & 0xFFFF) ^ (0x1021 if fb else 0)
    inv = (~crc) & 0xFFFF
    return [(inv >> i) & 1 for i in range(15, -1, -1)]


def rcpc_r23(t3_bits):
    out = []
    sr = [0, 0, 0, 0]  # sr[0..3]
    bit_phase = 0
    for din in t3_bits:
        sr3, sr2, sr1, sr0 = sr[3], sr[2], sr[1], sr[0]
        # G1 mask 5'b10011 over {sr3,sr2,sr1,sr0,din} → sr3 ^ sr0 ^ din
        g1 = sr3 ^ sr0 ^ din
        # G2 mask 5'b11101 → sr3 ^ sr2 ^ sr1 ^ din
        g2 = sr3 ^ sr2 ^ sr1 ^ din
        if bit_phase == 0:
            out += [g1, g2]
        else:
            out += [g1]
        bit_phase ^= 1
        sr = [din, sr[0], sr[1], sr[2]]
    assert len(out) == 432, len(out)
    return out


def interleave_a103(rcpc):
    a, n = 103, 432
    out = [0] * n
    for k in range(1, n + 1):
        j = 1 + (a * k) % n
        out[j - 1] = rcpc[k - 1]
    return out


def scramble(bits, init):
    taps = [0, 6, 9, 10, 16, 20, 21, 22, 24, 25, 27, 28, 30, 31]
    lfsr = init
    out = []
    for b in bits:
        fb = 0
        for t in taps:
            fb ^= (lfsr >> t) & 1
        out.append(b ^ fb)
        lfsr = ((fb << 31) | (lfsr >> 1)) & 0xFFFFFFFF
    return out


def sch_f_encode(info_bits, scramble_init):
    assert len(info_bits) == 268
    crc = crc16_ccitt(info_bits)
    t3 = info_bits + crc + [0, 0, 0, 0]
    rcpc = rcpc_r23(t3)
    inter = interleave_a103(rcpc)
    return scramble(inter, scramble_init)


def bits_to_hex_msb_first(bits, nbits):
    """Pack so bits[0] = MSB of the integer (= bus index nbits-1 in RTL)."""
    val = 0
    for i, b in enumerate(bits):
        val |= (b << (nbits - 1 - i))
    return f"{nbits}'h{val:0{(nbits + 3) // 4}x}"


def main():
    # Default test pattern: 268 bits 1010… (info[0]=1, info[1]=0, …)
    info = [(i + 1) % 2 for i in range(268)]   # = [1,0,1,0,...]
    scramble_init = 0x4183F207   # Gold-Cell value
    coded = sch_f_encode(info, scramble_init)
    print("# Test pattern: alternating 1010… (268 bits)")
    print(f"INFO  = {bits_to_hex_msb_first(info, 268)}")
    print(f"INIT  = 32'h{scramble_init:08x}")
    print(f"CODED = {bits_to_hex_msb_first(coded, 432)}")


if __name__ == "__main__":
    main()
