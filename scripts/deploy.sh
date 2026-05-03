#!/bin/bash
# =============================================================================
# deploy.sh — Build, Convert, Upload (tetra-bs)
#
# Pipeline from source to running daemon on Board #1 (LibreSDR @
# 192.168.2.180):
#   1. Vivado synth + impl + bitstream  → build/vivado/tetra_bs.bit
#   2. bootgen .bit → .bit.bin           → /lib/firmware/ on board
#   3. Cross-compile tetra_d + CGIs (ARM hard-float, gcc 13.3 system)
#   4. SCP upload bitstream + binaries + WebUI
#   5. (optional --start) restart tetra_d on the board
#
# Usage:
#   ./scripts/deploy.sh              full pipeline
#   ./scripts/deploy.sh --no-build   skip Vivado, use existing .bit
#   ./scripts/deploy.sh --no-sw      skip SW cross-compile + upload
#   ./scripts/deploy.sh --build-only Vivado only
#   ./scripts/deploy.sh --start      also (re)start tetra_d on the board
#
# Prereqs (per docs/HARDWARE.md):
#   - Vivado 2022.2 (auto-detected; settings64.sh sourced if needed)
#   - /usr/bin/arm-linux-gnueabihf-gcc (Ubuntu 13.3, NOT Vitis 11.2)
#   - sshpass, scp, ssh
#   - Board #1 reachable: root@192.168.2.180  pw=openwifi
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${REPO_ROOT}/build"
VIVADO_BUILD="${BUILD_DIR}/vivado"
ARM_BUILD="${BUILD_DIR}/arm"

BOARD_IP="${TETRA_BOARD_IP:-192.168.2.180}"
BOARD_USER="${TETRA_BOARD_USER:-root}"
BOARD_PASS="${TETRA_BOARD_PASS:-openwifi}"

BITSTREAM_NAME="tetra_bs"
BIT_FILE="${VIVADO_BUILD}/${BITSTREAM_NAME}.bit"
BIN_FILE="${VIVADO_BUILD}/${BITSTREAM_NAME}.bit.bin"

DAEMON_BIN="${ARM_BUILD}/tetra_d"
CGI_DIR="${ARM_BUILD}/cgi-bin"
WEBUI_HTML="${REPO_ROOT}/sw/webui/index.html"

REMOTE_FW_DIR="/lib/firmware"
REMOTE_BIN_DIR="/root"
REMOTE_WEB_DIR="/www"
REMOTE_CGI_DIR="/www/cgi-bin"
REMOTE_DAEMON="${REMOTE_BIN_DIR}/tetra_d"

DO_BUILD=true
DO_SW=true
DO_START=false
BUILD_ONLY=false

for arg in "$@"; do
    case "$arg" in
        --no-build)   DO_BUILD=false ;;
        --no-sw)      DO_SW=false ;;
        --build-only) BUILD_ONLY=true ;;
        --start)      DO_START=true ;;
        -h|--help)
            sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

ssh_cmd()  { sshpass -p "$BOARD_PASS" ssh -o StrictHostKeyChecking=no "$BOARD_USER@$BOARD_IP" "$@"; }
scp_to()   { sshpass -p "$BOARD_PASS" scp -o StrictHostKeyChecking=no "$1" "$BOARD_USER@$BOARD_IP:$2"; }
step()     { echo; echo "=== $1 ==="; }
fail()     { echo "ERROR: $1" >&2; exit 1; }

# ----- 1/4  Vivado synth + impl + bitstream ---------------------------------
if $DO_BUILD; then
    step "1/4: Vivado synth"
    if ! command -v vivado &>/dev/null; then
        if [ -f /opt/Xilinx/Vivado/2022.2/settings64.sh ]; then
            # shellcheck disable=SC1091
            source /opt/Xilinx/Vivado/2022.2/settings64.sh
        else
            fail "vivado not found; source /opt/Xilinx/Vivado/2022.2/settings64.sh"
        fi
    fi
    rm -f "$BIT_FILE" "$BIN_FILE"
    (cd "$REPO_ROOT" && make synth) || fail "vivado synth failed (see ${BUILD_DIR}/synth.log)"
    [ -f "$BIT_FILE" ] || fail "no bitstream at $BIT_FILE"
    [ -f "$BIN_FILE" ] || fail "no .bit.bin at $BIN_FILE"
    echo "bitstream: $BIT_FILE ($(stat -c %s "$BIT_FILE") bytes)"
    echo "bit.bin  : $BIN_FILE ($(stat -c %s "$BIN_FILE") bytes)"
