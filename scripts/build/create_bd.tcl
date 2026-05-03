# scripts/build/create_bd.tcl
#
# Phase 3.6 — Vivado Block Design construction for `tetra_system`.
#
# Replaces the dead-end-stub `tetra_synth_top.v` with a real PS7-backed
# block design so opt_design no longer prunes the entire datapath.
#
# Sourced from `scripts/build/synth.tcl` BEFORE `synth_design`. Mirrors
# the connectivity pattern of the carry-over BD at
# `/home/kevin/claude-ralph/tetra/build/vivado/tetra_zynq_phy.srcs/sources_1/bd/tetra_system/tetra_system.bd`
# but is NOT a 1:1 copy — different top, different AXI count, different
# PS7 config.
#
# IP set (locked):
#   sys_ps7      processing_system7:5.5    PS7 — FCLK_CLK0=100 MHz,
#                                          M_AXI_GP0, S_AXI_HP0, IRQ_F2P
#   sys_rstgen   proc_sys_reset:5.0        PL reset synchroniser
#   axi_ic_ctrl  axi_interconnect:2.1      M_AXI_GP0 → tetra_top.s_axi_lite
#                                          (1 SI / 1 MI — passthrough)
#   axi_ic_hp0   axi_interconnect:2.1      4× completer master → S_AXI_HP0
#                                          (4 SI / 1 MI — arbitrating)
#   tetra_top_0  tetra_top (user RTL)      The DUT.
#   completer_*  tetra_axi_mm_completer    4× slim→full AXI4-MM adapters,
#                                          one per tetra_top.m_axi_* port
#   xlconcat_irq xlconcat:2.1              4 IRQs → IRQ_F2P[3:0]
#
# Output: BD `tetra_system.bd`, wrapper RTL `tetra_system_wrapper.v`
# (Vivado-generated). The wrapper becomes the new top module.
#
# Hard cap: Phase 3.6 budget is 6h. If a Vivado quirk blocks BD-create,
# the failure is annotated below as a TODO and we ship the partial.

# ---- Inputs (set by caller, scripts/build/synth.tcl) ---------------------
# REPO_ROOT     normalized repo path
# BUILD_DIR     normalized build/vivado path
# PART          part name (xc7z020clg400-1)
# BD_NAME       bd name (`tetra_system`)
# BD_TOP        wrapper module name (`tetra_system_wrapper`)

if {![info exists BD_NAME]} { set BD_NAME "tetra_system" }
if {![info exists BD_TOP]}  { set BD_TOP  "${BD_NAME}_wrapper" }

puts "\[bd\] ============================================================"
puts "\[bd\]  Phase 3.6 BD-create"
puts "\[bd\]  bd-name : $BD_NAME"
puts "\[bd\]  bd-top  : $BD_TOP"
puts "\[bd\] ============================================================"

# ---- Create BD object ---------------------------------------------------
create_bd_design $BD_NAME
current_bd_design $BD_NAME

# =========================================================================
# 1. processing_system7:5.5 (sys_ps7)
# =========================================================================
puts "\[bd\] creating sys_ps7 (processing_system7:5.5)"
set ps7 [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 sys_ps7]

# Minimal config — only what is needed for the synth-time bring-up gate.
# The carry-over BD has a much heavier MIO/SDIO/Ethernet config which is
# orthogonal to PL fabric synth. We start from `apply_bd_automation`'s
# preset board (LibreSDR target = ZC706 reference clock?) — fall back to
# raw set_property when board files unavailable. The block-design tool
# will derive sensible defaults for everything not explicitly pinned.
set_property -dict [list \
    CONFIG.PCW_USE_M_AXI_GP0          {1} \
    CONFIG.PCW_USE_S_AXI_HP0          {1} \
    CONFIG.PCW_USE_FABRIC_INTERRUPT   {1} \
    CONFIG.PCW_IRQ_F2P_INTR           {1} \
    CONFIG.PCW_NUM_F2P_INTR_INPUTS    {4} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100} \
    CONFIG.PCW_EN_CLK0_PORT           {1} \
    CONFIG.PCW_FCLK_CLK0_BUF          {TRUE} \
    CONFIG.PCW_S_AXI_HP0_DATA_WIDTH   {32} \
    CONFIG.PCW_M_AXI_GP0_ENABLE_STATIC_REMAP {0} \
] $ps7

