# rtl/infra/Makefile — lint targets for Agent A1 (A1-fpga-axi-dma) wrappers.
#
# This Makefile compiles each wrapper file standalone with iverilog -g2001
# to catch syntax / Verilog-2001-compliance regressions without running a
# full TB. The actual functional tests live under tb/rtl/tb_*/.
#
# Targets:
#   make all      — lint-compile each wrapper.
#   make lint     — alias for all.
#   make clean    — remove build/ tree.

IVERILOG       ?= /usr/bin/iverilog
IVERILOG_FLAGS ?= -g2001 -Wall

BUILD := build

# Files to lint-compile. tetra_axi_dma_wrapper.v depends on the
# axi_dma_channel_inst module which lives in tb/rtl/models/ for sim and
# in Vivado-IP for synth — so to lint the wrapper standalone we pull in
# the behavioural model definition (it is the canonical port-list).
WRAPPER_SRCS := \
    tetra_axi_dma_wrapper.v \
    ../../tb/rtl/models/axi_dma_v7_1_bhv.v

PACKER_SRCS   := tetra_dma_frame_packer.v
UNPACKER_SRCS := tetra_dma_frame_unpacker.v

.PHONY: all lint clean

all: lint

lint: $(BUILD)/wrapper.lint $(BUILD)/packer.lint $(BUILD)/unpacker.lint
	@echo "[lint] OK rtl/infra/"

$(BUILD)/wrapper.lint: $(WRAPPER_SRCS) | $(BUILD)
	$(IVERILOG) $(IVERILOG_FLAGS) -o $@ -t null $(WRAPPER_SRCS)
	@touch $@

$(BUILD)/packer.lint: $(PACKER_SRCS) | $(BUILD)
	$(IVERILOG) $(IVERILOG_FLAGS) -o $@ -t null $(PACKER_SRCS)
	@touch $@

$(BUILD)/unpacker.lint: $(UNPACKER_SRCS) | $(BUILD)
	$(IVERILOG) $(IVERILOG_FLAGS) -o $@ -t null $(UNPACKER_SRCS)
	@touch $@

$(BUILD):
	@mkdir -p $@

clean:
	rm -rf $(BUILD)
