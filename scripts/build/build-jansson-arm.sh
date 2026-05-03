#!/usr/bin/env bash
# scripts/build/build-jansson-arm.sh — idempotent static jansson cross-build.
#
# Output: $REPO_ROOT/build/jansson-arm/install/{lib/libjansson.a,include/jansson.h}
#
# Used by Makefile sw-build target to satisfy ARM_JANSSON_LIBS without
# requiring multiarch on the dev host or a libjansson4.deb on Board #1
# (HARDWARE.md §10 "Lean: static, avoids board pkg-mgmt").
#
# Re-run: idempotent. If install/lib/libjansson.a already present and
# newer than the source tree, no-op.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/jansson-arm"
INSTALL_DIR="$BUILD_DIR/install"
ARCHIVE="$INSTALL_DIR/lib/libjansson.a"
JANSSON_VERSION="${JANSSON_VERSION:-2.14}"
JANSSON_TGZ="$BUILD_DIR/jansson-v${JANSSON_VERSION}.tar.gz"
JANSSON_SRC="$BUILD_DIR/jansson-${JANSSON_VERSION}"
JANSSON_URL="https://github.com/akheron/jansson/archive/refs/tags/v${JANSSON_VERSION}.tar.gz"

CC=/usr/bin/arm-linux-gnueabihf-gcc
CFLAGS="-O2 -march=armv7-a -mfpu=neon-vfpv3 -mfloat-abi=hard"

if [[ -f "$ARCHIVE" ]]; then
    echo "[build-jansson-arm] $ARCHIVE present, skipping rebuild."
    exit 0
fi

echo "[build-jansson-arm] building libjansson.a v$JANSSON_VERSION (ARM static)"
mkdir -p "$BUILD_DIR"

if [[ ! -f "$JANSSON_TGZ" ]]; then
    echo "[build-jansson-arm] downloading $JANSSON_URL"
    curl -fsSL "$JANSSON_URL" -o "$JANSSON_TGZ"
fi

if [[ ! -d "$JANSSON_SRC" ]]; then
    tar -xzf "$JANSSON_TGZ" -C "$BUILD_DIR"
fi

cd "$JANSSON_SRC"
if [[ ! -f configure ]]; then
    autoreconf -fi >/dev/null
fi
./configure --host=arm-linux-gnueabihf \
            --enable-static --disable-shared \
            --prefix="$INSTALL_DIR" \
            CC="$CC" CFLAGS="$CFLAGS" >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null

echo "[build-jansson-arm] OK -> $ARCHIVE"
