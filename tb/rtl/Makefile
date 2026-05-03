# =============================================================================
# tb/rtl/Makefile — RTL-TB aggregator/driver.
# =============================================================================
# T0 seeded a per-block-Makefile pattern (see tb/rtl/Makefile.inc). T1 owns
# this driver. All per-block TBs are auto-discovered via */Makefile glob —
# adding a new TB only requires dropping a new tb_<name>/ directory with its
# own Makefile that includes Makefile.inc.
#
# Targets:
#   make            — alias for `make tb`.
#   make tb         — run every discovered TB; aggregate PASS/FAIL.
#   make tb VCD=1   — run every TB with waveform dumping enabled. Each TB
#                     emits build/<tb_name>.vcd. Off by default to keep CI fast.
#   make tb-list    — list discovered TBs with their last-run state
#                     (PASS / FAIL / NEW), parsed from build/<tb>.log.
#   make tb-stats   — print assertion/sub-test counts per TB and totals
#                     (proxy for coverage; iverilog ships no coverage tool).
#   make clean      — recurse into every TB's `clean`.
#
# Recipes are /bin/sh-portable (no bashisms). VCD propagates as `make … VCD=1`
# argument to per-block Makefiles, where Makefile.inc injects the iverilog
# defines.
# =============================================================================

# Discover per-block TBs by glob: every */Makefile under this directory.
# Sort-stable so output ordering is reproducible.
RTL_TB_DIRS  := $(sort $(patsubst %/,%,$(dir $(wildcard */Makefile))))
RTL_TB_NAMES := $(notdir $(RTL_TB_DIRS))

# When a per-block dir name starts with "_" (like _smoke), the canonical TB
# name is the file inside it (tb_smoke). Map underscore-prefixed dirs to
# their TB_NAME by parsing the per-block Makefile.
# (Cheap proxy: assume tb_<basename> if dir starts with "_smoke", else dir.)
tb_dir_to_name = $(if $(filter _%,$(1)),tb$(1),$(1))

.PHONY: all tb tb-list tb-stats clean help

all: tb
help:
	@echo "tb/rtl driver targets:"
	@echo "  make tb         Run all RTL TBs (default)."
	@echo "  make tb VCD=1   Run all TBs with VCD waveform dumps."
	@echo "  make tb-list    List discovered TBs + last-run PASS/FAIL state."
	@echo "  make tb-stats   Print per-TB assertion counts + totals."
	@echo "  make clean      Recursively clean every TB build/."

# ---- tb -------------------------------------------------------------------
# Iterate every discovered TB; capture failures into a single fail flag and
# exit non-zero at the end if any failed. Pass VCD through unchanged.
tb:
	@if [ -z "$(RTL_TB_DIRS)" ]; then \
	    echo "[tb] FAIL — no per-block Makefiles found under tb/rtl/."; \
	    exit 1; \
	fi
	@echo "[tb] discovered $(words $(RTL_TB_DIRS)) TBs:"
	@for d in $(RTL_TB_DIRS); do echo "      $$d"; done
	@fail=0; for d in $(RTL_TB_DIRS); do \
	    echo "----------------------------------------"; \
	    echo "[tb] $$d (VCD=$(VCD))"; \
	    $(MAKE) -C $$d all VCD=$(VCD) || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then \
	    echo "[tb] FAIL — at least one RTL TB failed."; exit 1; \
	fi; \
	echo "[tb] all $(words $(RTL_TB_DIRS)) RTL TBs PASS."

# ---- tb-list --------------------------------------------------------------
# Print discovered TBs + their last-run state by inspecting per-block logs.
# Per-block Makefiles using rtl_tb_rule write build/<tb>.log. State legend:
#   PASS  — log exists and contains a "^PASS" line
#   FAIL  — log exists but no "^PASS" line
#   NEW   — log does not exist (TB has not been run yet)
tb-list:
	@printf "%-36s %-6s %s\n" "TB" "STATE" "LOG"
	@printf "%-36s %-6s %s\n" "------------------------------------" "------" "----------------------------------------"
	@for d in $(RTL_TB_DIRS); do \
	    name=$$( $(MAKE) --no-print-directory -C $$d _print_tb_name 2>/dev/null \
	             || echo $$d ); \
	    log="$$d/build/$$name.log"; \
	    if [ ! -f "$$log" ]; then \
	        state="NEW"; \
	    elif grep -q '^PASS' "$$log" 2>/dev/null; then \
	        state="PASS"; \
	    else \
	        state="FAIL"; \
	    fi; \
	    printf "%-36s %-6s %s\n" "$$name" "$$state" "$$log"; \
	done

# ---- tb-stats -------------------------------------------------------------
# Count assertion-style markers per TB. Coverage proxy: iverilog has no
# coverage tool, so we count what the TBs themselves report.
#
# Counted patterns:
#   - ^PASS      — explicit pass markers in the log (success sub-tests)
#   - TEST_ASSERT — uppercase assertion macro invocations in the .v sources
#   - $display.*PASS — inline pass-prints in the .v sources
#
# All grep counts use POSIX-portable -c (no GNU-isms).
tb-stats:
	@printf "%-36s %8s %8s %8s\n" "TB" "PASS-LOG" "ASSERTS" "DISP-PASS"
	@printf "%-36s %8s %8s %8s\n" "------------------------------------" "--------" "--------" "--------"
	@total_log=0; total_assert=0; total_disp=0; tb_count=0; \
	for d in $(RTL_TB_DIRS); do \
	    name=$$( $(MAKE) --no-print-directory -C $$d _print_tb_name 2>/dev/null \
	             || echo $$d ); \
	    log="$$d/build/$$name.log"; \
	    if [ -f "$$log" ]; then \
	        n_log=$$(grep -c '^PASS' "$$log" 2>/dev/null || echo 0); \
	    else \
	        n_log=0; \
	    fi; \
	    n_assert=$$(grep -c 'TEST_ASSERT' $$d/*.v /dev/null 2>/dev/null | awk -F: '{s+=$$NF} END {print s+0}'); \
	    n_disp=$$(grep -cE '\$$display.*PASS' $$d/*.v /dev/null 2>/dev/null | awk -F: '{s+=$$NF} END {print s+0}'); \
	    printf "%-36s %8d %8d %8d\n" "$$name" "$$n_log" "$$n_assert" "$$n_disp"; \
	    total_log=$$(( total_log + n_log )); \
	    total_assert=$$(( total_assert + n_assert )); \
	    total_disp=$$(( total_disp + n_disp )); \
	    tb_count=$$(( tb_count + 1 )); \
	done; \
	printf "%-36s %8s %8s %8s\n" "------------------------------------" "--------" "--------" "--------"; \
	printf "%-36s %8d %8d %8d\n" "TOTAL ($$tb_count TBs)" "$$total_log" "$$total_assert" "$$total_disp"

# ---- clean ----------------------------------------------------------------
clean:
	@for d in $(RTL_TB_DIRS); do \
	    echo "[clean] $$d"; \
	    $(MAKE) -C $$d clean >/dev/null 2>&1 || true; \
	done
