#!/bin/bash
# =============================================================================
# deploy.sh — Build, Convert, Upload
# Project: tetra-bs (carry-over 1:1 from tetra-zynq-phy, minimal adapt)
#
# Pipeline from Vivado source to files on the LibreSDR:
#   1. Vivado synthesis + implementation + bitstream generation
#   2. bootgen .bit → .bit.bin conversion (FPGA Manager format)
#   3. Cross-compile tetra_d daemon + 12 CGI binaries
#   4. SCP upload bitstream + binaries + WebUI to LibreSDR
#
# After deploy, run manually on the board:
#   ./scripts/tetra_ctrl.sh full_init
#   ./scripts/tetra_ctrl.sh rf_loopback
#
# Usage:
#   ./scripts/deploy.sh              # full pipeline (build + convert + compile + upload)
#   ./scripts/deploy.sh --no-build   # skip Vivado build (use existing .bit)
#   ./scripts/deploy.sh --no-sw      # skip SW compile + upload
#   ./scripts/deploy.sh --build-only # only run Vivado build
#   ./scripts/deploy.sh --init       # also run full_init + tetra_sysinfo after upload
#
# Prerequisites:
#   - Vivado 2022.2 (auto-detected or in PATH)
#   - arm-linux-gnueabihf-gcc (cross compiler)
#   - sshpass, scp, ssh
#   - Board accessible: root@192.168.2.180
# =============================================================================

set -euo pipefail

# =============================================================================
# Configuration
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_ROOT}/build"

BOARD_IP="192.168.2.180"
BOARD_USER="root"
BOARD_PASS="openwifi"

BITSTREAM_NAME="tetra_bs"
BIT_FILE="${BUILD_DIR}/vivado/${BITSTREAM_NAME}.bit"
BIN_FILE="${BUILD_DIR}/vivado/${BITSTREAM_NAME}.bit.bin"

REMOTE_FW_DIR="/lib/firmware"
REMOTE_BIN_DIR="/root"

# Flags
DO_BUILD=true
DO_SW=true
DO_INIT=false  # off by default — opt-in with --init

# =============================================================================
# Argument parsing
# =============================================================================

for arg in "$@"; do
    case "$arg" in
        --no-build)   DO_BUILD=false ;;
        --no-sw)      DO_SW=false ;;
        --build-only) DO_BUILD=true; DO_SW=false ;;
        --init)       DO_INIT=true ;;
        -h|--help)
            echo "Usage: $0 [--no-build] [--no-sw] [--build-only] [--init]"
            echo ""
            echo "  --no-build    Skip Vivado build (use existing .bit)"
            echo "  --no-sw       Skip SW cross-compile + upload"
            echo "  --build-only  Only run Vivado build, nothing else"
            echo "  --init        Also run full_init + tetra_sysinfo after upload"
            exit 0
            ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

# =============================================================================
# Helper functions
# =============================================================================

ssh_cmd() {
    sshpass -p "$BOARD_PASS" ssh -o StrictHostKeyChecking=no "$BOARD_USER@$BOARD_IP" "$@"
}

scp_to() {
    sshpass -p "$BOARD_PASS" scp -o StrictHostKeyChecking=no "$1" "$BOARD_USER@$BOARD_IP:$2"
}

step() {
    echo ""
    echo "=== $1 ==="
}

fail() {
    echo "ERROR: $1" >&2
    exit 1
}

# =============================================================================
# Step 1: Vivado Build
# =============================================================================

