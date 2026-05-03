# tb/rtl/tb_dma_frame_packer/Makefile — A1 frame-packer TB.

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/rtl/Makefile.inc

TB_NAME := tb_dma_frame_packer
SRCS    := tb_dma_frame_packer.v $(REPO_ROOT)/rtl/infra/tetra_dma_frame_packer.v

$(eval $(call rtl_tb_rule,$(TB_NAME),$(SRCS)))

.PHONY: all clean
all: $(TB_NAME)

clean:
	rm -rf $(RTL_BUILD)
