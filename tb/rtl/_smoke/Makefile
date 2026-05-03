# tb/rtl/_smoke/Makefile — smoke RTL TB.
#
# Owned by T0 build-skeleton. Pattern for all per-block RTL TB Makefiles:
# only declare TB_NAME + SRCS, then include the shared Makefile.inc and
# call rtl_tb_rule.

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/rtl/Makefile.inc

TB_NAME := tb_smoke
SRCS    := tb_smoke.v

$(eval $(call rtl_tb_rule,$(TB_NAME),$(SRCS)))

.PHONY: all clean
all: $(TB_NAME)

clean:
	rm -rf $(RTL_BUILD)
