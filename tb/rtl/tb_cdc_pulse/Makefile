# tb_cdc_pulse/Makefile
TOP      := tb_cdc_pulse
RTL_DIR  := ../../../rtl/infra/cdc
SRCS     := $(RTL_DIR)/cdc_pulse.v $(TOP).v
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