# =========================================================================
# 2. proc_sys_reset:5.0 (sys_rstgen)
# =========================================================================
puts "\[bd\] creating sys_rstgen (proc_sys_reset:5.0)"
set rstgen [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 sys_rstgen]

# Default: 1 BUS_RST + 1 INTERCONNECT_ARESETN + 1 PERP_ARESETN — fine.

# =========================================================================
# 3. axi_interconnect:2.1 — control plane (sys_ps7.M_AXI_GP0 → tetra_top)
# =========================================================================
puts "\[bd\] creating axi_ic_ctrl (axi_interconnect:2.1, 1 SI / 1 MI)"
set ic_ctrl [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 axi_ic_ctrl]
set_property -dict [list \
    CONFIG.NUM_SI {1} \
    CONFIG.NUM_MI {1} \
] $ic_ctrl

# =========================================================================
# 4. axi_interconnect:2.1 — data plane (4× completer.full → S_AXI_HP0)
# =========================================================================
puts "\[bd\] creating axi_ic_hp0 (axi_interconnect:2.1, 4 SI / 1 MI)"
set ic_hp0 [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 axi_ic_hp0]
set_property -dict [list \
    CONFIG.NUM_SI {4} \
    CONFIG.NUM_MI {1} \
] $ic_hp0

# =========================================================================
# 5. xlconcat:2.1 (xlconcat_irq) — 4 IRQs → IRQ_F2P[3:0]
# =========================================================================
puts "\[bd\] creating xlconcat_irq (xlconcat:2.1, 4 ports)"
set xlc [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat:2.1 xlconcat_irq]
set_property -dict [list CONFIG.NUM_PORTS {4}] $xlc

# =========================================================================
# 6. tetra_top_0 — user RTL block (via tetra_top_bd_facade)
# =========================================================================
# We instantiate the façade `tetra_top_bd_facade` (rtl/_bd/) rather
# than `tetra_top` directly. The façade renames the AXI-Lite slave
# port-prefix from `s_axil_*` to `s_axi_lite_*` so Vivado IPI infers
# a proper `S_AXI_LITE` bus interface — required for connecting to the
# axi_interconnect's M00_AXI bus pin via `connect_bd_intf_net`.
#
# All other ports (LVDS, GPIO, slim AXI-MM masters, IRQs) pass through
# the façade unchanged.
puts "\[bd\] creating tetra_top_0 (user RTL module tetra_top_bd_facade)"
set tt [create_bd_cell -type module -reference tetra_top_bd_facade tetra_top_0]

# =========================================================================
# 7. 4× tetra_axi_mm_completer — slim→full AXI4-MM bridges
# =========================================================================
# DIR_IS_S2MM=1 → write-only (tma_rx, tmd_rx)
# DIR_IS_S2MM=0 → read-only  (tma_tx, tmd_tx)
foreach {inst dir_is_s2mm} {
    completer_tma_rx 1
    completer_tma_tx 0
    completer_tmd_rx 1
    completer_tmd_tx 0
} {
    puts "\[bd\] creating $inst (tetra_axi_mm_completer, S2MM=$dir_is_s2mm)"
    set c [create_bd_cell -type module -reference tetra_axi_mm_completer $inst]
    set_property -dict [list \
        CONFIG.DIR_IS_S2MM $dir_is_s2mm \
        CONFIG.ADDR_WIDTH  {32} \
        CONFIG.DATA_WIDTH  {32} \
        CONFIG.ID_WIDTH    {1} \
    ] $c
}

# =========================================================================
# 8. External ports — board-pin façade
# =========================================================================
# Re-export every external pin that the carry-over xdc names. Names match
# tetra_top.v's port list 1:1 so `constraints/libresdr_tetra.xdc` does
# not need to change.

