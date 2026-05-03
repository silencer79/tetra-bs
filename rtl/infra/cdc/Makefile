# rtl/infra/cdc/Makefile
#
# Library makefile for the CDC primitive lib. Two main targets:
#
#   make lint   — lint-syntax-check every primitive in isolation with iverilog
#   make tb     — run all four CDC TBs (under ../../../tb/rtl/tb_cdc_*)
#                 and report PASS/FAIL.
#
# This Makefile drives the primitives, not the TB tree. The TB Makefiles are
# self-contained per directory and use absolute references back to this lib.

CDC_SRCS := \
    cdc_sync_2ff.v   \
    cdc_handshake.v  \
    cdc_async_fifo.v \
    cdc_pulse.v

TB_DIRS := \
    ../../../tb/rtl/tb_cdc_sync_2ff   \
    ../../../tb/rtl/tb_cdc_handshake  \
    ../../../tb/rtl/tb_cdc_async_fifo \
    ../../../tb/rtl/tb_cdc_pulse

.PHONY: all lint tb tb-clean
all: lint

lint:
	@for src in $(CDC_SRCS); do \
	    echo "[lint] $$src"; \
	    iverilog -g2001 -t null $$src || exit 1; \
	done
	@echo "lint: all primitives OK"

tb:
	@fail=0; \
	for d in $(TB_DIRS); do \
	    echo "[tb] $$d"; \
	    $(MAKE) -C $$d run || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then \
	    echo "tb: one or more TBs FAILED"; exit 1; \
	else \
	    echo "tb: all 4 CDC TBs PASSED"; \
	fi

tb-clean:
	@for d in $(TB_DIRS); do $(MAKE) -C $$d clean || true; done
