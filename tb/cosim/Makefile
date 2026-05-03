# tb/cosim/Makefile — Verilator + C-binary co-sim driver (T2).
#
# Owned by Agent T2 (T2-cosim-verilator). Top-level entry point is
# `make cosim SCENARIO=<name>` (run from this directory or via the
# repo-root Makefile that forwards through to here).
#
# Two modes, auto-selected by detecting `verilator` in $PATH:
#
#   FULL MODE      verilator present → elaborate Vtetra_top, link
#                  verilator_top.cpp + shm_dma_bridge.c, run the
#                  selected scenario, bit-diff the captured DL
#                  output against scenarios/expected_dl/<scenario>.bin.
#
#   FALLBACK MODE  verilator missing → build only the shm-bridge
#                  self-test, run it (proves the bridge code is
#                  green), print the FALLBACK banner per
#                  Decision-Table §D #3, and exit 0 (CI accepts).
#
# Scenarios:
#   m2_attach          0/432 bit diff target.
#   group_attach       0/124 bit diff target.
#   d_nwrk_broadcast   byte-identical to Burst #423 (24-byte expected).
#
# All three accept fallback exit-0 with the banner.

SHELL       := /bin/bash
THIS_DIR    := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT   := $(abspath $(THIS_DIR)/../..)
RTL_DIR     := $(REPO_ROOT)/rtl
RTL_TOP     := $(RTL_DIR)/tetra_top.v
SCENARIOS   := $(THIS_DIR)/scenarios
BUILD_DIR   := $(THIS_DIR)/build

VERILATOR   := $(shell command -v verilator 2>/dev/null)
HOST_CC     := /usr/bin/gcc
HOST_CXX    := /usr/bin/g++

# Strict flags per CLAUDE.md / T2 contract.
CSTD     := -std=c11
CXXSTD   := -std=c++17
WARN     := -Wall -Wextra -Werror
CFLAGS   := $(CSTD) $(WARN) -O2 -g -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
CXXFLAGS := $(CXXSTD) $(WARN) -O2 -g
INCLUDE  := -I$(THIS_DIR)/include

SCENARIO ?= m2_attach
ALL_SCENARIOS := m2_attach group_attach d_nwrk_broadcast

# Allow overriding from CLI: `make cosim SCENARIO=group_attach`.
ifeq ($(filter $(SCENARIO),$(ALL_SCENARIOS)),)
$(error unknown SCENARIO=$(SCENARIO); valid: $(ALL_SCENARIOS))
endif

STIM_BIN     := $(SCENARIOS)/$(SCENARIO).bin
EXPECTED_DL  := $(SCENARIOS)/expected_dl/$(SCENARIO).bin
CAPTURE_OUT  := $(BUILD_DIR)/$(SCENARIO).dl.bin
DIFF_REPORT  := $(BUILD_DIR)/$(SCENARIO).diff.txt

# Bit-budget per scenario for the diff report. Locked from the §T2 test
# gate: m2_attach=432 bits, group_attach=124 bits, d_nwrk_broadcast=124
# bits. The build/<scenario>.diff.txt header echoes this so a CI run is
# self-describing.
BIT_BUDGET_m2_attach        := 432
BIT_BUDGET_group_attach     := 124
BIT_BUDGET_d_nwrk_broadcast := 124
BIT_BUDGET := $(BIT_BUDGET_$(SCENARIO))

.PHONY: cosim cosim-all clean fixtures shm-selftest help

help:
	@echo "tb/cosim/ targets:"
	@echo "  make cosim SCENARIO=<name>   Run one scenario (m2_attach|group_attach|d_nwrk_broadcast)."
	@echo "  make cosim-all               Run all three scenarios."
	@echo "  make fixtures                Rebuild the .bin fixtures from gold-ref captures."
	@echo "  make shm-selftest            Build + run the shm-bridge selftest only."
	@echo "  make clean                   Remove build/ artefacts."

# ---- fixtures (regen scenarios/*.bin) -------------------------------------
fixtures:
	@python3 $(SCENARIOS)/build_fixtures.py

# ---- shm-bridge selftest --------------------------------------------------
SHM_SELFTEST_BIN := $(BUILD_DIR)/shm_selftest

$(SHM_SELFTEST_BIN): $(THIS_DIR)/shm_dma_bridge.c $(THIS_DIR)/include/cosim_shm.h | $(BUILD_DIR)
	@echo "[cosim] building shm-bridge selftest -> $@"
	$(HOST_CC) $(CFLAGS) $(INCLUDE) -DSHM_BRIDGE_SELFTEST_MAIN \
	    -o $@ $(THIS_DIR)/shm_dma_bridge.c -lrt -lpthread

shm-selftest: $(SHM_SELFTEST_BIN)
	@$(SHM_SELFTEST_BIN)

# ---- main cosim entry point ----------------------------------------------
$(BUILD_DIR):
	@mkdir -p $@

cosim: $(BUILD_DIR)
	@echo "[cosim] scenario=$(SCENARIO) bit_budget=$(BIT_BUDGET) verilator='$(VERILATOR)'"
ifeq ($(VERILATOR),)
	@$(MAKE) --no-print-directory _run_fallback