else
    step "1/4: Vivado synth [SKIPPED]"
    [ -f "$BIN_FILE" ] || fail "no $BIN_FILE; remove --no-build"
fi

if $BUILD_ONLY; then
    echo; echo "build-only requested — exiting before SW + upload."
    exit 0
fi

# ----- 2/4  Cross-compile SW (daemon + CGIs) --------------------------------
if $DO_SW; then
    step "2/4: SW cross-compile (ARM hard-float, gcc 13.3)"
    (cd "$REPO_ROOT" && make sw-build) || fail "make sw-build failed"
    [ -x "$DAEMON_BIN" ] || fail "tetra_d not produced at $DAEMON_BIN"
    echo "daemon : $DAEMON_BIN ($(stat -c %s "$DAEMON_BIN") bytes)"
    if [ -d "$CGI_DIR" ]; then
        for f in "$CGI_DIR"/*.cgi; do
            [ -f "$f" ] || continue
            echo "cgi    : $(basename "$f") ($(stat -c %s "$f") bytes)"
        done
    fi
else
    step "2/4: SW cross-compile [SKIPPED]"
fi

# ----- 3/4  Upload to board -------------------------------------------------
step "3/4: upload to ${BOARD_IP}"

ssh_cmd "echo OK" >/dev/null 2>&1 || fail "board not reachable at ${BOARD_IP}"
echo "board reachable"

# Stop any running daemon before swapping the binary.
ssh_cmd "killall -q tetra_d 2>/dev/null; rm -f /var/run/tetra_d.sock; true"

# Bitstream → /lib/firmware (consumed by fpga_manager).
echo "uploading bitstream..."
scp_to "$BIN_FILE" "${REMOTE_FW_DIR}/${BITSTREAM_NAME}.bit.bin"
LOCAL_MD5=$(md5sum "$BIN_FILE" | cut -d' ' -f1)
REMOTE_MD5=$(ssh_cmd "md5sum ${REMOTE_FW_DIR}/${BITSTREAM_NAME}.bit.bin" | cut -d' ' -f1)
[ "$LOCAL_MD5" = "$REMOTE_MD5" ] || fail "MD5 mismatch  local=$LOCAL_MD5 remote=$REMOTE_MD5"
echo "bitstream verified (md5 $LOCAL_MD5)"

if $DO_SW; then
    echo "uploading tetra_d..."
    scp_to "$DAEMON_BIN" "${REMOTE_DAEMON}"
    ssh_cmd "chmod +x ${REMOTE_DAEMON}"

    if [ -d "$CGI_DIR" ]; then
        echo "uploading CGI binaries..."
        ssh_cmd "mkdir -p ${REMOTE_CGI_DIR}"
        for f in "$CGI_DIR"/*.cgi; do
            [ -f "$f" ] || continue
            scp_to "$f" "${REMOTE_CGI_DIR}/$(basename "$f")"
        done
        ssh_cmd "chmod +x ${REMOTE_CGI_DIR}/*.cgi"
    fi

    if [ -f "$WEBUI_HTML" ]; then
        echo "uploading WebUI index.html..."
        scp_to "$WEBUI_HTML" "${REMOTE_WEB_DIR}/index.html"
    fi
fi

# ----- 4/4  Optional: start daemon -----------------------------------------
if $DO_START; then
    step "4/4: starting tetra_d on board"
    ssh_cmd "mkdir -p /var/lib/tetra"
    # Daemon manages its own DB init from db.json on first run.
    ssh_cmd "setsid ${REMOTE_DAEMON} < /dev/null > /tmp/tetra_d.log 2>&1 &"
    sleep 1
    ssh_cmd "pidof tetra_d >/dev/null && echo 'tetra_d running' || echo 'tetra_d FAILED to start; see /tmp/tetra_d.log'"
else
    step "4/4: start [SKIPPED]"
    echo "to start: ssh root@${BOARD_IP} 'setsid ${REMOTE_DAEMON} </dev/null >/tmp/tetra_d.log 2>&1 &'"
fi

echo
echo "================================================"
echo " DEPLOY COMPLETE"
echo "================================================"
echo " bitstream : ${BITSTREAM_NAME}.bit.bin (md5 verified)"
$DO_SW && echo " daemon    : ${REMOTE_DAEMON}"
$DO_SW && [ -d "$CGI_DIR" ] && echo " webui     : ${REMOTE_WEB_DIR}/index.html + ${REMOTE_CGI_DIR}/*.cgi"
$DO_START && echo " status    : tetra_d started on board"
echo "================================================"
