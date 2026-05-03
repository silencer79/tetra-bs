# tb/sw/Makefile.inc — common per-block SW host-test rules.
#
# Owned by T0 build-skeleton. Per-block Makefile pattern:
#
#   include $(REPO_ROOT)/tb/sw/Makefile.inc
#   TEST_BIN := test_smoke
#   SRCS     := test_smoke.c
#   $(eval $(call sw_test_rule,$(TEST_BIN),$(SRCS)))
#
# Variables provided to including Makefiles:
#   CC           — host compiler. Pinned to /usr/bin/gcc per HARDWARE.md §5
#                  (Ubuntu 24.04 GCC 13.3.0). The PATH-precedence trap in
#                  §2 only affects ARM cross-compile; host gcc is safe to
#                  let PATH resolve, but we pin anyway for reproducibility.
#   CFLAGS       — common host-test flags (C11, all warnings as errors).
#   UNITY_DIR    — vendored Unity tree (sw/external/unity).
#   UNITY_OBJ    — pre-built unity.o, shared across tests.
#   SW_BUILD     — per-block build dir (defaults to ./build under per-block dir).
#
# Targets provided (per call to sw_test_rule):
#   $(SW_BUILD)/$(TEST_BIN)  — compiled binary, links unity.o.
#   <TEST_BIN>               — phony, runs the binary; exits non-zero on fail.
#
# Pass/fail contract: Unity's main() returns 0 on all-pass, non-zero on any
# failure. The harness propagates that exit code directly.

CC          ?= /usr/bin/gcc
CFLAGS      ?= -std=c11 -O0 -g -Wall -Wextra -Werror

# Resolve repo root (Makefile.inc lives in tb/sw/, so up two).
SW_INC_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
REPO_ROOT  ?= $(abspath $(SW_INC_DIR)/../..)

UNITY_DIR ?= $(REPO_ROOT)/sw/external/unity
UNITY_SRC := $(UNITY_DIR)/unity.c
UNITY_HDR := $(UNITY_DIR)/unity.h $(UNITY_DIR)/unity_internals.h

# Per-block build dir (override-able per Makefile).
SW_BUILD ?= build
UNITY_OBJ := $(SW_BUILD)/unity.o

CFLAGS_UNITY := $(CFLAGS) -I$(UNITY_DIR)

$(SW_BUILD):
	@mkdir -p $@

$(UNITY_OBJ): $(UNITY_SRC) $(UNITY_HDR) | $(SW_BUILD)
	$(CC) $(CFLAGS_UNITY) -c $(UNITY_SRC) -o $@

# Macro: sw_test_rule(test_bin, srcs)
#   Generates compile + run rules for a single test binary.
#   Sources resolved relative to calling Makefile's dir; test main() is
#   responsible for calling UNITY_BEGIN/RUN_TEST/UNITY_END.
define sw_test_rule
$(SW_BUILD)/$(1): $(2) $(UNITY_OBJ) | $(SW_BUILD)
	$$(CC) $$(CFLAGS_UNITY) -o $$@ $(2) $(UNITY_OBJ)

$(1): $(SW_BUILD)/$(1)
	@echo "[SW-TEST] $(1)"
	@$$<

.PHONY: $(1)
endef
