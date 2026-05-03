# scripts/build/synth.tcl — Vivado batch synthesis for tetra_top
#
# Project: tetra-bs (LibreSDR, Zynq-7020 XC7Z020-CLG400)
# Tool:    Vivado 2022.2 (HARDWARE.md §1)
# Top:     rtl/tetra_top.v  (Agent A5)
#
# Usage:   vivado -mode batch -source scripts/build/synth.tcl
#          (or via repo root: `make synth`)
#
# Outputs:
#   build/vivado/tetra_bs.bit            — Bitstream
#   build/vivado/tetra_bs.bit.bin        — bootgen-converted (for fpga_manager
#                                          /lib/firmware/ on Board #1)
#   build/vivado/reports/                — Timing + utilization
#   build/vivado/tetra_bs_project.xpr    — Vivado project (re-openable)

set PROJ_NAME   "tetra_bs"
set REPO_ROOT   [file normalize [file dirname [file dirname [file dirname [info script]]]]]
set BUILD_DIR   "$REPO_ROOT/build/vivado"
set REPORT_DIR  "$BUILD_DIR/reports"
set PART        "xc7z020clg400-1"
set TOP_MODULE  "tetra_top"

puts "============================================================"
puts " tetra-bs Vivado synth"
puts " Part : $PART  Top : $TOP_MODULE"
puts " Dir  : $REPO_ROOT"
puts " Build: $BUILD_DIR"
puts "============================================================"

file mkdir $BUILD_DIR
file mkdir $REPORT_DIR

# ---- Project --------------------------------------------------------------
set XPR_PATH "$BUILD_DIR/${PROJ_NAME}_project.xpr"
if {[file exists $XPR_PATH]} {
    puts "\[synth\] re-opening existing project $XPR_PATH"
    open_project $XPR_PATH
} else {
    puts "\[synth\] creating new project $XPR_PATH"
    create_project $PROJ_NAME $BUILD_DIR -part $PART -force
}
set_property target_language Verilog [current_project]
set_property default_lib work [current_project]

