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
    puts "[synth] re-opening existing project $XPR_PATH"
    open_project $XPR_PATH
} else {
    puts "[synth] creating new project $XPR_PATH"
    create_project $PROJ_NAME $BUILD_DIR -part $PART -force
}
set_property target_language Verilog [current_project]
set_property default_lib work [current_project]

# ---- Sources --------------------------------------------------------------
# rtl_sources: every *.v under rtl/ EXCEPT rtl/_retired/. The retired path
# holds carry-over modules that do not belong in the new bitstream
# (per docs/MIGRATION_PLAN.md §"FPGA modules to delete").
set RTL_FILES [list]
foreach f [glob -nocomplain "$REPO_ROOT/rtl/*.v" \
                            "$REPO_ROOT/rtl/infra/*.v" \
                            "$REPO_ROOT/rtl/infra/cdc/*.v" \
                            "$REPO_ROOT/rtl/lmac/*.v" \
                            "$REPO_ROOT/rtl/umac/*.v" \
                            "$REPO_ROOT/rtl/phy/*.v" ] {
    if {[string first "/_retired/" $f] >= 0} { continue }
    lappend RTL_FILES $f
}
puts "[synth] adding [llength $RTL_FILES] RTL files"
add_files -norecurse $RTL_FILES

# ---- Constraints ----------------------------------------------------------
set XDC "$REPO_ROOT/constraints/libresdr_tetra.xdc"
if {[file exists $XDC]} {
    add_files -fileset constrs_1 -norecurse $XDC
    puts "[synth] xdc: $XDC"
} else {
    puts "[synth] WARNING: $XDC not found — synth will run but P&R will lack pinout"
}

set_property top $TOP_MODULE [current_fileset]
update_compile_order -fileset sources_1

# ---- Synthesis ------------------------------------------------------------
puts "[synth] launching synth_1"
launch_runs synth_1 -jobs 4
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} {
    puts "[synth] FAIL synth_1 — see $BUILD_DIR/${PROJ_NAME}.runs/synth_1/runme.log"
    exit 1
}
open_run synth_1 -name synth_1
report_utilization -file $REPORT_DIR/utilization_synth.rpt
report_timing_summary -file $REPORT_DIR/timing_synth.rpt -warn_on_violation

# ---- Implementation -------------------------------------------------------
puts "[synth] launching impl_1 (P&R)"
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} {
    puts "[synth] FAIL impl_1 — see $BUILD_DIR/${PROJ_NAME}.runs/impl_1/runme.log"
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
    puts "[synth] bitstream -> $BIT_DST"
} else {
    puts "[synth] FAIL — bitstream not produced at $BIT_SRC"
    exit 1
}

# bootgen → .bit.bin (consumed by Board #1's fpga_manager via /lib/firmware/)
set BIF_PATH "$BUILD_DIR/${PROJ_NAME}.bif"
set BIN_PATH "$BUILD_DIR/${PROJ_NAME}.bit.bin"
set bif [open $BIF_PATH w]
puts $bif "all:\n{\n    $BIT_DST\n}"
close $bif
if {[catch {exec bootgen -arch zynq -image $BIF_PATH -o $BIN_PATH -w on -process_bitstream bin} bg_err]} {
    puts "[synth] bootgen warning: $bg_err"
} else {
    puts "[synth] bit.bin    -> $BIN_PATH"
}

puts "============================================================"
puts " [synth] done"
puts " bit:    $BIT_DST"
puts " bit.bin: $BIN_PATH"
puts " reports: $REPORT_DIR/"
puts "============================================================"
exit 0
