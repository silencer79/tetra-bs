#!/usr/bin/env bash
# scripts/build/setup-host.sh — idempotent host bootstrap for tetra-bs builds.
#
# Owned by T0 build-skeleton. Closes the relevant follow-ups in
# docs/HARDWARE.md §10 by ensuring the audited dependencies are installed.
# Re-runnable: only installs missing packages, then prints a probe-vs-expected
# table so you can eyeball the toolchain.
#
# What this script DOES NOT do (out of scope; see HARDWARE.md):
#   - Vivado 2022.2 install (Xilinx-EULA, manual)
#   - Vitis cross-compiler (we use the Ubuntu system 13.3 explicitly)
#   - Board-side driver build (handled by S1, on-target)
#
# Per HARDWARE.md §10 follow-ups this script handles:
#   [x] apt install verilator         (Phase 3 cosim prerequisite)
#   [x] apt install libjansson-dev    (host build — only libjansson4 runtime
#                                      ships on Ubuntu 24.04 by default)
#   [x] arm-linux-gnueabihf-gcc       (Ubuntu 13.3.0 — the *system* one;
#                                      we then pin /usr/bin/... in the Makefile
#                                      to avoid the Vitis PATH trap from §2)
#   [x] gcc + iverilog                (host TB toolchain — usually present, but
#                                      we double-check)
#   [x] build-essential + make        (foundation, harmless to re-state)
#
# Exit codes:
#   0 — host is good (or was made good); table printed
#   1 — apt install failed or required tool still missing afterwards

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# ---- Audited expected versions (from docs/HARDWARE.md §1, §2, §5, §6) -----
declare -A EXPECTED
EXPECTED[gcc]="13.3.0"
EXPECTED[arm-linux-gnueabihf-gcc]="13.3.0"
EXPECTED[iverilog]="12.0"
EXPECTED[verilator]="5.x"          # Ubuntu noble ships 5.020-1
EXPECTED[libjansson-dev]="2.14"

# ---- Packages to ensure (apt names) ---------------------------------------
APT_PKGS=(
    build-essential
    make
    gcc
    iverilog
    verilator
    libjansson-dev
    gcc-arm-linux-gnueabihf
    file                 # used by `make sw-build` to print ELF kind
    pkg-config
)

# ---- Helpers --------------------------------------------------------------
say() { printf '[setup-host] %s\n' "$*"; }

probe_version() {
    local tool="$1"
    case "$tool" in
        gcc)
            /usr/bin/gcc -dumpfullversion 2>/dev/null || echo "missing"
            ;;
        arm-linux-gnueabihf-gcc)
            # Probe explicitly via /usr/bin to avoid the Vitis PATH trap
            # documented in HARDWARE.md §2.
            /usr/bin/arm-linux-gnueabihf-gcc -dumpfullversion 2>/dev/null \
                || echo "missing"
            ;;
        iverilog)
            /usr/bin/iverilog -V 2>/dev/null \
                | awk '/Icarus Verilog version/ {print $4; exit}' \
                || echo "missing"
            ;;
        verilator)
            verilator --version 2>/dev/null \
                | awk '{print $2; exit}' \
                || echo "missing"
            ;;
        libjansson-dev)
            dpkg-query -W -f='${Version}\n' libjansson-dev 2>/dev/null \
                | awk -F- '{print $1}' \
                || echo "missing"
            ;;
        *)
            echo "unknown-probe-for-$tool"
            ;;
    esac
}

# ---- Install ---------------------------------------------------------------
need_install=()
for p in "${APT_PKGS[@]}"; do
    if ! dpkg -s "$p" >/dev/null 2>&1; then
        need_install+=("$p")
    fi
done

if [ "${#need_install[@]}" -gt 0 ]; then
    say "apt install: ${need_install[*]}"
    if [ "$(id -u)" -ne 0 ]; then
        sudo apt-get update
        sudo apt-get install -y "${need_install[@]}"
    else
        apt-get update
        apt-get install -y "${need_install[@]}"
    fi
else
    say "all apt packages already present (skipping install)."
fi

# ---- Probe + print table ---------------------------------------------------
say "probe-vs-expected version table:"
printf '\n'
printf '  %-32s %-16s %-16s %s\n' "tool" "expected" "probed" "status"
printf '  %-32s %-16s %-16s %s\n' "----" "--------" "------" "------"

bad=0
for tool in gcc arm-linux-gnueabihf-gcc iverilog verilator libjansson-dev; do
    exp="${EXPECTED[$tool]}"
    got="$(probe_version "$tool")"
    if [ "$got" = "missing" ]; then
        status="MISSING"
        bad=1
    elif [[ "$got" == "$exp"* || "$exp" == *"x"* ]]; then
        status="ok"
    else
        # Drift but present — warn only, do not fail; HARDWARE.md is the
        # source of truth and a drift means the audit needs an update.
        status="drift (audit?)"
    fi
    printf '  %-32s %-16s %-16s %s\n' "$tool" "$exp" "$got" "$status"
done
printf '\n'

# ---- Final verdict ---------------------------------------------------------
if [ "$bad" -ne 0 ]; then
    say "FAIL — at least one required tool is missing after install attempt."
    exit 1
fi

say "OK — host has the audited toolchain."
say "Next: cd $REPO_ROOT && make help"