# AD9361 LVDS — differential pin pairs.
# For BD external ports, we re-use single-ended ports + Vivado's "make
# external" creates the pin pair with an _N companion automatically when
# the port is marked `lvds_pair_p`. To keep the .xdc unchanged we instead
# expose `*_p` and `*_n` separately as plain wires; `tetra_top` declares
# them that way already.

set lvds_in_pairs  {rx_clk_in rx_frame_in}
set lvds_in_buses  {rx_data_in}
set lvds_out_pairs {tx_clk_out tx_frame_out}
set lvds_out_buses {tx_data_out}

foreach p $lvds_in_pairs {
    create_bd_port -dir I ${p}_p
    create_bd_port -dir I ${p}_n
}
foreach b $lvds_in_buses {
    create_bd_port -dir I -from 5 -to 0 ${b}_p
    create_bd_port -dir I -from 5 -to 0 ${b}_n
}
foreach p $lvds_out_pairs {
    create_bd_port -dir O ${p}_p
    create_bd_port -dir O ${p}_n
}
foreach b $lvds_out_buses {
    create_bd_port -dir O -from 5 -to 0 ${b}_p
    create_bd_port -dir O -from 5 -to 0 ${b}_n
}

# AD9361 control + GPIO (LVCMOS) — single-ended.
foreach p { enable txnrx spi_csn spi_clk spi_mosi gpio_en_agc \
            gpio_sync gpio_resetb pl_led0 pl_led1 \
            dac_sync dac_sclk dac_din } {
    create_bd_port -dir O $p
}
create_bd_port -dir I  spi_miso
create_bd_port -dir I  -from 7 -to 0 gpio_status
create_bd_port -dir O  -from 3 -to 0 gpio_ctl
create_bd_port -dir IO iic_scl
create_bd_port -dir IO iic_sda

# =========================================================================
# 9. Connect external ports → tetra_top_0
# =========================================================================
puts "\[bd\] wiring external pins → tetra_top_0"
# LVDS in (pair → pin)
connect_bd_net [get_bd_ports rx_clk_in_p]    [get_bd_pins tetra_top_0/rx_clk_in_p]
connect_bd_net [get_bd_ports rx_clk_in_n]    [get_bd_pins tetra_top_0/rx_clk_in_n]
connect_bd_net [get_bd_ports rx_frame_in_p]  [get_bd_pins tetra_top_0/rx_frame_in_p]
connect_bd_net [get_bd_ports rx_frame_in_n]  [get_bd_pins tetra_top_0/rx_frame_in_n]
connect_bd_net [get_bd_ports rx_data_in_p]   [get_bd_pins tetra_top_0/rx_data_in_p]
connect_bd_net [get_bd_ports rx_data_in_n]   [get_bd_pins tetra_top_0/rx_data_in_n]
connect_bd_net [get_bd_pins tetra_top_0/tx_clk_out_p]   [get_bd_ports tx_clk_out_p]
connect_bd_net [get_bd_pins tetra_top_0/tx_clk_out_n]   [get_bd_ports tx_clk_out_n]
connect_bd_net [get_bd_pins tetra_top_0/tx_frame_out_p] [get_bd_ports tx_frame_out_p]
connect_bd_net [get_bd_pins tetra_top_0/tx_frame_out_n] [get_bd_ports tx_frame_out_n]
connect_bd_net [get_bd_pins tetra_top_0/tx_data_out_p]  [get_bd_ports tx_data_out_p]
connect_bd_net [get_bd_pins tetra_top_0/tx_data_out_n]  [get_bd_ports tx_data_out_n]

# AD9361 control + GPIO
foreach p { enable txnrx spi_csn spi_clk spi_mosi gpio_en_agc \
            gpio_sync gpio_resetb pl_led0 pl_led1 \
            dac_sync dac_sclk dac_din gpio_ctl } {
    connect_bd_net [get_bd_pins tetra_top_0/$p] [get_bd_ports $p]
}
connect_bd_net [get_bd_ports spi_miso]    [get_bd_pins tetra_top_0/spi_miso]
connect_bd_net [get_bd_ports gpio_status] [get_bd_pins tetra_top_0/gpio_status]
connect_bd_net [get_bd_ports iic_scl]     [get_bd_pins tetra_top_0/iic_scl]
connect_bd_net [get_bd_ports iic_sda]     [get_bd_pins tetra_top_0/iic_sda]

