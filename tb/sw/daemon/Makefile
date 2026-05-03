# tb/sw/daemon/Makefile — S7 daemon host unit-tests.
#
# Owned by S7 (S7-sw-tetra-d). Picked up automatically by top-level
# `make sw-test` via the tb/sw/<block>/Makefile glob in the root Makefile.
#
# Three test binaries:
#   test_socket_envelope — IF_DAEMON_OPS_v1 envelope round-trip + listen.
#                          Links sw/socket_server.c only (no entities).
#   test_main_loop       — main_loop dispatch + shutdown-flag plumbing.
#                          Links the full SW stack so msgbus + LLC + MLE +
#                          MM + CMCE init paths exercise.
#   test_shutdown        — AST snapshot/reload lifecycle (Decision #10).
#                          Links sw/persistence/ only.
#
# jansson + pthread (jansson sometimes pulls libpthread on older
# distros): linked via pkg-config (HARDWARE.md §5).

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/sw/Makefile.inc

CORE_INC    := $(REPO_ROOT)/sw/core/include
DMA_INC     := $(REPO_ROOT)/sw/dma_io/include
LLC_INC     := $(REPO_ROOT)/sw/llc/include
MLE_INC     := $(REPO_ROOT)/sw/mle/include
MM_INC      := $(REPO_ROOT)/sw/mm/include
CMCE_INC    := $(REPO_ROOT)/sw/cmce/include
PERSIST_INC := $(REPO_ROOT)/sw/persistence/include
DAEMON_INC  := $(REPO_ROOT)/sw/include

CORE_SRC    := $(REPO_ROOT)/sw/core/src/msgbus.c \
               $(REPO_ROOT)/sw/core/src/bitbuffer.c
DMA_SRC     := $(REPO_ROOT)/sw/dma_io/dma_io.c
LLC_SRC     := $(REPO_ROOT)/sw/llc/llc.c \
               $(REPO_ROOT)/sw/llc/llc_pdu.c
MLE_SRC     := $(REPO_ROOT)/sw/mle/mle.c \
               $(REPO_ROOT)/sw/mle/mle_fsm.c
MM_SRC      := $(REPO_ROOT)/sw/mm/mm.c \
               $(REPO_ROOT)/sw/mm/mm_iep.c \
               $(REPO_ROOT)/sw/mm/mm_accept_builder.c
CMCE_SRC    := $(REPO_ROOT)/sw/cmce/cmce.c \
               $(REPO_ROOT)/sw/cmce/cmce_pdu.c \
               $(REPO_ROOT)/sw/cmce/cmce_fsm.c \
               $(REPO_ROOT)/sw/cmce/cmce_nwrk_bcast.c
PERSIST_SRC := $(REPO_ROOT)/sw/persistence/src/db.c \
               $(REPO_ROOT)/sw/persistence/src/ast_snapshot.c
DAEMON_SRC  := $(REPO_ROOT)/sw/socket_server.c \
               $(REPO_ROOT)/sw/main_loop.c

JANSSON_CFLAGS := $(shell pkg-config --cflags jansson 2>/dev/null)
JANSSON_LIBS   := $(shell pkg-config --libs   jansson 2>/dev/null)

ifeq ($(strip $(JANSSON_LIBS)),)
$(error libjansson not found via pkg-config; \
  run 'apt install libjansson-dev' or set PKG_CONFIG_PATH)
endif

CFLAGS_UNITY += -I$(CORE_INC) -I$(DMA_INC) -I$(LLC_INC) -I$(MLE_INC) \
                -I$(MM_INC) -I$(CMCE_INC) -I$(PERSIST_INC) \
                -I$(DAEMON_INC) $(JANSSON_CFLAGS)

# ----- test_socket_envelope -------------------------------------------------
ENV_SRCS := test_socket_envelope.c \
            $(REPO_ROOT)/sw/socket_server.c

$(SW_BUILD)/test_socket_envelope: $(ENV_SRCS) $(UNITY_OBJ) | $(SW_BUILD)
	$(CC) $(CFLAGS_UNITY) -o $@ $(ENV_SRCS) $(UNITY_OBJ) $(JANSSON_LIBS)

test_socket_envelope: $(SW_BUILD)/test_socket_envelope
	@echo "[SW-TEST] test_socket_envelope"
	@$<

# ----- test_main_loop -------------------------------------------------------
ML_SRCS := test_main_loop.c \
           $(DAEMON_SRC) \
           $(CORE_SRC) $(DMA_SRC) $(LLC_SRC) $(MLE_SRC) $(MM_SRC) \
           $(CMCE_SRC) $(PERSIST_SRC)

$(SW_BUILD)/test_main_loop: $(ML_SRCS) $(UNITY_OBJ) | $(SW_BUILD)
	$(CC) $(CFLAGS_UNITY) -o $@ $(ML_SRCS) $(UNITY_OBJ) $(JANSSON_LIBS)

test_main_loop: $(SW_BUILD)/test_main_loop
	@echo "[SW-TEST] test_main_loop"
	@$<

# ----- test_shutdown --------------------------------------------------------
SD_SRCS := test_shutdown.c \
           $(PERSIST_SRC)

$(SW_BUILD)/test_shutdown: $(SD_SRCS) $(UNITY_OBJ) | $(SW_BUILD)
	$(CC) $(CFLAGS_UNITY) -o $@ $(SD_SRCS) $(UNITY_OBJ) $(JANSSON_LIBS)

test_shutdown: $(SW_BUILD)/test_shutdown
	@echo "[SW-TEST] test_shutdown"
	@$<

.PHONY: test_socket_envelope test_main_loop test_shutdown all clean
all: test_socket_envelope test_main_loop test_shutdown

clean:
	rm -rf $(SW_BUILD)
