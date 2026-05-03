# tb/sw/mm/Makefile — S3 MM host unit-tests.
#
# Owned by S3 (S3-sw-mle-mm). Picked up automatically by top-level
# `make sw-test` via the tb/sw/<block>/Makefile glob.

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/sw/Makefile.inc

CORE_INC    := $(REPO_ROOT)/sw/core/include
LLC_INC     := $(REPO_ROOT)/sw/llc/include
PERSIST_INC := $(REPO_ROOT)/sw/persistence/include
MM_INC      := $(REPO_ROOT)/sw/mm/include

CORE_SRC := $(REPO_ROOT)/sw/core/src/msgbus.c \
            $(REPO_ROOT)/sw/core/src/bitbuffer.c
LLC_SRC  := $(REPO_ROOT)/sw/llc/llc.c \
            $(REPO_ROOT)/sw/llc/llc_pdu.c
MM_SRC   := $(REPO_ROOT)/sw/mm/mm.c \
            $(REPO_ROOT)/sw/mm/mm_iep.c \
            $(REPO_ROOT)/sw/mm/mm_accept_builder.c
DB_SRC   := $(REPO_ROOT)/sw/persistence/src/db.c \
            $(REPO_ROOT)/sw/persistence/src/ast_snapshot.c

JANSSON_CFLAGS := $(shell pkg-config --cflags jansson 2>/dev/null)
JANSSON_LIBS   := $(shell pkg-config --libs   jansson 2>/dev/null)

ifeq ($(strip $(JANSSON_LIBS)),)
$(error libjansson not found via pkg-config; \
  run 'apt install libjansson-dev' or set PKG_CONFIG_PATH)
endif

CFLAGS_UNITY += -I$(CORE_INC) -I$(LLC_INC) -I$(PERSIST_INC) -I$(MM_INC) \
                $(JANSSON_CFLAGS)

ACCEPT_BIN  := test_mm_d_loc_update_accept
ACCEPT_SRCS := test_mm_d_loc_update_accept.c $(MM_SRC) $(CORE_SRC) $(LLC_SRC) \
               $(DB_SRC)

GRPACK_BIN  := test_mm_iep_d_attach_grp_ack
GRPACK_SRCS := test_mm_iep_d_attach_grp_ack.c $(MM_SRC) $(CORE_SRC) $(LLC_SRC) \
               $(DB_SRC)

DECODE_BIN  := test_mm_iep_decode
DECODE_SRCS := test_mm_iep_decode.c $(MM_SRC) $(CORE_SRC) $(LLC_SRC) \
               $(DB_SRC)

# Override the generic sw_test_rule recipe to thread $(JANSSON_LIBS) through.
$(SW_BUILD)/$(ACCEPT_BIN): $(ACCEPT_SRCS) $(UNITY_OBJ) | $(SW_BUILD)
	$(CC) $(CFLAGS_UNITY) -o $@ $(ACCEPT_SRCS) $(UNITY_OBJ) $(JANSSON_LIBS)
$(ACCEPT_BIN): $(SW_BUILD)/$(ACCEPT_BIN)
	@echo "[SW-TEST] $(ACCEPT_BIN)"
	@$<

$(SW_BUILD)/$(GRPACK_BIN): $(GRPACK_SRCS) $(UNITY_OBJ) | $(SW_BUILD)
	$(CC) $(CFLAGS_UNITY) -o $@ $(GRPACK_SRCS) $(UNITY_OBJ) $(JANSSON_LIBS)
$(GRPACK_BIN): $(SW_BUILD)/$(GRPACK_BIN)
	@echo "[SW-TEST] $(GRPACK_BIN)"
	@$<

$(SW_BUILD)/$(DECODE_BIN): $(DECODE_SRCS) $(UNITY_OBJ) | $(SW_BUILD)
	$(CC) $(CFLAGS_UNITY) -o $@ $(DECODE_SRCS) $(UNITY_OBJ) $(JANSSON_LIBS)
$(DECODE_BIN): $(SW_BUILD)/$(DECODE_BIN)
	@echo "[SW-TEST] $(DECODE_BIN)"
	@$<

.PHONY: $(ACCEPT_BIN) $(GRPACK_BIN) $(DECODE_BIN)

.PHONY: all clean
all: $(ACCEPT_BIN) $(GRPACK_BIN) $(DECODE_BIN)

clean:
	rm -rf $(SW_BUILD)
