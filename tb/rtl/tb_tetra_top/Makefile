# tb/rtl/tb_tetra_top/Makefile — A5 top-level smoke TB.
#
# Owned by Agent A5 (A5-fpga-top-xdc-cleanup).
#
# Compiled with -DTETRA_TOP_NO_PHY so the heavy PHY/LMAC-coding chain
# is excluded — iverilog cannot elaborate the full PHY at acceptable
# speed. Production Vivado synth uses the full DUT (no -D).

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/rtl/Makefile.inc

IVERILOG_FLAGS += -DTETRA_TOP_NO_PHY

TB_NAME := tb_tetra_top
SRCS    := tb_tetra_top.v \
           $(REPO_ROOT)/rtl/tetra_top.v \
           $(REPO_ROOT)/rtl/infra/tetra_axi_lite_regs.v \
           $(REPO_ROOT)/rtl/infra/tetra_axi_dma_wrapper.v \
           $(REPO_ROOT)/rtl/infra/tetra_tmasap_rx_framer.v \
           $(REPO_ROOT)/rtl/infra/tetra_tmasap_tx_framer.v \
           $(REPO_ROOT)/rtl/infra/tetra_tmdsap_rx_framer.v \
           $(REPO_ROOT)/rtl/infra/tetra_tmdsap_tx_framer.v \
           $(REPO_ROOT)/tb/rtl/models/axi_dma_v7_1_bhv.v

$(eval $(call rtl_tb_rule,$(TB_NAME),$(SRCS)))

.PHONY: all clean run
all: $(TB_NAME)
run: $(TB_NAME)

clean:
	rm -rf $(RTL_BUILD)
