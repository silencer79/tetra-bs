# tb/rtl/tb_tmdsap_tx_framer/Makefile — A3 TmdSap-TX framer TB.

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/rtl/Makefile.inc

TB_NAME := tb_tmdsap_tx_framer
SRCS    := tb_tmdsap_tx_framer.v $(REPO_ROOT)/rtl/infra/tetra_tmdsap_tx_framer.v

$(eval $(call rtl_tb_rule,$(TB_NAME),$(SRCS)))

.PHONY: all clean
all: $(TB_NAME)

clean:
	rm -rf $(RTL_BUILD)
