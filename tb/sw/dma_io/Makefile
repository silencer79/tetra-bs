# tb/sw/dma_io/Makefile — S1 host unit-tests.
#
# Owned by S1 (S1-sw-dma-glue). Picked up automatically by top-level
# `make sw-test` via the tb/sw/<block>/Makefile glob.
#
# HAVE_LIBAXIDMA is intentionally NOT defined here — host CI runs the
# pipe-mock backend (Ubuntu 24.04 has no libaxidma packaged; see
# docs/HARDWARE.md §4 + §10). The real-HW build path is exercised in
# T2 cosim / Phase-4 live, not in unit tests.

REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(REPO_ROOT)/tb/sw/Makefile.inc

DMA_INC := $(REPO_ROOT)/sw/dma_io/include
DMA_PRIV := $(REPO_ROOT)/sw/dma_io
DMA_SRC := $(REPO_ROOT)/sw/dma_io/dma_io.c

CFLAGS_UNITY += -I$(DMA_INC) -I$(DMA_PRIV)

TEST_BIN := test_dma_loopback
SRCS     := test_dma_loopback.c $(DMA_SRC)

$(eval $(call sw_test_rule,$(TEST_BIN),$(SRCS)))

.PHONY: all clean
all: $(TEST_BIN)

clean:
	rm -rf $(SW_BUILD)
