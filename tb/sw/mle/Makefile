# tb/sw/mle/Makefile — S3 MLE host unit-tests.
#
# Owned by S3 (S3-sw-mle-mm). Picked up automatically by top-level
# `make sw-test`.

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/sw/Makefile.inc

CORE_INC    := $(REPO_ROOT)/sw/core/include
LLC_INC     := $(REPO_ROOT)/sw/llc/include
PERSIST_INC := $(REPO_ROOT)/sw/persistence/include
MM_INC      := $(REPO_ROOT)/sw/mm/include
MLE_INC     := $(REPO_ROOT)/sw/mle/include

CORE_SRC := $(REPO_ROOT)/sw/core/src/msgbus.c \
            $(REPO_ROOT)/sw/core/src/bitbuffer.c
LLC_SRC  := $(REPO_ROOT)/sw/llc/llc.c \
            $(REPO_ROOT)/sw/llc/llc_pdu.c
MM_SRC   := $(REPO_ROOT)/sw/mm/mm.c \
            $(REPO_ROOT)/sw/mm/mm_iep.c \
            $(REPO_ROOT)/sw/mm/mm_accept_builder.c
MLE_SRC  := $(REPO_ROOT)/sw/mle/mle.c \
            $(REPO_ROOT)/sw/mle/mle_fsm.c
DB_SRC   := $(REPO_ROOT)/sw/persistence/src/db.c \
            $(REPO_ROOT)/sw/persistence/src/ast_snapshot.c

JANSSON_CFLAGS := $(shell pkg-config --cflags jansson 2>/dev/null)
JANSSON_LIBS   := $(shell pkg-config --libs   jansson 2>/dev/null)

ifeq ($(strip $(JANSSON_LIBS)),)
$(error libjansson not found via pkg-config; \
  run 'apt install libjansson-dev' or set PKG_CONFIG_PATH)
endif

CFLAGS_UNITY += -I$(CORE_INC) -I$(LLC_INC) -I$(PERSIST_INC) \
                -I$(MM_INC) -I$(MLE_INC) $(JANSSON_CFLAGS)

TEST_BIN := test_mle_fsm
SRCS     := test_mle_fsm.c $(MLE_SRC) $(MM_SRC) $(CORE_SRC) $(LLC_SRC) $(DB_SRC)

$(SW_BUILD)/$(TEST_BIN): $(SRCS) $(UNITY_OBJ) | $(SW_BUILD)
	$(CC) $(CFLAGS_UNITY) -o $@ $(SRCS) $(UNITY_OBJ) $(JANSSON_LIBS)

$(TEST_BIN): $(SW_BUILD)/$(TEST_BIN)
	@echo "[SW-TEST] $(TEST_BIN)"
	@$<

.PHONY: $(TEST_BIN)

.PHONY: all clean
all: $(TEST_BIN)

clean:
	rm -rf $(SW_BUILD)
