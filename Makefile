# Makefile — top-level build for tetra-bs.
#
# Owned by T0 build-skeleton. This is the green-CI entry point that all
# Phase-2 agents (A1..A6, S0..S7, T1..T3) land their work into.
#
# Targets:
#   make tb        — run all RTL TBs under tb/rtl/<block>/ via iverilog -g2001.
#   make sw-test   — build + run all C unit-tests under tb/sw/<block>/ against
#                    /usr/bin/gcc (Ubuntu 13.3), linking vendored Unity.
#   make sw-build  — cross-compile sw/tetra_d.c + friends for ARM hard-float
#                    using /usr/bin/arm-linux-gnueabihf-gcc (Ubuntu 13.3, NOT
#                    Vitis 11.2 — see HARDWARE.md §2 PATH-precedence trap).
#                    Output → build/arm/.
#   make cosim     — Verilator-based cosim under tb/cosim/. T2 fills this in.
#   make synth     — Vivado 2022.2 batch synth of rtl/tetra_top.v. A5 fills in.
#   make clean     — wipe build/, **/*.vvp, **/*.o.
#   make help      — list targets with one-line descriptions.
#
# CI: `.github/workflows/ci.yml` runs `make tb && make sw-test && make sw-build`
# on ubuntu-24.04. `make synth` and `make cosim` are NOT in CI (Vivado not
# installed; cosim is T2's deliverable).

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD_DIR := $(REPO_ROOT)/build
BUILD_ARM := $(BUILD_DIR)/arm

# ---- Toolchain pins (from HARDWARE.md §2 / §5 / §6) ----------------------
# Host gcc — used by sw-test for unit-test binaries.
HOST_CC := /usr/bin/gcc

# ARM cross-compiler — explicit /usr/bin path defeats the Vitis PATH trap.
# HARDWARE.md §2 documents that `settings64.sh` prepends Vitis 11.2's same-name
# wrapper; we must not rely on PATH order.
ARM_CC := /usr/bin/arm-linux-gnueabihf-gcc

# Cortex-A9 (Zynq-7020) hard-float, NEON+VFPv3 per HARDWARE.md §2.
ARM_CFLAGS := -std=c11 -O2 -g \
              -march=armv7-a -mfpu=neon-vfpv3 -mfloat-abi=hard \
              -Wall -Wextra -Werror

IVERILOG := /usr/bin/iverilog

# ---- Tree discovery -------------------------------------------------------
# Per-block TB Makefiles live one level below tb/rtl/ and tb/sw/ respectively.
# Glob picks them up automatically — Phase-2 agents only have to drop a new
# block dir with its own Makefile and it joins the run.
RTL_TB_DIRS := $(sort $(dir $(wildcard $(REPO_ROOT)/tb/rtl/*/Makefile)))
SW_TB_DIRS  := $(sort $(dir $(wildcard $(REPO_ROOT)/tb/sw/*/Makefile)))

# Smoke ARM source for sw-build until S7 replaces it with sw/tetra_d.c.
ARM_SMOKE_SRC := $(REPO_ROOT)/sw/tetra_d_smoke.c
ARM_SMOKE_BIN := $(BUILD_ARM)/tetra_d_smoke

.PHONY: help tb sw-test sw-build cosim synth clean \
        _check_rtl_dirs _check_sw_dirs

# ---- help -----------------------------------------------------------------
help:
	@echo "tetra-bs build targets:"
	@echo "  make tb        Run all RTL TBs (iverilog 12.0)."
	@echo "  make sw-test   Build + run all C host unit-tests (gcc 13.3, Unity)."
	@echo "  make sw-build  Cross-compile SW for ARM hard-float (gcc 13.3)."
	@echo "  make cosim     Verilator cosim (T2 stub — not yet implemented)."
	@echo "  make synth     Vivado 2022.2 synth of rtl/tetra_top.v (A5 stub)."
	@echo "  make clean     Remove build/ and stray .vvp/.o files."
	@echo "  make help      Show this list."

# ---- tb (RTL) -------------------------------------------------------------
tb: _check_rtl_dirs
	@echo "[tb] running RTL TBs in:"
	@for d in $(RTL_TB_DIRS); do echo "      $$d"; done
	@fail=0; for d in $(RTL_TB_DIRS); do \
	    echo "----------------------------------------"; \
	    echo "[tb] $$d"; \
	    $(MAKE) -C $$d all || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then \
	    echo "[tb] FAIL — at least one RTL TB failed."; exit 1; \
	fi; \
	echo "[tb] all RTL TBs PASS."

_check_rtl_dirs:
	@if [ -z "$(RTL_TB_DIRS)" ]; then \
	    echo "[tb] FAIL — no per-block Makefiles found under tb/rtl/."; exit 1; \
	fi

# ---- sw-test (host) -------------------------------------------------------
sw-test: _check_sw_dirs
	@echo "[sw-test] running SW host-tests in:"
	@for d in $(SW_TB_DIRS); do echo "          $$d"; done
	@fail=0; for d in $(SW_TB_DIRS); do \
	    echo "----------------------------------------"; \
	    echo "[sw-test] $$d"; \
	    $(MAKE) -C $$d CC=$(HOST_CC) all || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then \
	    echo "[sw-test] FAIL — at least one host-test failed."; exit 1; \
	fi; \
	echo "[sw-test] all SW host-tests PASS."

_check_sw_dirs:
	@if [ -z "$(SW_TB_DIRS)" ]; then \
	    echo "[sw-test] FAIL — no per-block Makefiles found under tb/sw/."; \
	    exit 1; \
	fi

# ---- sw-build (ARM cross) -------------------------------------------------
# Until S7 lands, the canonical sw-build target is the smoke binary so CI
# proves the cross-compile path works. Once sw/tetra_d.c lands, swap
# ARM_SMOKE_SRC -> the real source set and add link rules here.
sw-build: $(ARM_SMOKE_BIN)
	@echo "[sw-build] artefact: $(ARM_SMOKE_BIN)"
	@file $(ARM_SMOKE_BIN) || true

$(BUILD_ARM):
	@mkdir -p $@

$(ARM_SMOKE_BIN): $(ARM_SMOKE_SRC) | $(BUILD_ARM)
	@echo "[sw-build] $(ARM_CC) $(ARM_SMOKE_SRC) -> $@"
	$(ARM_CC) $(ARM_CFLAGS) -o $@ $(ARM_SMOKE_SRC)

# ---- cosim (T2 stub) ------------------------------------------------------
cosim:
	@echo "[cosim] T2-cosim not yet implemented"
	@exit 0

# ---- synth (A5 stub) ------------------------------------------------------
synth:
	@echo "[synth] A5 will fill this in"
	@exit 0

# ---- clean ----------------------------------------------------------------
clean:
	@echo "[clean] removing build artefacts"
	@rm -rf $(BUILD_DIR)
	@find $(REPO_ROOT) -name '*.vvp' -type f -delete 2>/dev/null || true
	@find $(REPO_ROOT) -name '*.o' -type f -delete 2>/dev/null || true
	@for d in $(RTL_TB_DIRS) $(SW_TB_DIRS); do \
	    $(MAKE) -C $$d clean >/dev/null 2>&1 || true; \
	done
