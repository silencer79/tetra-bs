# tb/rtl/tb_cdc_pulse/Makefile — A4 CDC pulse TB.

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/rtl/Makefile.inc

TB_NAME := tb_cdc_pulse
SRCS    := tb_cdc_pulse.v $(REPO_ROOT)/rtl/infra/cdc/cdc_pulse.v

$(eval $(call rtl_tb_rule,$(TB_NAME),$(SRCS)))

.PHONY: all clean
all: $(TB_NAME)

clean:
	rm -rf $(RTL_BUILD)
