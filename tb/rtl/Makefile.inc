# tb/rtl/Makefile.inc — common per-block RTL TB rules.
#
# Owned by T0 build-skeleton; extended by T1 (VCD dump support). Per-block
# Makefile pattern:
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
#
# T1 extension — waveform dumps (off by default to keep CI fast):
#   make <tb> VCD=1   — compile with -DVCDDUMP and -DVCD_FILE="<build>/<tb>.vcd",
#                       so the TB's `ifdef VCDDUMP block emits a VCD trace.
#                       The TB sources MUST contain a VCDDUMP-guarded block
#                       calling $dumpfile(`VCD_FILE) and $dumpvars(0, <top>).

IVERILOG       ?= /usr/bin/iverilog
VVP            ?= /usr/bin/vvp
IVERILOG_FLAGS ?= -g2001 -Wall

# Resolve repo root once (Makefile.inc lives in tb/rtl/, so up two).
RTL_INC_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
REPO_ROOT   ?= $(abspath $(RTL_INC_DIR)/../..)

# Per-block build dir (override-able per Makefile).
RTL_BUILD ?= build

# T1: VCD-mode flags. When VCD=1 is set on the make command line,
# inject -DVCDDUMP and -DVCD_FILE="\"<build>/<tb>.vcd\"" into iverilog.
# Off by default — CI must stay fast and not generate gigabytes of waveforms.
# The double-quote escaping is needed so the C preprocessor sees the path
# as a Verilog string literal.
RTL_VCD_DEFS = $(if $(VCD),-DVCDDUMP -DVCD_FILE="\"$(RTL_BUILD)/$(1).vcd\"",)

# Macro: rtl_tb_rule(tb_name, srcs)
#   Generates compile + run rules for a single TB.
#   Sources are resolved relative to the calling Makefile's directory.
define rtl_tb_rule
$(RTL_BUILD)/$(1).vvp: $(2) | $(RTL_BUILD)
	$$(IVERILOG) $$(IVERILOG_FLAGS) $(call RTL_VCD_DEFS,$(1)) -o $$@ -s $(1) $(2)

$(1): $(RTL_BUILD)/$(1).vvp
	@echo "[RTL-TB] $(1)"
	@$$(VVP) -n $$< | tee $(RTL_BUILD)/$(1).log
	@grep -q '^PASS' $(RTL_BUILD)/$(1).log || { echo "FAIL: $(1) (no PASS line)"; exit 1; }

.PHONY: $(1)
endef

$(RTL_BUILD):
	@mkdir -p $@

# T1: introspection helper used by the aggregator (tb/rtl/Makefile) for
# tb-list / tb-stats. Per-block Makefiles include this file AFTER setting
# TB_NAME, so this target resolves correctly. Suppress make's own chatter
# with `--no-print-directory` at the call site.
.PHONY: _print_tb_name
_print_tb_name:
	@echo $(TB_NAME)
