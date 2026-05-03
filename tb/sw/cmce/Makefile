# tb/sw/cmce/Makefile — S4 CMCE host unit-tests.
#
# Owned by S4 (S4-sw-cmce). Picked up automatically by top-level
# `make sw-test` via the tb/sw/<block>/Makefile glob in Makefile (T0
# build skeleton).

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/sw/Makefile.inc

CORE_INC := $(REPO_ROOT)/sw/core/include
LLC_INC  := $(REPO_ROOT)/sw/llc/include
CMCE_INC := $(REPO_ROOT)/sw/cmce/include

CORE_SRC := $(REPO_ROOT)/sw/core/src/msgbus.c \
            $(REPO_ROOT)/sw/core/src/bitbuffer.c
LLC_SRC  := $(REPO_ROOT)/sw/llc/llc.c \
            $(REPO_ROOT)/sw/llc/llc_pdu.c
CMCE_SRC := $(REPO_ROOT)/sw/cmce/cmce.c \
            $(REPO_ROOT)/sw/cmce/cmce_pdu.c \
            $(REPO_ROOT)/sw/cmce/cmce_fsm.c \
            $(REPO_ROOT)/sw/cmce/cmce_nwrk_bcast.c

CFLAGS_UNITY += -I$(CORE_INC) -I$(LLC_INC) -I$(CMCE_INC)

# Three test binaries per agent contract.
TESTS := test_cmce_d_nwrk_broadcast test_cmce_pdu test_cmce_fsm

$(eval $(call sw_test_rule,test_cmce_d_nwrk_broadcast,test_cmce_d_nwrk_broadcast.c $(CMCE_SRC) $(LLC_SRC) $(CORE_SRC)))
$(eval $(call sw_test_rule,test_cmce_pdu,test_cmce_pdu.c $(CMCE_SRC) $(LLC_SRC) $(CORE_SRC)))
$(eval $(call sw_test_rule,test_cmce_fsm,test_cmce_fsm.c $(CMCE_SRC) $(LLC_SRC) $(CORE_SRC)))

.PHONY: all clean
all: $(TESTS)

clean:
	rm -rf $(SW_BUILD)
