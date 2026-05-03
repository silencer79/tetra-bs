#!/usr/bin/env python3
"""ref_mac_resource_dl.py — Python reference for tetra_mac_resource_dl_builder.

Builds a 268-bit MAC-RESOURCE PDU bit-by-bit per the bluestation-aligned
algorithm documented in `rtl/umac/tetra_mac_resource_dl_builder.v` (mirrors
bluestation `mac_resource.rs::to_bitbuf`).  Used by tb_mac_resource_dl_builder
to generate the bit-exact reference vector.
"""

PDU_BITS = 268


def write_field(buf, start, width, value):
    """Place width-bit `value` MSB-first starting at on-air position `start`."""
    for i in range(width):
        b = (value >> (width - 1 - i)) & 1
        buf[start + i] = b


def build_mac_resource_dl(*,
                          ssi,
                          addr_type=0b001,
                          nr=0,
                          ns=0,
                          llc_pdu_type=0,           # 0=BL-ADATA, 1=BL-DATA
                          random_access_flag=0,
                          power_control_flag=0,
                          power_control_element=0,
                          slot_granting_flag=0,
                          slot_granting_element=0,
                          chan_alloc_flag=0,
                          chan_alloc_element=0,
                          chan_alloc_element_len=0,
                          mm_bits=None):
    if mm_bits is None:
        mm_bits = []
    mm_len = len(mm_bits)
    # mac_hdr_bits = 40 base + 3 mandatory flag bits + optional element bits
    mac_hdr_bits = (40 + 3
                    + (4 if power_control_flag else 0)
                    + (8 if slot_granting_flag else 0)
                    + (chan_alloc_element_len if chan_alloc_flag else 0))
    if llc_pdu_type == 0:    # BL-ADATA
        llc_hdr_bits = 6
    elif llc_pdu_type == 1:  # BL-DATA
        llc_hdr_bits = 5
    else:
        raise NotImplementedError(f"llc_pdu_type {llc_pdu_type}")
    mle_pd_bits = 3
    llc_cov_len = llc_hdr_bits + mle_pd_bits + mm_len
    mac_total_bits = mac_hdr_bits + llc_cov_len
    mac_total_octets = (mac_total_bits + 7) // 8
    length_ind = mac_total_octets
    fill_bit_ind = (mac_total_bits % 8) != 0

    buf = [0] * PDU_BITS
    # Base 40 bits
    write_field(buf, 0, 2, 0b00)
    write_field(buf, 2, 1, 1 if fill_bit_ind else 0)
    write_field(buf, 3, 1, 0)               # PoG
    write_field(buf, 4, 2, 0b00)            # encryption
    write_field(buf, 6, 1, random_access_flag)
    write_field(buf, 7, 6, length_ind)
    write_field(buf, 13, 3, addr_type)
    write_field(buf, 16, 24, ssi)
    pos = 40
    # Mandatory flags + optional elements
    write_field(buf, pos, 1, power_control_flag); pos += 1
    if power_control_flag:
        write_field(buf, pos, 4, power_control_element); pos += 4
    write_field(buf, pos, 1, slot_granting_flag); pos += 1
    if slot_granting_flag:
        write_field(buf, pos, 8, slot_granting_element); pos += 8
    write_field(buf, pos, 1, chan_alloc_flag); pos += 1
    if chan_alloc_flag:
        write_field(buf, pos, chan_alloc_element_len, chan_alloc_element)
        pos += chan_alloc_element_len
    assert pos == mac_hdr_bits, (pos, mac_hdr_bits)
    # LLC + MLE-PD + MM body
    if llc_pdu_type == 0:    # BL-ADATA: link(1) fcs(1) bl(2)=00 nr(1) ns(1)
        write_field(buf, pos, 1, 0); pos += 1
        write_field(buf, pos, 1, 0); pos += 1
        write_field(buf, pos, 2, 0b00); pos += 2
        write_field(buf, pos, 1, nr); pos += 1
        write_field(buf, pos, 1, ns); pos += 1
    else:                    # BL-DATA: link(1) fcs(1) bl(2)=01 ns(1)
        write_field(buf, pos, 1, 0); pos += 1
        write_field(buf, pos, 1, 0); pos += 1
        write_field(buf, pos, 2, 0b01); pos += 2
        write_field(buf, pos, 1, ns); pos += 1
    write_field(buf, pos, 3, 0b001); pos += 3   # MLE-PD MM
    for i, b in enumerate(mm_bits):
        buf[pos + i] = b
    pos += mm_len
    assert pos == mac_total_bits
    if fill_bit_ind:
        buf[pos] = 1
    return buf, dict(mac_hdr_bits=mac_hdr_bits, mac_total_bits=mac_total_bits,
                     mac_total_octets=mac_total_octets, length_ind=length_ind,
                     fill_bit_ind=int(fill_bit_ind))


def bits_to_hex_msb_first(bits, nbits):
    val = 0
    for i, b in enumerate(bits):
        val |= (b << (nbits - 1 - i))
    return f"{nbits}'h{val:0{(nbits + 3) // 4}x}"


if __name__ == "__main__":
    mm = [(i + 1) % 2 for i in range(80)]   # 1010… 80 bits
    pdu, meta = build_mac_resource_dl(
        ssi=0x282FF4,
        addr_type=0b001,
        nr=0,
        ns=0,
        llc_pdu_type=0,            # BL-ADATA
        random_access_flag=0,
        slot_granting_flag=1,
        slot_granting_element=0x00,
        mm_bits=mm,
    )
    print("# DL#735-shaped scenario (80-bit MM body 1010…)")
    for k, v in meta.items():
        print(f"#   {k} = {v}")
    print(f"PDU = {bits_to_hex_msb_first(pdu, 268)}")