if $DO_BUILD; then
    step "1/4: Vivado Build"

    # Find Vivado
    if command -v vivado &>/dev/null; then
        VIVADO=vivado
    elif [ -x /opt/Xilinx/Vivado/2022.2/bin/vivado ]; then
        VIVADO=/opt/Xilinx/Vivado/2022.2/bin/vivado
    else
        fail "Vivado not found. Source settings64.sh or install Vivado 2022.2"
    fi

    echo "Using: $VIVADO"
    echo "Building..."

    cd "$PROJECT_ROOT"
    # Delete stale bitstream first — otherwise a synth failure leaves the
    # previous .bit in place and the existence check below passes silently.
    rm -f "$BIT_FILE"
    set +e
    make synth 2>&1 | tee "${BUILD_DIR}/vivado_build.log" | \
        grep -E "^(Phase|INFO.*Timing|ERROR|WARNING.*timing|Build|Bitstream|\[synth\])"
    viv_rc=${PIPESTATUS[0]}
    set -e
    if [ "$viv_rc" -ne 0 ]; then
        fail "make synth failed (exit $viv_rc) — see ${BUILD_DIR}/vivado_build.log"
    fi

    if [ ! -f "$BIT_FILE" ]; then
        fail "Bitstream not generated: $BIT_FILE"
    fi

    echo "Bitstream: $BIT_FILE ($(stat -c %s "$BIT_FILE") bytes)"
else
    step "1/4: Vivado Build [SKIPPED]"
    [ -f "$BIT_FILE" ] || fail "No bitstream found: $BIT_FILE"
fi

# =============================================================================
# Step 2: Convert .bit → .bit.bin
# =============================================================================

step "2/4: Bitstream Conversion (.bit → .bit.bin)"
# tetra-bs adapt: scripts/build/synth.tcl runs bootgen as part of `make
# synth`, so the .bit.bin is already on disk. Just verify it exists.
[ -f "$BIN_FILE" ] || fail "$BIN_FILE missing — re-run 'make synth' (bootgen step lives there)"
echo "Output: $BIN_FILE ($(stat -c %s "$BIN_FILE") bytes)"

# Stop here if build-only
if $DO_BUILD && ! $DO_SW && [ "${1:-}" = "--build-only" ]; then
    echo ""
    echo "Build complete. Run: $0 --no-build to upload."
    exit 0
fi

# =============================================================================
# Step 3: Cross-compile SW (tetra_d daemon + 12 CGI binaries)
# =============================================================================