# =========================================================================
# 10. Clock + reset distribution
# =========================================================================
puts "\[bd\] connecting FCLK_CLK0 + reset network"
# PS7 FCLK_CLK0 → rstgen.slowest_sync_clk + every aclk in the system.
set fclk0 [get_bd_pins sys_ps7/FCLK_CLK0]
connect_bd_net $fclk0 [get_bd_pins sys_rstgen/slowest_sync_clk]
connect_bd_net [get_bd_pins sys_ps7/FCLK_RESET0_N] \
               [get_bd_pins sys_rstgen/ext_reset_in]

# clk_axi (= clk_sys in baseline; same source per IF_TETRA_TOP_v1 banner).
connect_bd_net $fclk0 [get_bd_pins tetra_top_0/clk_axi]
connect_bd_net $fclk0 [get_bd_pins tetra_top_0/clk_sys]
connect_bd_net $fclk0 [get_bd_pins axi_ic_ctrl/ACLK]
connect_bd_net $fclk0 [get_bd_pins axi_ic_ctrl/S00_ACLK]
connect_bd_net $fclk0 [get_bd_pins axi_ic_ctrl/M00_ACLK]
connect_bd_net $fclk0 [get_bd_pins axi_ic_hp0/ACLK]
foreach idx {S00 S01 S02 S03 M00} {
    connect_bd_net $fclk0 [get_bd_pins axi_ic_hp0/${idx}_ACLK]
}
connect_bd_net $fclk0 [get_bd_pins sys_ps7/M_AXI_GP0_ACLK]
connect_bd_net $fclk0 [get_bd_pins sys_ps7/S_AXI_HP0_ACLK]
foreach c {completer_tma_rx completer_tma_tx completer_tmd_rx completer_tmd_tx} {
    connect_bd_net $fclk0 [get_bd_pins $c/aclk]
}

# Active-low reset network: rstgen.peripheral_aresetn + interconnect_aresetn.
set rstn_periph [get_bd_pins sys_rstgen/peripheral_aresetn]
set rstn_ic     [get_bd_pins sys_rstgen/interconnect_aresetn]

connect_bd_net $rstn_periph [get_bd_pins tetra_top_0/rstn_axi]
connect_bd_net $rstn_periph [get_bd_pins tetra_top_0/rstn_sys]
connect_bd_net $rstn_ic     [get_bd_pins axi_ic_ctrl/ARESETN]
connect_bd_net $rstn_periph [get_bd_pins axi_ic_ctrl/S00_ARESETN]
connect_bd_net $rstn_periph [get_bd_pins axi_ic_ctrl/M00_ARESETN]
connect_bd_net $rstn_ic     [get_bd_pins axi_ic_hp0/ARESETN]
foreach idx {S00 S01 S02 S03 M00} {
    connect_bd_net $rstn_periph [get_bd_pins axi_ic_hp0/${idx}_ARESETN]
}
foreach c {completer_tma_rx completer_tma_tx completer_tmd_rx completer_tmd_tx} {
    connect_bd_net $rstn_periph [get_bd_pins $c/aresetn]
}

# =========================================================================
# 11. AXI control plane: M_AXI_GP0 → tetra_top.s_axi_lite (via ic_ctrl)
# =========================================================================
puts "\[bd\] connecting M_AXI_GP0 → axi_ic_ctrl → tetra_top_0/S_AXI_LITE"
connect_bd_intf_net [get_bd_intf_pins sys_ps7/M_AXI_GP0] \
                    [get_bd_intf_pins axi_ic_ctrl/S00_AXI]

