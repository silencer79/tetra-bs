# tb/rtl/tb_cdc_sync_2ff/Makefile — A4 CDC sync_2ff TB.

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/rtl/Makefile.inc

TB_NAME := tb_cdc_sync_2ff
SRCS    := tb_cdc_sync_2ff.v $(REPO_ROOT)/rtl/infra/cdc/cdc_sync_2ff.v

$(eval $(call rtl_tb_rule,$(TB_NAME),$(SRCS)))

.PHONY: all clean
all: $(TB_NAME)

clean:
	rm -rf $(RTL_BUILD)
