# =============================================================================
# tb/rtl/Makefile.inc — shared iverilog test-bench rules
# =============================================================================
#
# Owned by Agent T0 (tb-rtl-infra) per docs/MIGRATION_PLAN.md.  A6 prepublishes
# this skeleton so its UMAC carry-over TBs can include it from day 0.  T0 will
# extend / replace as needed.
#
# Per-TB Makefiles set:
#   TB_NAME    — name of the TB top module (e.g. tb_ul_demand_reassembly)
#   TB_SRCS    — list of .v files in dependency order; first must contain the
#                tb_<name> top
#   REPO_ROOT  — relative path back to the repo root (default ../../..)
#
# and then `include $(REPO_ROOT)/tb/rtl/Makefile.inc`.
#
# Targets:
#   make          — alias for `run`
#   run           — compile + simulate; non-zero exit on failure
#   compile       — iverilog only, drops .vvp into BUILD_DIR
#   clean         — rm BUILD_DIR
# =============================================================================

REPO_ROOT ?= ../../..
BUILD_DIR ?= build
IVERILOG  ?= iverilog
VVP       ?= vvp

# Verilog-2001 only — CLAUDE.md §Languages.
IVERILOG_FLAGS ?= -g2001 -Wall

VVP_FILE := $(BUILD_DIR)/$(TB_NAME).vvp

.PHONY: all run compile clean
all: run

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

compile: $(VVP_FILE)

$(VVP_FILE): $(TB_SRCS) | $(BUILD_DIR)
	$(IVERILOG) $(IVERILOG_FLAGS) -o $@ -s $(TB_NAME) $(TB_SRCS)

run: $(VVP_FILE)
	@$(VVP) $(VVP_FILE)

clean:
	rm -rf $(BUILD_DIR)
