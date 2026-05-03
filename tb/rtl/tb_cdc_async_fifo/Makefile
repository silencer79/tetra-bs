# tb/rtl/tb_cdc_async_fifo/Makefile — A4 CDC async-FIFO TB.

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/rtl/Makefile.inc

TB_NAME := tb_cdc_async_fifo
SRCS    := tb_cdc_async_fifo.v $(REPO_ROOT)/rtl/infra/cdc/cdc_async_fifo.v

$(eval $(call rtl_tb_rule,$(TB_NAME),$(SRCS)))

.PHONY: all clean
all: $(TB_NAME)

clean:
	rm -rf $(RTL_BUILD)
