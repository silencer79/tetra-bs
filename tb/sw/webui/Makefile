# tb/sw/webui/Makefile — S6 host unit tests for sw/webui.
#
# Owned by S6 (S6-sw-webui-cgis). Picked up automatically by top-level
# `make sw-test` via the tb/sw/<block>/Makefile glob.
#
# Two test binaries:
#   test_cgi_common    — env/stdin/length-prefix/round-trip via mocked
#                        AF_UNIX socket (no S7 required).
#   test_op_allowlist  — every CGI binary's main() is included as a
#                        renamed function and exercised against a
#                        forbidden op; assert FORBIDDEN_OP envelope.

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/sw/Makefile.inc

WEBUI_DIR := $(REPO_ROOT)/sw/webui
INC_DIR   := $(WEBUI_DIR)/include
COMMON_SRC := $(WEBUI_DIR)/cgi_common.c

CFLAGS_UNITY += -I$(INC_DIR) -pthread
LDLIBS       += -pthread

TEST_BIN_COMMON   := test_cgi_common
SRCS_COMMON       := test_cgi_common.c $(COMMON_SRC)

TEST_BIN_ALLOW    := test_op_allowlist
SRCS_ALLOW        := test_op_allowlist.c $(COMMON_SRC)

# We don't use the shared sw_test_rule macro here because it doesn't
# pass LDLIBS; pthread is needed for the mock-daemon thread. Manually
# wire the rules up.
$(SW_BUILD)/$(TEST_BIN_COMMON): $(SRCS_COMMON) $(UNITY_OBJ) | $(SW_BUILD)
	$(CC) $(CFLAGS_UNITY) -o $@ $(SRCS_COMMON) $(UNITY_OBJ) $(LDLIBS)

$(SW_BUILD)/$(TEST_BIN_ALLOW): $(SRCS_ALLOW) $(UNITY_OBJ) | $(SW_BUILD)
	$(CC) $(CFLAGS_UNITY) -o $@ $(SRCS_ALLOW) $(UNITY_OBJ) $(LDLIBS)

.PHONY: all clean test_cgi_common test_op_allowlist
all: test_cgi_common test_op_allowlist

test_cgi_common: $(SW_BUILD)/$(TEST_BIN_COMMON)
	@echo "[SW-TEST] $@"
	@$<

test_op_allowlist: $(SW_BUILD)/$(TEST_BIN_ALLOW)
	@echo "[SW-TEST] $@"
	@$<

clean:
	rm -rf $(SW_BUILD)
