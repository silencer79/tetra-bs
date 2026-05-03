# tb/sw/persistence/Makefile — S5 host unit-tests.
#
# Owned by S5 (S5-sw-persistence). Picked up automatically by top-level
# `make sw-test` via the tb/sw/<block>/Makefile glob in the root Makefile.
#
# jansson: linked via pkg-config (HARDWARE.md §5). On host, install with
# `apt install libjansson-dev`. Set PKG_CONFIG_PATH if jansson lives in a
# non-default prefix (e.g. user-local install for offline workstations).
#
# This block dynamically links libjansson (host-side default). The
# production cross-compile decision (static vs dynamic for the ARM
# binary) is documented in HARDWARE.md §10 and is a tetra_d-build switch,
# not a test-build switch.

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/sw/Makefile.inc

PERSIST_INC := $(REPO_ROOT)/sw/persistence/include
CORE_INC    := $(REPO_ROOT)/sw/core/include
PERSIST_SRC := $(REPO_ROOT)/sw/persistence/src/db.c \
               $(REPO_ROOT)/sw/persistence/src/ast_snapshot.c

JANSSON_CFLAGS := $(shell pkg-config --cflags jansson 2>/dev/null)
JANSSON_LIBS   := $(shell pkg-config --libs   jansson 2>/dev/null)

ifeq ($(strip $(JANSSON_LIBS)),)
$(error libjansson not found via pkg-config; \
  run 'apt install libjansson-dev' or set PKG_CONFIG_PATH)
endif

CFLAGS_UNITY += -I$(PERSIST_INC) -I$(CORE_INC) $(JANSSON_CFLAGS)

TEST_BIN := test_db_atomic
SRCS     := test_db_atomic.c $(PERSIST_SRC)

# Override the generic sw_test_rule recipe to thread $(JANSSON_LIBS) through.
$(SW_BUILD)/$(TEST_BIN): $(SRCS) $(UNITY_OBJ) | $(SW_BUILD)
	$(CC) $(CFLAGS_UNITY) -o $@ $(SRCS) $(UNITY_OBJ) $(JANSSON_LIBS)

$(TEST_BIN): $(SW_BUILD)/$(TEST_BIN)
	@echo "[SW-TEST] $(TEST_BIN)"
	@$<

.PHONY: $(TEST_BIN) all clean
all: $(TEST_BIN)

clean:
	rm -rf $(SW_BUILD)
