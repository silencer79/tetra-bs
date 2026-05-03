# tb/rtl/Makefile.inc — common per-block RTL TB rules.
#
# Owned by T0 build-skeleton. Per-block Makefile pattern:
#
#   include $(REPO_ROOT)/tb/rtl/Makefile.inc
#   TB_NAME := tb_smoke
#   SRCS    := tb_smoke.v
#   $(eval $(call rtl_tb_rule,$(TB_NAME),$(SRCS)))
#
# Variables provided to including Makefiles:
#   IVERILOG     — Icarus-Verilog compiler (HARDWARE.md §6: 12.0 stable)
#   VVP          — Icarus runtime
#   IVERILOG_FLAGS  — common flags (Verilog-2001 strict; CLAUDE.md "Languages")
#   RTL_BUILD    — per-block build dir (defaults to ./build under the per-block dir)
#
# Targets provided (per call to rtl_tb_rule):
#   $(BLOCK_DIR)/build/$(TB_NAME).vvp  — compiled simulation
#   <TB_NAME>      — phony, runs the .vvp and greps for "PASS"; exits non-zero on fail.
#
# Pass/fail contract: a TB MUST print a single line containing the exact word
# "PASS" on success. Any failure (including iverilog/vvp errors, $fatal,
# absence of "PASS") propagates as a non-zero exit from the recipe.

IVERILOG       ?= /usr/bin/iverilog
VVP            ?= /usr/bin/vvp
IVERILOG_FLAGS ?= -g2001 -Wall

# Resolve repo root once (Makefile.inc lives in tb/rtl/, so up two).
RTL_INC_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
REPO_ROOT   ?= $(abspath $(RTL_INC_DIR)/../..)

# Per-block build dir (override-able per Makefile).
RTL_BUILD ?= build

# Macro: rtl_tb_rule(tb_name, srcs)
#   Generates compile + run rules for a single TB.
#   Sources are resolved relative to the calling Makefile's directory.
define rtl_tb_rule
$(RTL_BUILD)/$(1).vvp: $(2) | $(RTL_BUILD)
	$$(IVERILOG) $$(IVERILOG_FLAGS) -o $$@ -s $(1) $(2)

$(1): $(RTL_BUILD)/$(1).vvp
	@echo "[RTL-TB] $(1)"
	@$$(VVP) -n $$< | tee $(RTL_BUILD)/$(1).log
	@grep -q '^PASS' $(RTL_BUILD)/$(1).log || { echo "FAIL: $(1) (no PASS line)"; exit 1; }

.PHONY: $(1)
endef

$(RTL_BUILD):
	@mkdir -p $@
