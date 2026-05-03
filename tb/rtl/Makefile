# =============================================================================
# tb/rtl/Makefile — top-level driver to run every per-TB Makefile.
# =============================================================================
# T1 will replace this with a fancier driver (waveform dumps, parallel runs).
# For A6 the requirement is "make tb" green: `make` here invokes every
# subdir's `make` and aggregates pass/fail.
# =============================================================================

A6_TBS := \
    tb_ul_demand_reassembly \
    tb_mac_access_parser \
    tb_dl_signal_queue \
    tb_dl_signal_scheduler \
    tb_sch_f_encoder \
    tb_mac_resource_dl_builder

.PHONY: all run clean $(A6_TBS) $(addprefix clean-,$(A6_TBS))

all: run

run: $(A6_TBS)
	@echo "================================================================"
	@echo "All A6 carry-over UMAC TBs PASSED."
	@echo "================================================================"

$(A6_TBS):
	@echo "==== running $@ ===="
	@$(MAKE) -C $@ run

clean: $(addprefix clean-,$(A6_TBS))

$(addprefix clean-,$(A6_TBS)):
	@$(MAKE) -C $(@:clean-%=%) clean
