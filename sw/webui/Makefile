# sw/webui/Makefile — S6 CGI binaries.
#
# Owned by S6 (S6-sw-webui-cgis). Builds every CGI listed below by
# statically linking sw/webui/cgi_common.c into a small per-binary
# main.c under sw/webui/src/.
#
# Targets:
#   all       — build every CGI under build/cgi-bin/.
#   <name>    — build a single CGI, e.g. `make status.cgi`.
#   clean     — remove build/cgi-bin/ + .o files.
#
# Two compile profiles:
#   make           → host gcc (Ubuntu 24.04, /usr/bin/gcc), used by
#                    sw-test mocks + by `make sw-build` invocation
#                    when CC is overridden.
#   make ARM=1     → /usr/bin/arm-linux-gnueabihf-gcc, hard-float ARMv7-A.
#
# The top-level Makefile invokes `make ARM=1` from sw-build.

WEBUI_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT := $(abspath $(WEBUI_DIR)/../..)
SRC_DIR   := $(WEBUI_DIR)/src
INC_DIR   := $(WEBUI_DIR)/include

ifeq ($(ARM),1)
CC      := /usr/bin/arm-linux-gnueabihf-gcc
CFLAGS  := -std=c11 -O2 -g \
           -march=armv7-a -mfpu=neon-vfpv3 -mfloat-abi=hard \
           -Wall -Wextra -Werror
OUT_DIR := $(REPO_ROOT)/build/arm/cgi-bin
else
CC      ?= /usr/bin/gcc
CFLAGS  ?= -std=c11 -O0 -g -Wall -Wextra -Werror
OUT_DIR := $(REPO_ROOT)/build/cgi-bin
endif

CFLAGS += -I$(INC_DIR)

CGI_NAMES := apply.cgi entities.cgi policy.cgi profiles.cgi sessions.cgi \
             status.cgi stop.cgi db.cgi debug.cgi config.cgi tools.cgi \
             jobs.cgi
CGI_BINS  := $(addprefix $(OUT_DIR)/,$(CGI_NAMES))

COMMON_SRC := $(WEBUI_DIR)/cgi_common.c
COMMON_HDR := $(INC_DIR)/tetra/cgi_common.h

# main.c per CGI: <stem>_main.c
define src_for
$(SRC_DIR)/$(patsubst %.cgi,%,$(1))_main.c
endef

.PHONY: all clean
all: $(CGI_BINS)

$(OUT_DIR):
	@mkdir -p $@

# Generic rule: each CGI pulls in cgi_common.c + its own main.c.
$(OUT_DIR)/%.cgi: $(SRC_DIR)/%_main.c $(COMMON_SRC) $(COMMON_HDR) | $(OUT_DIR)
	@echo "[cgi] $(CC) -> $@"
	$(CC) $(CFLAGS) -o $@ $< $(COMMON_SRC)

# Phony aliases so `make status.cgi` works from this dir.
.PHONY: $(CGI_NAMES)
$(CGI_NAMES): %.cgi: $(OUT_DIR)/%.cgi

clean:
	rm -f $(CGI_BINS)
	@rmdir $(OUT_DIR) 2>/dev/null || true
