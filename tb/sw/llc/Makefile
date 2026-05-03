# tb/sw/llc/Makefile — S2 LLC host unit-tests.
#
# Owned by S2 (S2-sw-llc). Picked up automatically by top-level
# `make sw-test` via the tb/sw/<block>/Makefile glob in Makefile (T0
# build skeleton).

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/sw/Makefile.inc

CORE_INC := $(REPO_ROOT)/sw/core/include
LLC_INC  := $(REPO_ROOT)/sw/llc/include

CORE_SRC := $(REPO_ROOT)/sw/core/src/msgbus.c \
            $(REPO_ROOT)/sw/core/src/bitbuffer.c
LLC_SRC  := $(REPO_ROOT)/sw/llc/llc.c \
            $(REPO_ROOT)/sw/llc/llc_pdu.c

CFLAGS_UNITY += -I$(CORE_INC) -I$(LLC_INC)

TEST_BIN := test_llc_pdu
SRCS     := test_llc_pdu.c $(LLC_SRC) $(CORE_SRC)

$(eval $(call sw_test_rule,$(TEST_BIN),$(SRCS)))

.PHONY: all clean
all: $(TEST_BIN)

clean:
	rm -rf $(SW_BUILD)
