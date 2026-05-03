# tb/sw/_smoke/Makefile — smoke SW host-test.
#
# Owned by T0 build-skeleton. Pattern for all per-block SW host-test
# Makefiles: only declare TEST_BIN + SRCS, then include the shared
# Makefile.inc and call sw_test_rule.

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/sw/Makefile.inc

TEST_BIN := test_smoke
SRCS     := test_smoke.c

$(eval $(call sw_test_rule,$(TEST_BIN),$(SRCS)))

.PHONY: all clean
all: $(TEST_BIN)

clean:
	rm -rf $(SW_BUILD)
