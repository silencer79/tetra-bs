# tb/rtl/tb_axi_dma_wrapper/Makefile — A1 wrapper TB.
#
# Pulls in the behavioural axi_dma_v7_1 model from tb/rtl/models/.

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/rtl/Makefile.inc

TB_NAME := tb_axi_dma_wrapper
SRCS    := tb_axi_dma_wrapper.v \
           $(REPO_ROOT)/rtl/infra/tetra_axi_dma_wrapper.v \
           $(REPO_ROOT)/tb/rtl/models/axi_dma_v7_1_bhv.v

$(eval $(call rtl_tb_rule,$(TB_NAME),$(SRCS)))

.PHONY: all clean
all: $(TB_NAME)

clean:
	rm -rf $(RTL_BUILD)