# ---- Xilinx LogiCORE IP : axi_dma:7.1 (4× channels) ----------------------
# `rtl/infra/tetra_axi_dma_wrapper.v` instantiates `axi_dma_channel_inst` 4×.
# In simulation, that name resolves to the behavioural model at
# `tb/rtl/models/axi_dma_v7_1_bhv.v`. For synth, we materialise a real
# Xilinx LogiCORE `axi_dma:7.1` IP and rename its top module to
# `axi_dma_channel_inst` so the wrapper picks it up.
#
# IP config mirrors carry-over `tetra_system_axi_dma_0_0.xci`:
#   c_include_sg            = 1   (Scatter-Gather enabled)
#   c_include_s2mm          = 1
#   c_include_s2mm_dre      = 1
#   c_include_mm2s          = 1   (carry-over had 0; we enable both
#                                  directions because the wrapper's
#                                  per-channel DIR_IS_S2MM toggles
#                                  S2MM-only vs MM2S-only at the wrapper
#                                  level, NOT inside the IP)
#   c_addr_width            = 32  (PS-DDR HP slave geometry)
#   c_m_axi_s2mm_data_width = 32
#   c_s_axis_s2mm_tdata_width = 32
#   c_m_axi_mm2s_data_width = 32
#   c_m_axis_mm2s_tdata_width = 32
#   c_sg_length_width       = 14  (carry-over default)
#   c_s2mm_burst_size       = 256 (carry-over user value)
#
# =============================================================================
# TODO(synth-ip-bringup, 2026-05-03): synth currently FAILS at this point
# with the following Vivado error (captured at
# build/vivado/reports/synth_failure.log line ~80):
#
#   ERROR: [Synth 8-7136] In the module 'axi_dma_channel_inst' declared at
#     '<build>/.Xil/Vivado-XXXX/realtime/axi_dma_channel_inst_stub.v:5',
#     parameter 'CHANNEL_ID' used as named parameter override, does not exist
#     [/home/kevin/claude-ralph/tetra-bs/rtl/infra/tetra_axi_dma_wrapper.v:400]
#   ERROR: [Synth 8-6156] failed synthesizing module 'tetra_axi_dma_wrapper'
#   ERROR: [Synth 8-6156] failed synthesizing module 'tetra_top'
#
# Root cause: the wrapper at `rtl/infra/tetra_axi_dma_wrapper.v:399..580`
# instantiates `axi_dma_channel_inst` four times with a *slim*, custom
# port-list and parameter set:
#
#     axi_dma_channel_inst #(
#         .CHANNEL_ID    (0..3),
#         .DIR_IS_S2MM   (0/1),
#         .AXIS_TDATA_W  (32), .AXIS_TKEEP_W (4),
#         .MM_ADDR_W     (32), .MM_DATA_W    (32)
#     ) u_chN_* ( ... slim AXIS s/m + slim AXI4-MM r/w + irq_done + ... );
#
# That signature is satisfied by the simulation behavioural model
# (`tb/rtl/models/axi_dma_v7_1_bhv.v`) but NOT by the real Xilinx
# `axi_dma:7.1` IP, which exposes the full LogiCORE port-list (separate
# AXI-Lite control slave, full burst signals AWLEN/AWBURST/AWCACHE/
# AWPROT/AWUSER/AWQOS/AWREGION, separate SG read+write masters,
# mm2s_introut/s2mm_introut, etc.) and accepts NO `CHANNEL_ID` /
# `DIR_IS_S2MM` parameters.
#
# Resolution requires a synthesis-only RTL shim — a new file something
# like `rtl/infra/ip/axi_dma_channel_inst.v` — that:
#   1. Exposes the slim port/param shape the wrapper expects (so the
#      bhv-model path keeps working unchanged for sim).
#   2. Internally instantiates the real `axi_dma_channel_inst_ip` IP
#      (renamed via `create_ip -module_name axi_dma_channel_inst_ip`,
#      NOT `axi_dma_channel_inst` as we tried — that collides).
#   3. Drives sensible AXI-Lite control defaults so the wrapper's slim
#      AXI-Lite is unused, OR fans-in the wrapper's tiny sub-window into
#      the IP's S_AXI_LITE control register block.
#   4. Tied-off / aggregated SG, AXI-burst, IRQ side-band per
#      DIR_IS_S2MM.
#
# That shim is ~200 LOC of plumbing and is OUT OF SCOPE for this task
# (which is forbidden from modifying rtl/* and capped at 90 minutes).
# It is the natural follow-up: a new "A1.b" sub-task under MIGRATION_PLAN.md
# §A1 deliverable "Vivado-IP-Tcl `rtl/infra/ip/axi_dma_*.tcl` (4×)"
# (line 203) — that line should be revised to also include
# `rtl/infra/ip/axi_dma_channel_inst.v` (the shim) since IP-Tcl alone
# cannot bridge the port-list mismatch.
#
# Until that shim lands, `make synth` exits 1 at synth_design. The IP
# itself IS created correctly under build/vivado/tetra_bs.gen/sources_1/
# ip/axi_dma_channel_inst/ and its OOC-synth run completes — only the
# top-level synth_design step fails when elaborating the wrapper.
#
# Repro:  `make synth`  → synth_failure.log line ~80 (Synth 8-7136).
# =============================================================================
puts "\[synth\] creating axi_dma:7.1 IP (renamed -> axi_dma_channel_inst)"
create_ip -name axi_dma -vendor xilinx.com -library ip -version 7.1 \
          -module_name axi_dma_channel_inst
set_property -dict [list \
    CONFIG.c_include_sg              {1} \
    CONFIG.c_sg_length_width         {14} \
    CONFIG.c_sg_include_stscntrl_strm {1} \
    CONFIG.c_include_mm2s            {1} \
    CONFIG.c_include_mm2s_dre        {0} \
    CONFIG.c_include_mm2s_sf         {1} \
    CONFIG.c_m_axi_mm2s_data_width   {32} \
    CONFIG.c_m_axis_mm2s_tdata_width {32} \
    CONFIG.c_include_s2mm            {1} \
    CONFIG.c_include_s2mm_dre        {1} \
    CONFIG.c_include_s2mm_sf         {1} \
    CONFIG.c_m_axi_s2mm_data_width   {32} \
    CONFIG.c_s_axis_s2mm_tdata_width {32} \
    CONFIG.c_s2mm_burst_size         {256} \
    CONFIG.c_addr_width              {32} \
    CONFIG.c_micro_dma               {0} \
    CONFIG.c_enable_multi_channel    {0} \
    CONFIG.c_increase_throughput     {0} \
] [get_ips axi_dma_channel_inst]

# Generate the IP's output products targeting synthesis/simulation; this
# materialises the synthesisable wrapper Verilog under
# build/vivado/${PROJ_NAME}.gen/sources_1/ip/axi_dma_channel_inst/.
generate_target {synthesis simulation} [get_ips axi_dma_channel_inst]
# OOC synth produces a checkpoint that's faster + more isolated.
catch { create_ip_run [get_ips axi_dma_channel_inst] }