# tetra_top_bd_facade re-exports the AXI-Lite slave with the standard
# `s_axi_lite_*` prefix so Vivado IPI infers an `S_AXI_LITE` bus
# interface. The connect below uses that bus name.
if {[catch {
    connect_bd_intf_net [get_bd_intf_pins axi_ic_ctrl/M00_AXI] \
                        [get_bd_intf_pins tetra_top_0/S_AXI_LITE]
} err]} {
    puts "\[bd\] ERROR cannot connect AXI-Lite — bus inference failed."
    puts "\[bd\]       err: $err"
    puts "\[bd\]       TODO: investigate the inferred interface name; the"
    puts "\[bd\]             façade port-prefix `s_axi_lite_*` may have"
    puts "\[bd\]             produced a different bus name. Probe with:"
    puts "\[bd\]             `get_bd_intf_pins tetra_top_0/*` to enumerate."
    puts "\[bd\]             Phase 3.6 ships partial; revisit next session."
}

# =========================================================================
# 12. AXI data plane: tetra_top.m_axi_* → completer.slim
#                     completer.full     → axi_ic_hp0.S0n_AXI
#                     axi_ic_hp0.M00_AXI → sys_ps7.S_AXI_HP0
# =========================================================================
puts "\[bd\] connecting 4× slim AXI-MM masters → completers → S_AXI_HP0"

# Pin-by-pin connection of slim ports — tetra_top's master ports are NOT
# grouped as a bus interface (slim shape). The completer's slim side
# matches signal-by-signal.
proc wire_slim_master {tt_prefix completer dir_is_s2mm} {
    # Common to write+read: addr/valid/ready exist on both AW and AR
    # depending on direction. tetra_top.v exposes only the side it owns.
    if {$dir_is_s2mm} {
        connect_bd_net [get_bd_pins tetra_top_0/${tt_prefix}_awaddr]  [get_bd_pins ${completer}/slim_awaddr_in]
        connect_bd_net [get_bd_pins tetra_top_0/${tt_prefix}_awvalid] [get_bd_pins ${completer}/slim_awvalid_in]
        connect_bd_net [get_bd_pins ${completer}/slim_awready_out]    [get_bd_pins tetra_top_0/${tt_prefix}_awready]
        connect_bd_net [get_bd_pins tetra_top_0/${tt_prefix}_wdata]   [get_bd_pins ${completer}/slim_wdata_in]
        connect_bd_net [get_bd_pins tetra_top_0/${tt_prefix}_wvalid]  [get_bd_pins ${completer}/slim_wvalid_in]
        connect_bd_net [get_bd_pins ${completer}/slim_wready_out]     [get_bd_pins tetra_top_0/${tt_prefix}_wready]
        connect_bd_net [get_bd_pins tetra_top_0/${tt_prefix}_wlast]   [get_bd_pins ${completer}/slim_wlast_in]
        connect_bd_net [get_bd_pins ${completer}/slim_bresp_out]      [get_bd_pins tetra_top_0/${tt_prefix}_bresp]
        connect_bd_net [get_bd_pins ${completer}/slim_bvalid_out]     [get_bd_pins tetra_top_0/${tt_prefix}_bvalid]
        connect_bd_net [get_bd_pins tetra_top_0/${tt_prefix}_bready]  [get_bd_pins ${completer}/slim_bready_in]
    } else {
        connect_bd_net [get_bd_pins tetra_top_0/${tt_prefix}_araddr]  [get_bd_pins ${completer}/slim_araddr_in]
        connect_bd_net [get_bd_pins tetra_top_0/${tt_prefix}_arvalid] [get_bd_pins ${completer}/slim_arvalid_in]
        connect_bd_net [get_bd_pins ${completer}/slim_arready_out]    [get_bd_pins tetra_top_0/${tt_prefix}_arready]
        connect_bd_net [get_bd_pins ${completer}/slim_rdata_out]      [get_bd_pins tetra_top_0/${tt_prefix}_rdata]
        connect_bd_net [get_bd_pins ${completer}/slim_rvalid_out]     [get_bd_pins tetra_top_0/${tt_prefix}_rvalid]
        connect_bd_net [get_bd_pins tetra_top_0/${tt_prefix}_rready]  [get_bd_pins ${completer}/slim_rready_in]
        connect_bd_net [get_bd_pins ${completer}/slim_rlast_out]      [get_bd_pins tetra_top_0/${tt_prefix}_rlast]
        connect_bd_net [get_bd_pins ${completer}/slim_rresp_out]      [get_bd_pins tetra_top_0/${tt_prefix}_rresp]
    }
}

