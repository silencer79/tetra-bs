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

## Toolchain + Dependencies

**Audit date:** 2026-05-03. Versions captured from current host (`SDR`, Ubuntu 24.04
noble) and Board #1 (`192.168.2.180`, ADI Kuiper bullseye). Re-run when migrating
hosts or reflashing the board SD card.

### 1. FPGA build chain

| Item | Version | Path | License | Status |
|---|---|---|---|---|
| Vivado | **2022.2** (SW Build 3671981, 2022-10-14) | `/opt/Xilinx/Vivado/2022.2/bin/vivado` | Xilinx ML Standard / Webpack — Webpack is free for `xc7z020` (Zynq-7020) | probed, in use |
| `bootgen` | shipped with Vivado 2022.2 | `/opt/Xilinx/Vivado/2022.2/bin/bootgen` (loaded via `settings64.sh`) | Xilinx | probed, used by `scripts/deploy.sh` to convert `.bit` → `.bit.bin` |
| AXI DMA IP core | LogiCORE `axi_dma:7.1` | bundled in Vivado IP catalog | Webpack-eligible (free) | required for 4× DMA channels (decision #3) |
| Constraints | `constraints/libresdr_tetra.xdc` | repo | repo (GPL-2.0) | carried over from `tetra-zynq-phy`, validated |

**Pin to 2022.2.** Mixing Vivado versions across runs has caused IP-version
drift in the carry-over project. Do not bump until the full RTL+TB suite has
been re-validated under the new tools.

**`axi_dma:7.1` IP config — locked.** Pinned in `scripts/build/synth.tcl`
via `create_ip` + `set_property -dict CONFIG.*`. Mirrors the carry-over
`tetra/build/vivado/.../tetra_system_axi_dma_0_0.xci` with both directions
enabled (the wrapper drives `DIR_IS_S2MM` per channel; same IP variant
serves all 4 instances):

| Param | Value | Notes |
|---|---|---|
| `c_include_sg` | 1 | Scatter-Gather enabled |
| `c_sg_length_width` | 14 | carry-over default |
| `c_sg_include_stscntrl_strm` | 1 | status/control SG stream |
| `c_include_mm2s` | 1 | both directions per IP — direction selected per-channel by wrapper |
| `c_include_mm2s_dre` | 0 | MM2S unaligned DRE off |
| `c_include_mm2s_sf` | 1 | MM2S store-and-forward |
| `c_m_axi_mm2s_data_width` | 32 | PS-DDR HP slave geometry |
| `c_m_axis_mm2s_tdata_width` | 32 | matches IF_AXIDMA_v1 fabric width |
| `c_include_s2mm` | 1 | |
| `c_include_s2mm_dre` | 1 | S2MM unaligned DRE on (carry-over) |
| `c_include_s2mm_sf` | 1 | S2MM store-and-forward |
| `c_m_axi_s2mm_data_width` | 32 | |
| `c_s_axis_s2mm_tdata_width` | 32 | |
| `c_s2mm_burst_size` | 256 | carry-over user value |
| `c_addr_width` | 32 | |
| `c_micro_dma` | 0 | |
| `c_enable_multi_channel` | 0 | |
| `c_increase_throughput` | 0 | |

**Status (2026-05-03, branch `feat/synth-ip-bringup`):** IP creation works,
OOC synth of the IP itself completes; top-level `synth_design` of
`tetra_top` currently FAILS at `tetra_axi_dma_wrapper` elaboration because
the wrapper's per-channel `axi_dma_channel_inst` instantiation uses a
slim, custom port-list/parameter shape (`CHANNEL_ID`, `DIR_IS_S2MM`,
slim AXIS+AXI4-MM only) that matches the simulation behavioural model at
`tb/rtl/models/axi_dma_v7_1_bhv.v` but NOT the real Xilinx IP's full
port-list (S_AXI_LITE control slave, full burst signals, separate SG
master, mm2s/s2mm_introut, etc.). A synthesis-only RTL shim under
`rtl/infra/ip/axi_dma_channel_inst.v` is required to bridge the two —
that shim is the natural follow-up to A1 (see TODO in
`scripts/build/synth.tcl`).

### 2. ARM cross-compile (target = Zynq-7020, Cortex-A9, ARMv7-A + VFPv3 + NEON)

| Item | Version | Path | License | Status |
|---|---|---|---|---|
| `arm-linux-gnueabihf-gcc` (Ubuntu system) | **GCC 13.3.0** (Ubuntu meta-pkg `gcc-arm-linux-gnueabihf 4:13.2.0-7ubuntu1` → `gcc-13` 13.3.0-6ubuntu2~24.04.1) | `/usr/bin/arm-linux-gnueabihf-gcc` | GPL-3.0 (compiler), system libs LGPL | installed; **preferred for daily builds — must invoke by full path** (see PATH note) |
| `arm-linux-gnueabihf-gcc` (Vitis wrapper) | **GCC 11.2.0** (wrapper script → `arm-xilinx-linux-gnueabi-gcc.real`, repackaged by Xilinx under the gnueabihf name) | `/opt/Xilinx/Vitis/2022.2/gnu/aarch32/lin/gcc-arm-linux-gnueabi/bin/arm-linux-gnueabihf-gcc` | Xilinx EULA + GPL components | first hit on PATH after `settings64.sh`; reserve for kernel-module rebuilds |
| ABI | `gnueabihf` (hard-float) — Cortex-A9 has VFPv3 + NEON, confirmed in board `/proc/cpuinfo` |  | — | locked |
| Recommended flags | `-march=armv7-a -mfpu=neon-vfpv3 -mfloat-abi=hard -O2` |  | — | derive from board `/proc/cpuinfo` (`half thumb fastmult vfp edsp neon vfpv3 tls vfpd32`) |

**PATH-precedence trap.** Both the Ubuntu-system gcc and the Vitis-bundled gcc
register the **same command name** `arm-linux-gnueabihf-gcc`. The Xilinx
`settings64.sh` prepends the Vitis path, so a bare `arm-linux-gnueabihf-gcc`
invocation in any shell sourced from Vitis-init resolves to the **11.2 wrapper,
not the 13.3 system compiler** — confirmed via `type -a` on the audit host:

```
arm-linux-gnueabihf-gcc → /opt/Xilinx/Vitis/2022.2/.../arm-linux-gnueabihf-gcc   (first hit, GCC 11.2.0)
arm-linux-gnueabihf-gcc → /usr/bin/arm-linux-gnueabihf-gcc                       (second hit, GCC 13.3.0)
```

For SW daily builds, either (a) invoke `/usr/bin/arm-linux-gnueabihf-gcc`
explicitly in `Makefile`s (recommended — pin via `CC := /usr/bin/...`), or
(b) un-source Vivado/Vitis env in the SW build shell. Do **not** rely on PATH
order. Board kernel itself was built with Vitis GCC 12.2.0 (per
`/proc/version`); userspace targets board glibc, so any modern gcc 11–13 in
the hard-float ABI links cleanly.

### 3. Board #1 runtime environment (probed live)

| Item | Value | Source |
|---|---|---|
| Distro | Kuiper GNU/Linux 11.2 (Debian bullseye, ADI flavor) | `/etc/os-release` |
| Kernel | **5.10.0-98248-g1bbe32fa5182-dirty** (built 2025-01-05 by `pavel@linux-work-station`) | `uname -a`, `/proc/version` |
| Kernel build toolchain | arm-xilinx-linux-gnueabi-gcc 12.2.0 + GNU ld 2.39.0.20220819 | `/proc/version` |
| Architecture | `armv7l`, ARM Cortex-A9 (CPU part 0xc09), VFPv3+NEON | `/proc/cpuinfo` |
| RAM | 1 GB (≈155 MiB free at idle), 100 MiB swap | `free -h` |
| Rootfs | 14 GB SD card (~3.3 GB free at audit time) | `df -h /` |
| Init | systemd (Debian `init.d/` services + units), **not** BusyBox init | `/etc/init.d/`, `ps` |
| BusyBox (board) | **v1.30.1 (Raspbian 1:1.30.1-6)** — provides `httpd`, basic utils | `busybox --help` |
| HTTP server | `busybox httpd` listening on `:80` (PID 2002 at probe) | `ss -tlnp` |
| Document root | `/www/index.html`, CGI in `/www/cgi-bin/*.cgi`, mode 0755 | observed |
| AD9361 stack | `iiod` on `:30431`, `ad9361_drv.ko` loaded (lsmod) | `lsmod`, `ss -tlnp` |
| FPGA load | `fpga_manager` consumes `/lib/firmware/<name>.bit.bin` | `scripts/deploy.sh` |

### 4. AXI-DMA driver path

The migration plan calls for 4× AXI-DMA channels (TmaSap RX/TX, TmdSap RX/TX,
ARCHITECTURE.md). Both ends of the stack come from the existing tooling — no
3rd-party vendoring:

- **FPGA side:** Xilinx LogiCORE `axi_dma:7.1` IP from the Vivado catalog
  (Webpack-eligible for `xc7z020`). Carry-over from `tetra-zynq-phy` build at
  `tetra/build/vivado/.../tetra_system_axi_dma_0_0.xci` confirms our prior
  config (Scatter-Gather + S2MM_DRE + `c_addr_width=32`). Wrapped 4× by
  `rtl/infra/tetra_axi_dma_wrapper.v` (Agent A1).
- **Linux kernel driver:** in-tree `xilinx_dma.ko` already shipped on Board #1
  under `/root/kernel_modules32/` (probed 2026-05-03). Matches DT compatible
  `xlnx,axi-dma-1.00.a`. `modprobe xilinx_dma` and the four channels described
  in `dts/tetra_axi_dma_overlay.dtsi` bind. **No 3rd-party kernel module.**
- **Userspace:** thin glue around the standard Xilinx char-dev path. The
  daemon (`sw/dma_io/dma_io.c`, Agent S1) defines `IF_DMA_API_v1`
  (`dma_init`/`dma_send_frame`/`dma_recv_frame`/…) with a pipe-mock backend
  for x86 host unit-tests and a `HAVE_XILINX_DMA`-gated real-HW path that
  opens `/dev/dma_proxy_*` (or equivalent xilinx_dma char-dev) per DT-label
  channel. Real-HW path is a Phase-3 task: confirm the board's exact char-dev
  shape in-situ before wiring the ioctl/mmap calls.

**Decision (Kevin, 2026-05-03, revised after libaxidma audit):** use the
**in-tree Vivado/Xilinx stack only** — `axi_dma:7.1` IP + `xilinx_dma.ko` +
own thin C glue. No `libaxidma`/`xilinx_axidma` 3rd-party vendoring. The
earlier suggestion to vendor `jacobfeder/xilinx_axidma` was withdrawn after
that URL turned out to be 404 and the existing tooling already covers the
stack end-to-end.

### 5. SW dependencies

| Item | Choice | Version on host | License | Rationale |
|---|---|---|---|---|
| **C standard** | C11 | (gcc default) | — | per CLAUDE.md |
| **JSON lib** | **jansson** | 2.14-2build2 (runtime `libjansson4` installed; **`libjansson-dev` NOT yet installed** — `apt install libjansson-dev` before SW build) | MIT | mature, RFC 7159, opaque ref-counted handles, simpler API for the daemon's persistence layer; cJSON is the backup if a smaller footprint is ever needed |
| **C unit-test framework** | **Unity** (ThrowTheSwitch) | not yet vendored | MIT | header-only, no fork(), trivial cross-compile, embedded-friendly. Check needs `fork()` + libsubunit and is awkward across cross-toolchains. Vendor under `sw/external/unity/` at a pinned tag (e.g. `v2.6.0`) |
| **Host gcc** (x86 unit-tests) | GCC 13.3.0 (Ubuntu 24.04) | system | GPL-3.0 | probed |
| **JSON lib on board** | jansson | needs `libjansson4` cross-built or static-link | MIT | board does not have libjansson installed by default; statically link into `tetra_d` to avoid runtime dep |

### 6. RTL simulation chain

| Item | Version | Path | License | Status |
|---|---|---|---|---|
| Icarus Verilog | **12.0 (stable)** | `/usr/bin/iverilog` | LGPL-2.1 / GPL-2 | probed; primary for `tb/rtl/` |
| **Verilator** | candidate **5.020-1** (Ubuntu noble) | NOT installed yet — `apt install verilator` | LGPL-3 / Artistic-2.0 | required for Phase 3 co-sim (`tb/cosim/`); install before Phase 3 starts |
| Vivado xsim | 2022.2 (bundled) | via `vivado -mode batch` | Xilinx | secondary, only for vendor-IP-heavy TBs |

### 7. Deploy chain

| Item | Version | License | Status |
|---|---|---|---|
| `bash` | host system | — | used by `scripts/deploy.sh` |
| `sshpass` | system | GPL-2.0 | password auth to board (`openwifi`) — see `scripts/deploy.sh` |
| `scp`, `ssh` | OpenSSH system | BSD-style | — |
| Python 3 | **3.12.3** | PSF | for `scripts/decode_*.py` analyzers |

### 8. WebUI (busybox httpd) CGI conventions

Probed against running `busybox httpd` v1.30.1 on Board #1:

- **Document root:** `/www/`
- **CGI directory:** `/www/cgi-bin/` — any executable file here is invoked as
  CGI when the URL matches `/cgi-bin/<name>`. BusyBox httpd does not require an
  explicit `cgi-bin` directive in `httpd.conf`; the directory name is enough.
- **CGI invocation contract** (BusyBox `httpd.c`):
  - stdin = request body (POST), stdout = HTTP response (CGI must emit
    `Content-Type:` header, blank line, then body).
  - Env vars: `REQUEST_METHOD`, `QUERY_STRING`, `CONTENT_LENGTH`, `CONTENT_TYPE`,
    `PATH_INFO`, `REMOTE_ADDR`, `HTTP_*`, `SERVER_PROTOCOL`, `SCRIPT_NAME`.
  - Working dir = directory of the CGI binary.
  - Default exit timeout: ~3 s (BusyBox-compiled-in); long-running operations
    must dispatch to the daemon over the Unix socket and return immediately.
- **Config (optional):** `/etc/httpd.conf` or `/www/httpd.conf` — neither is
  present on Board #1, so defaults apply: no auth, no MIME overrides beyond
  built-ins, all files under `/www/` served.
- **Auth model:** none today. WebUI authority lives in `tetra_d` (decision #9);
  CGIs only marshal JSON over the Unix socket. Per migration plan: keep daemon
  authoritative, do not move state into CGIs.
- **Existing CGI corpus on board** (from `tetra-zynq-phy`, replaced wholesale
  in big-bang): `apply.cgi entities.cgi policy.cgi profiles.cgi sessions.cgi
  status.cgi stop.cgi`. Use as endpoint-name reference, not as code source.

**BusyBox version mismatch:** host is 1.36.1, board is 1.30.1. CGI semantics
above are stable across that range, but if a feature is needed that landed
post-1.30 (e.g. newer error-page handling), test on the board, not on host.

### 9. What we are NOT depending on

Recorded explicitly to avoid re-litigation:

- No SystemVerilog tooling (CLAUDE.md). VHDL only inside vendor IP cores.
- No C++ runtime, no Rust toolchain, no Cargo. Bluestation is read-only inspiration.
- No Apache / nginx / lighttpd — busybox httpd is sufficient.
- No `net_brew`, `net_telemetry`, `net_control` from bluestation (CLAUDE.md).
- No commercial simulator (Synopsys VCS, Cadence Xcelium): IcarusVerilog +
  Verilator + Vivado xsim cover all RTL test needs in-house.

### 10. Open follow-ups out of this audit

- [x] Pin `libaxidma` strategy (Section 4) — withdrawn; using in-tree `xilinx_dma.ko` + own thin glue, no 3rd-party vendoring (revised 2026-05-03).
- [ ] `apt install verilator` on host before Phase 3 co-sim work begins.
- [ ] `apt install libjansson-dev` on host before first SW build (only runtime
      `libjansson4` is on the host today; headers + `.pc` are missing).
- [ ] Pin `CC := /usr/bin/arm-linux-gnueabihf-gcc` in the SW Makefile so daily
      builds use Ubuntu 13.3 deterministically, regardless of whether the
      shell has Vitis `settings64.sh` sourced (Section 2 PATH-precedence trap).
- [x] Vendor `unity` (`sw/external/unity/v2.6.0`) — done by Agent T0.
- [ ] Phase-3: confirm board's xilinx_dma char-dev path (`/dev/dma_proxy_*` or equivalent) and finish `sw/dma_io/dma_io.c` real-HW backend behind `HAVE_XILINX_DMA`.
- [ ] Bake DT nodes for the 4× AXI-DMA instances into the new bitstream's PL
      overlay; current DT only has the single `axidmatest@1` carry-over node.
- [ ] Decide whether to statically link `libjansson` into `tetra_d` or ship
      `libjansson4.deb` as part of deploy. Lean: static, avoids board pkg-mgmt.

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
