# tb_cdc_sync_2ff/Makefile
TOP      := tb_cdc_sync_2ff
RTL_DIR  := ../../../rtl/infra/cdc
SRCS     := $(RTL_DIR)/cdc_sync_2ff.v $(TOP).v
BIN      := $(TOP).vvp

.PHONY: all run clean
all: run

$(BIN): $(SRCS)
	iverilog -g2001 -o $@ $(SRCS)

run: $(BIN)
	@vvp $(BIN) | tee $(TOP).log
	@grep -q '^PASS' $(TOP).log

clean:
	rm -f $(BIN) $(TOP).log
