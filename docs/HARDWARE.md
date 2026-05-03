# Hardware

## Boards

### Board #1 — Migration Target

- LibreSDR (Zynq-7020 + AD9361 RFIC)
- IP: `root@192.168.2.180`, password `openwifi`
- Receives every new bitstream + SW build
- Primary live-test target

### Board #2 — Special Tasks

- Identical LibreSDR (Zynq-7020 + AD9361)
- IP: TBD (Kevin to assign)
- Use cases:
  - UL/DL sniffer (RX-only, listen to Board #1 transmissions)
  - MS emulator (transmit timed UL stimulus for latency measurement)
  - CMCE-Voice capture (with two MTP3550 in same group)
  - Side-by-side bit-diff against Board #1

## Test MS

- 2× **Motorola MTP3550** with **identical firmware**
- Codeplug-differentiated (different ISSI: e.g. `0x282F91` and `0x282F92`)
- Power-cycle required after each BS-side DL signalling change (memory: feedback_announce_ms_restart)

## Toolchain (TODO-D — pending audit)

This section to be filled by TODO-D session. Required entries:

- Vivado version (project last built with 2022.2)
- ARM cross-compiler (`arm-linux-gnueabihf-gcc` version)
- Linux kernel on board (for axi_dma driver compatibility)
- JSON library: jansson vs cJSON (decide)
- C unit-test framework: Unity vs Check (decide)
- Verilator version (co-sim)
- xilinx_axidma userspace driver source
- busybox httpd CGI conventions
- Host gcc (for SW unit-tests on x86)

## AD9361 Configuration

Carried over from `tetra-zynq-phy`. See `rtl/phy/` headers for sample rate,
filter taps, etc. Validated on-air (2026-04-25 .. 2026-05-02 logs).

## Pinout / Constraints

`constraints/libresdr_tetra.xdc` (carried over from old project).

## Power-On / Boot Sequence

1. Board boots from SD card (PetaLinux/Buildroot)
2. `init` script loads `system.bit` via `fpga_manager` (or U-Boot equivalent)
3. SW daemon `tetra_d` started by `init` script
4. `httpd` started for WebUI

Exact init script: see `scripts/deploy.sh` after carry-over + adaptation.

## TX Path Validated, Hands-Off

Per memory `feedback_tx_path_hands_off`: TX chain (modulator, RRC pulse, CIC,
up-conversion, AD9361 TX) is validated. Bugs are typically in RX or higher
layers, not TX.
