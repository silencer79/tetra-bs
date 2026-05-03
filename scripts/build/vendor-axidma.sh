#!/usr/bin/env bash
# scripts/build/vendor-axidma.sh — fetch + pin Jacob-Feder xilinx_axidma.
#
# Owned by S1 (S1-sw-dma-glue). Runs idempotently:
#   - if sw/external/xilinx_axidma/.upstream/ is missing, clone it.
#   - resolve PIN_REF -> 40-char commit, hard-checkout, write COMMIT_HASH.
#   - patch sw/external/xilinx_axidma/README.md "Pinned commit:" line.
#   - copy LICENSE up to the parent so it's discoverable.
#
# Bump procedure: edit PIN_REF below, re-run the script, commit the
# README + COMMIT_HASH change in one atomic commit.
#
# Requires: bash, git, curl (only for the optional ls-remote sanity
# check). Network is required on first run; offline re-run after
# clone is fine.

set -euo pipefail

# ---- pin point ------------------------------------------------------------
# Set to a 40-char commit hash, a tag name, or 'main' to track HEAD.
# As of 2026-05-03 the upstream URL `https://github.com/jacobfeder/
# xilinx_axidma` was unreachable from the audit host (HTTP 404 / repo
# not found), so this is left as PLACEHOLDER. Update it once the live
# upstream is located (see README.md for fallback options).
UPSTREAM_URL="${TETRA_AXIDMA_UPSTREAM_URL:-https://github.com/jacobfeder/xilinx_axidma}"
PIN_REF="${TETRA_AXIDMA_PIN_REF:-PLACEHOLDER}"
KEEP_HISTORY="${TETRA_AXIDMA_KEEP_HISTORY:-0}"   # 1 → keep .git, 0 → strip

# ---- locations ------------------------------------------------------------
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
VENDOR_DIR="$REPO_ROOT/sw/external/xilinx_axidma"
UPSTREAM_DIR="$VENDOR_DIR/.upstream"
README_FILE="$VENDOR_DIR/README.md"
COMMIT_FILE="$VENDOR_DIR/COMMIT_HASH"

mkdir -p "$VENDOR_DIR"

log() { printf '[vendor-axidma] %s\n' "$*"; }
die() { printf '[vendor-axidma] FAIL: %s\n' "$*" >&2; exit 1; }

# ---- pre-flight -----------------------------------------------------------
if [[ "$PIN_REF" == "PLACEHOLDER" ]]; then
    cat <<EOF >&2
[vendor-axidma] PIN_REF is PLACEHOLDER — refusing to vendor.

Edit this script's PIN_REF= variable (or pass TETRA_AXIDMA_PIN_REF=...
in the environment) to a real upstream commit hash or tag.

To discover candidate refs:
    git ls-remote $UPSTREAM_URL

If the upstream URL above is dead, see sw/external/xilinx_axidma/
README.md "Source" section for fallback locations.
EOF
    exit 2
fi

command -v git >/dev/null 2>&1 || die "git not on PATH"

# ---- clone (or update) ----------------------------------------------------
if [[ ! -d "$UPSTREAM_DIR/.git" ]]; then
    log "cloning $UPSTREAM_URL → $UPSTREAM_DIR"
    rm -rf "$UPSTREAM_DIR"
    git clone --no-tags "$UPSTREAM_URL" "$UPSTREAM_DIR"
else
    log "updating existing clone in $UPSTREAM_DIR"
    git -C "$UPSTREAM_DIR" fetch --no-tags origin
fi

# ---- pin ------------------------------------------------------------------
log "checking out pin ref: $PIN_REF"
git -C "$UPSTREAM_DIR" checkout --detach "$PIN_REF"
RESOLVED="$(git -C "$UPSTREAM_DIR" rev-parse HEAD)"
log "resolved to commit: $RESOLVED"
printf '%s\n' "$RESOLVED" > "$COMMIT_FILE"

# ---- patch README ---------------------------------------------------------
if [[ -f "$README_FILE" ]]; then
    # GNU sed in-place; replace placeholder OR existing pinned-commit line.
    # Idempotent: each run produces exactly one "Pinned commit: <hash>" line.
    if grep -q '^- \*\*Pinned commit:\*\*' "$README_FILE"; then
        sed -i -E "s|^- \*\*Pinned commit:\*\*.*|- **Pinned commit:** \`$RESOLVED\`|" "$README_FILE"
        log "patched $README_FILE 'Pinned commit:' line"
    else
        log "WARN: no 'Pinned commit:' line in $README_FILE — skipping patch"
    fi
fi

# ---- copy LICENSE up to vendor root for discoverability -------------------
if [[ -f "$UPSTREAM_DIR/LICENSE" ]]; then
    cp "$UPSTREAM_DIR/LICENSE" "$VENDOR_DIR/LICENSE"
    log "copied upstream LICENSE → $VENDOR_DIR/LICENSE"
fi

# ---- materialise top-level dirs (driver/, library/) for build steps -------
for sub in driver library examples scripts; do
    if [[ -d "$UPSTREAM_DIR/$sub" ]]; then
        rm -rf "$VENDOR_DIR/$sub"
        cp -R "$UPSTREAM_DIR/$sub" "$VENDOR_DIR/$sub"
    fi
done

# ---- optionally strip .git --------------------------------------------------
if [[ "$KEEP_HISTORY" != "1" ]]; then
    rm -rf "$UPSTREAM_DIR/.git"
fi

log "OK — $VENDOR_DIR pinned at $RESOLVED"
log "Next: re-run 'make sw-test' (host, mock backend) and on the board"
log "      'make -C $VENDOR_DIR/driver modules ...' (see README.md)"