if $DO_SW; then
    step "3/4: Cross-Compile sw/"

    CROSS=arm-linux-gnueabihf-gcc
    if ! command -v $CROSS &>/dev/null; then
        fail "$CROSS not found. Install: apt install gcc-arm-linux-gnueabihf"
    fi

    DAEMON_BIN="${BUILD_DIR}/arm/tetra_d"
    CGI_DIR="${BUILD_DIR}/arm/cgi-bin"

    echo "Building tetra_d + 12 CGIs (ARM hard-float, static)..."
    make -C "$PROJECT_ROOT" sw-build
    [ -x "$DAEMON_BIN" ] || fail "tetra_d not produced at $DAEMON_BIN"
    echo "  tetra_d : $(stat -c %s "$DAEMON_BIN") bytes"
    if [ -d "$CGI_DIR" ]; then
        for f in "$CGI_DIR"/*.cgi; do
            [ -f "$f" ] || continue
            echo "  cgi     : $(basename "$f") ($(stat -c %s "$f") bytes)"
        done
    fi
else
    step "3/4: Cross-Compile sw/ [SKIPPED]"
fi

# =============================================================================
# Step 4: Upload to board
# =============================================================================

step "4/4: Upload to ${BOARD_IP}"

# Check board reachable
if ! sshpass -p "$BOARD_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$BOARD_USER@$BOARD_IP" "echo OK" &>/dev/null; then
    fail "Board not reachable at ${BOARD_IP}"
fi

# Kill running tetra_d before upload
ssh_cmd "killall tetra_d 2>/dev/null || true; rm -f /var/run/tetra_d.sock"

# Upload bitstream
echo "Uploading bitstream..."
scp_to "$BIN_FILE" "${REMOTE_FW_DIR}/${BITSTREAM_NAME}.bit.bin"

# Verify bitstream
LOCAL_MD5=$(md5sum "$BIN_FILE" | cut -d' ' -f1)
REMOTE_MD5=$(ssh_cmd "md5sum ${REMOTE_FW_DIR}/${BITSTREAM_NAME}.bit.bin" | cut -d' ' -f1)
if [ "$LOCAL_MD5" != "$REMOTE_MD5" ]; then
    fail "MD5 mismatch! Local=$LOCAL_MD5 Remote=$REMOTE_MD5"
fi
echo "Bitstream verified (MD5: $LOCAL_MD5)"

# Upload tetra_d daemon + 12 CGI binaries
if $DO_SW; then
    DAEMON_BIN="${BUILD_DIR}/arm/tetra_d"
    CGI_DIR="${BUILD_DIR}/arm/cgi-bin"
    WEBUI_HTML="${PROJECT_ROOT}/sw/webui/index.html"

    echo "Uploading tetra_d..."
    scp_to "$DAEMON_BIN" "${REMOTE_BIN_DIR}/tetra_d"
    ssh_cmd "chmod +x ${REMOTE_BIN_DIR}/tetra_d"

    if [ -d "$CGI_DIR" ]; then
        echo "Uploading 12 CGI binaries..."
        ssh_cmd "mkdir -p /www/cgi-bin"
        for f in "$CGI_DIR"/*.cgi; do
            [ -f "$f" ] || continue
            scp_to "$f" "/www/cgi-bin/$(basename "$f")"
        done
        ssh_cmd "chmod +x /www/cgi-bin/*.cgi"
    fi

    if [ -f "$WEBUI_HTML" ]; then
        echo "Uploading WebUI index.html..."
        scp_to "$WEBUI_HTML" "/www/index.html"
    fi
    echo "sw binaries + WebUI uploaded"
fi

# =============================================================================
# Optional: Full init + tetra_sysinfo
# =============================================================================

if $DO_INIT; then
    step "Running full_init + tetra_d daemon"

    # Operativer RF-Pfad (TETRA Basestation 70 cm Amateur):
    #   RX = 428.250 MHz  (UL band — wir empfangen MS-Bursts)
    #   TX = 438.250 MHz  (DL band — wir senden zum MS)
    #   TX_ATT = -10 dB
    bash "${SCRIPT_DIR}/tetra_ctrl.sh" full_init 428250000 438250000

    # VCXO trim — ohne diesen DAC-Wert driftet der Board-Takt; Wert 153
    # wurde als sweet-spot kalibriert (siehe scripts/vcxo_cal.sh).
    bash "${SCRIPT_DIR}/vcxo_cal.sh" --host 192.168.2.180 --dac 153

    # rf_loopback re-issued damit AGC + TX_ATT=-10 dB sauber gesetzt sind
    # (full_init initialisiert die Kette ohne TX_ATT-Override).
    bash "${SCRIPT_DIR}/tetra_ctrl.sh" rf_loopback 428250000 438250000 13 -10

    # Subscriber-DB lives in /var/lib/tetra/db.json (Decision #8). Daemon
    # creates and seeds it on first run with Profile-0 = 0x0000_088F.
    ssh_cmd "mkdir -p /var/lib/tetra"

    ssh_cmd "setsid /root/tetra_d < /dev/null > /tmp/tetra_d.log 2>&1 &"
    echo "tetra_d started → /tmp/tetra_d.log"
fi

# =============================================================================
# Done
# =============================================================================

echo ""
echo "================================================"
echo " DEPLOY COMPLETE"
echo "================================================"
echo " Bitstream : ${BITSTREAM_NAME}.bit.bin (verified)"
if $DO_SW; then
echo " SW        : tetra_d + 12 CGIs + index.html"
fi
echo ""
if ! $DO_INIT; then
echo " Next steps:"
echo "   $0 --init           # FPGA double-load + AD9361 + tetra_d"
echo "   ./scripts/tetra_ctrl.sh monitor   # poll status"
fi
echo "================================================"