else
	@$(MAKE) --no-print-directory _run_full
endif

cosim-all:
	@for s in $(ALL_SCENARIOS); do \
	    $(MAKE) --no-print-directory cosim SCENARIO=$$s || exit $$?; \
	done

# ---- FALLBACK MODE -------------------------------------------------------
.PHONY: _run_fallback
_run_fallback: $(SHM_SELFTEST_BIN)
	@echo "[cosim] FALLBACK MODE - shm-bridge deferred to Phase-4 live A/B"
	@echo "[cosim]   (verilator not on PATH; install via 'sudo apt install -y verilator')"
	@echo "[cosim]   running shm-bridge selftest as a smoke check ..."
	@$(SHM_SELFTEST_BIN) || (echo "[cosim] FAIL - shm-bridge selftest broken"; exit 1)
	@echo "[cosim]   shm-bridge selftest PASS"
	@echo "[cosim]   scenario=$(SCENARIO) DL bit-diff: deferred (no verilator)"
	@printf '[cosim] FALLBACK MODE - scenario=%s budget=%s bits\n' \
	    '$(SCENARIO)' '$(BIT_BUDGET)' > $(DIFF_REPORT)
	@printf '[cosim] verilator not installed; capture not produced.\n' \
	    >> $(DIFF_REPORT)
	@printf '[cosim] re-enable: apt install verilator + see tb/cosim/README.md\n' \
	    >> $(DIFF_REPORT)
	@cat $(DIFF_REPORT)
	@exit 0

# ---- FULL MODE -----------------------------------------------------------
# When verilator is installed: elaborate the DUT, build the harness,
# run it under a 60 s wall-clock timeout (per §T2 test gate), and
# diff the captured DL bytes against the expected fixture.

VLT_OBJ_DIR := $(BUILD_DIR)/obj_dir
VLT_TOP_BIN := $(VLT_OBJ_DIR)/Vtetra_top
COSIM_BIN   := $(BUILD_DIR)/verilator_top

# Verilator generates a Makefile under $(VLT_OBJ_DIR) that we forward
# to. We hand verilator the SystemC-style top + the C++ harness +
# the C bridge so they all link into one binary.
.PHONY: _vlt_elaborate _run_full

_vlt_elaborate:
	@echo "[cosim] elaborating $(RTL_TOP) under verilator"
	@mkdir -p $(VLT_OBJ_DIR)
	@$(VERILATOR) --cc --exe --build -O2 \
	    -DTETRA_TOP_NO_PHY \
	    --top-module tetra_top \
	    --Mdir $(VLT_OBJ_DIR) \
	    -CFLAGS "$(CFLAGS) $(INCLUDE)" \
	    -LDFLAGS "-lrt -lpthread" \
	    -Wno-fatal -Wno-WIDTH -Wno-UNUSED -Wno-UNDRIVEN \
	    $(RTL_TOP) \
	    $(THIS_DIR)/verilator_top.cpp \
	    $(THIS_DIR)/shm_dma_bridge.c \
	    -CFLAGS "-I$(THIS_DIR)/include" \
	    -o Vtetra_top
	@echo "[cosim] verilator build done -> $(VLT_TOP_BIN)"

# Run the elaborated binary against the scenario fixture. 60 s wall-clock
# cap per §T2 test gate.
_run_full: _vlt_elaborate
	@echo "[cosim] running $(VLT_TOP_BIN) scenario=$(SCENARIO)"
	@timeout --foreground 60s $(VLT_TOP_BIN) \
	    $(SCENARIO) $(STIM_BIN) $(EXPECTED_DL) $(CAPTURE_OUT) \
	    || (echo "[cosim] FAIL - verilator binary died or hit 60s timeout"; exit 1)
	@$(MAKE) --no-print-directory _diff

# Bit-level diff. Emits a non-zero count if the capture deviates from
# the expected DL fixture by >0 bits within the budget. Implementation
# uses cmp -l to spot byte-level differences then a python helper to
# fold the byte diff into a bit count. Keeps the report tooling
# self-contained (no perl/awk dependence beyond what HARDWARE.md §6
# already requires).
_diff:
	@echo "[cosim] diffing $(CAPTURE_OUT) vs $(EXPECTED_DL) (budget=$(BIT_BUDGET) bits)"
	@if [ ! -s $(CAPTURE_OUT) ]; then \
	    echo "[cosim] FAIL - capture is empty"; exit 1; \
	fi
	@python3 -c "import sys; \
got = open(sys.argv[1],'rb').read(); \
exp = open(sys.argv[2],'rb').read(); \
n = min(len(got), len(exp)); \
diff = sum(bin(got[i]^exp[i]).count('1') for i in range(n)) \
       + 8 * abs(len(got) - len(exp)); \
budget = int(sys.argv[3]); \
print(f'[cosim] bit-diff = {diff}/{budget}'); \
sys.exit(0 if diff == 0 else 2)" \
	    $(CAPTURE_OUT) $(EXPECTED_DL) $(BIT_BUDGET) \
	    || (echo "[cosim] FAIL - DL bit-diff > 0"; exit 1)
	@echo "[cosim] PASS scenario=$(SCENARIO)"

clean:
	@rm -rf $(BUILD_DIR)