# ---- RTL Sources ---------------------------------------------------------
# Every *.v under rtl/ EXCEPT rtl/_retired/. The retired path holds
# carry-over modules that do not belong in the new bitstream
# (per docs/MIGRATION_PLAN.md §"FPGA modules to delete").
#
# IMPORTANT: the simulation-only behavioural model at
# `tb/rtl/models/axi_dma_v7_1_bhv.v` defines a module also called
# `axi_dma_channel_inst`. It must NOT enter synth — only iverilog.
# Because this glob deliberately scans only `rtl/**` (and never `tb/**`),
# the bhv model is excluded by construction. The check below is a
# belt-and-braces guard against future refactors.
set RTL_FILES [list]
foreach f [glob -nocomplain "$REPO_ROOT/rtl/*.v" \
                            "$REPO_ROOT/rtl/infra/*.v" \
                            "$REPO_ROOT/rtl/infra/cdc/*.v" \
                            "$REPO_ROOT/rtl/lmac/*.v" \
                            "$REPO_ROOT/rtl/umac/*.v" \
                            "$REPO_ROOT/rtl/phy/*.v" ] {
    if {[string first "/_retired/" $f] >= 0} { continue }
    if {[string first "/tb/"        $f] >= 0} { continue }
    if {[string first "_bhv.v"      $f] >= 0} { continue }
    lappend RTL_FILES $f
}
puts "\[synth\] adding [llength $RTL_FILES] RTL files"
add_files -norecurse $RTL_FILES

# ---- Constraints ----------------------------------------------------------
set XDC "$REPO_ROOT/constraints/libresdr_tetra.xdc"
if {[file exists $XDC]} {
    add_files -fileset constrs_1 -norecurse $XDC
    puts "\[synth\] xdc: $XDC"
} else {
    puts "\[synth\] WARNING: $XDC not found — synth will run but P&R will lack pinout"
}

set_property top $TOP_MODULE [current_fileset]
update_compile_order -fileset sources_1

# ---- Synthesis ------------------------------------------------------------
puts "\[synth\] launching synth_1"
launch_runs synth_1 -jobs 4
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} {
    puts "\[synth\] FAIL synth_1 — see $BUILD_DIR/${PROJ_NAME}.runs/synth_1/runme.log"
    exit 1
}
open_run synth_1 -name synth_1
report_utilization -file $REPORT_DIR/utilization_synth.rpt
report_timing_summary -file $REPORT_DIR/timing_synth.rpt -warn_on_violation

# ---- Implementation -------------------------------------------------------
puts "\[synth\] launching impl_1 (P&R)"
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} {
    puts "\[synth\] FAIL impl_1 — see $BUILD_DIR/${PROJ_NAME}.runs/impl_1/runme.log"
    exit 1
}
open_run impl_1
report_utilization     -file $REPORT_DIR/utilization_impl.rpt
report_timing_summary  -file $REPORT_DIR/timing_impl.rpt -warn_on_violation
report_drc             -file $REPORT_DIR/drc.rpt

# ---- Outputs --------------------------------------------------------------
set BIT_SRC "$BUILD_DIR/${PROJ_NAME}.runs/impl_1/${TOP_MODULE}.bit"
set BIT_DST "$BUILD_DIR/${PROJ_NAME}.bit"
if {[file exists $BIT_SRC]} {
    file copy -force $BIT_SRC $BIT_DST
    puts "\[synth\] bitstream -> $BIT_DST"
} else {
    puts "\[synth\] FAIL — bitstream not produced at $BIT_SRC"
    exit 1
}

# bootgen → .bit.bin (consumed by Board #1's fpga_manager via /lib/firmware/)
set BIF_PATH "$BUILD_DIR/${PROJ_NAME}.bif"
set BIN_PATH "$BUILD_DIR/${PROJ_NAME}.bit.bin"
set bif [open $BIF_PATH w]
puts $bif "all:\n{\n    $BIT_DST\n}"
close $bif
if {[catch {exec bootgen -arch zynq -image $BIF_PATH -o $BIN_PATH -w on -process_bitstream bin} bg_err]} {
    puts "\[synth\] bootgen warning: $bg_err"
} else {
    puts "\[synth\] bit.bin    -> $BIN_PATH"
}

puts "============================================================"
puts " \[synth\] done"
puts " bit:    $BIT_DST"
puts " bit.bin: $BIN_PATH"
puts " reports: $REPORT_DIR/"
puts "============================================================"
exit 0
