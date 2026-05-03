# tb/sw/core_msgbus/Makefile — S0 host unit-tests.
#
# Owned by S0 (S0-sw-core-msgbus-types). Picked up automatically by
# top-level `make sw-test` via the tb/sw/<block>/Makefile glob in
# Makefile (T0 build skeleton).

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/sw/Makefile.inc

CORE_INC := $(REPO_ROOT)/sw/core/include
CORE_SRC := $(REPO_ROOT)/sw/core/src/msgbus.c \
            $(REPO_ROOT)/sw/core/src/bitbuffer.c

CFLAGS_UNITY += -I$(CORE_INC)

TEST_BIN := test_core_msgbus
SRCS     := test_core_msgbus.c $(CORE_SRC)

$(eval $(call sw_test_rule,$(TEST_BIN),$(SRCS)))

.PHONY: all clean
all: $(TEST_BIN)

clean:
	rm -rf $(SW_BUILD)
