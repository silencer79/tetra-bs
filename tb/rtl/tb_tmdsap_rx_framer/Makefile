# tb/rtl/tb_tmdsap_rx_framer/Makefile — A3 TmdSap-RX framer TB.

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/rtl/Makefile.inc

TB_NAME := tb_tmdsap_rx_framer
SRCS    := tb_tmdsap_rx_framer.v $(REPO_ROOT)/rtl/infra/tetra_tmdsap_rx_framer.v

$(eval $(call rtl_tb_rule,$(TB_NAME),$(SRCS)))

.PHONY: all clean
all: $(TB_NAME)

clean:
	rm -rf $(RTL_BUILD)