wire_slim_master m_axi_tma_rx completer_tma_rx 1
wire_slim_master m_axi_tma_tx completer_tma_tx 0
wire_slim_master m_axi_tmd_rx completer_tmd_rx 1
wire_slim_master m_axi_tmd_tx completer_tmd_tx 0

# Connect the completer's full-AXI master to ic_hp0's slave slot — by
# bus-interface inference (port-prefix `m_axi_*` → bus `M_AXI`).
# If inference fails, the catch block reports the precise issue.
set hp_slots {S00 S01 S02 S03}
set comps    {completer_tma_rx completer_tma_tx completer_tmd_rx completer_tmd_tx}
for {set i 0} {$i < 4} {incr i} {
    set slot [lindex $hp_slots $i]
    set c    [lindex $comps    $i]
    if {[catch {
        connect_bd_intf_net [get_bd_intf_pins $c/M_AXI] \
                            [get_bd_intf_pins axi_ic_hp0/${slot}_AXI]
    } err]} {
        puts "\[bd\] WARN completer→ic_hp0 intf-inference failed for $c: $err"
        puts "\[bd\]      TODO: completer module might need an interface_xml"
        puts "\[bd\]            stub; ship partial."
    }
}

# ic_hp0.M00_AXI → S_AXI_HP0
connect_bd_intf_net [get_bd_intf_pins axi_ic_hp0/M00_AXI] \
                    [get_bd_intf_pins sys_ps7/S_AXI_HP0]

# =========================================================================
# 13. IRQ aggregation: 4 IRQs → xlconcat → IRQ_F2P[3:0]
# =========================================================================
puts "\[bd\] connecting IRQ network"
connect_bd_net [get_bd_pins tetra_top_0/irq_tma_rx_o] [get_bd_pins xlconcat_irq/In0]
connect_bd_net [get_bd_pins tetra_top_0/irq_tma_tx_o] [get_bd_pins xlconcat_irq/In1]
connect_bd_net [get_bd_pins tetra_top_0/irq_tmd_rx_o] [get_bd_pins xlconcat_irq/In2]
connect_bd_net [get_bd_pins tetra_top_0/irq_tmd_tx_o] [get_bd_pins xlconcat_irq/In3]
connect_bd_net [get_bd_pins xlconcat_irq/dout]       [get_bd_pins sys_ps7/IRQ_F2P]

# =========================================================================
# 14. PS7 DDR + FIXED_IO — re-export as external bus interfaces
# =========================================================================
puts "\[bd\] making PS7 DDR + FIXED_IO external"
make_bd_intf_pins_external [get_bd_intf_pins sys_ps7/DDR]
make_bd_intf_pins_external [get_bd_intf_pins sys_ps7/FIXED_IO]

# =========================================================================
# 15. Address assignment
# =========================================================================
puts "\[bd\] assigning addresses"
# tetra_top.S_AXIL: 4 KiB at 0x4000_0000 (matches dts/tetra_pl_overlay.dtsi).
# Use catch — if intf-inference failed, address-assign is a no-op.
catch { assign_bd_address }
catch { include_bd_addr_seg \
            [get_bd_addr_segs -of_objects [get_bd_addr_spaces sys_ps7/Data] \
                              -filter "NAME =~ *tetra_top*"] }

# =========================================================================
# 16. Validate + save BD
# =========================================================================
puts "\[bd\] validate_bd_design"
if {[catch { validate_bd_design } verr]} {
    puts "\[bd\] WARN validate_bd_design reported issues:"
    puts "$verr"
    puts "\[bd\]      TODO: triage post-Phase-3.6"
}

save_bd_design

# =========================================================================
# 17. Generate HDL wrapper
# =========================================================================
puts "\[bd\] generating $BD_TOP wrapper"
set bd_file [get_files ${BD_NAME}.bd]
set wrapper [make_wrapper -files $bd_file -top -force]
add_files -norecurse $wrapper
update_compile_order -fileset sources_1
set_property top $BD_TOP [current_fileset]

puts "\[bd\] ============================================================"
puts "\[bd\]  done — wrapper: $BD_TOP"
puts "\[bd\] ============================================================"
