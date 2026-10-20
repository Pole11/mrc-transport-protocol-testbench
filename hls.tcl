# Vitis HLS flow for the testbench, called by the Makefile (make csim / csynth / cosim).
#   STEP      csim   : C simulation of every test (same as make run, with the Vitis libraries)
#             csynth : synthesis of mrc_transport_protocol_top for each clock in TARGETS
#             cosim  : synthesis at 4 ns + C/RTL co-simulation, waveform in cosim_prj/solution1/sim/verilog
#   TB_DIR, NETSTACK: absolute paths of this repository and of the network stack hls folder

set TB_DIR   $::env(TB_DIR)
set NETSTACK $::env(NETSTACK)
set STEP     $::env(STEP)
set CFLAGS   "-I$TB_DIR/include -I$NETSTACK -D__gmp_const=const"
set PART     xcu55c-fsvh2892-2L-e
set DATA_WIDTH 512

set DUT_SRCS [list $NETSTACK/mrc_transport_protocol/mrc_transport_protocol.cpp \
                   $NETSTACK/mrc_transport_protocol/ib_utils.cpp \
                   $NETSTACK/axi_utils.cpp \
                   {*}[glob $TB_DIR/src/top/*.cpp]]
set TB_SRCS  [glob $TB_DIR/src/*.cpp $TB_DIR/src/integration/*.cpp $TB_DIR/src/unit/*.cpp]

open_project -reset ${STEP}_prj
set_top mrc_transport_protocol_top
foreach f $DUT_SRCS { add_files     $f -cflags $CFLAGS }
foreach f $TB_SRCS  { add_files -tb $f -cflags $CFLAGS }

if {$STEP == "csim"} {
    open_solution -reset solution1
    set_part $PART
    csim_design
}

if {$STEP == "csynth"} {
    # name  clock period [ns]  uncertainty [ns]: line rate = DATA_WIDTH * f_clk with II = 1
    set ALL_TARGETS { 100G 4.0 1.08  200G 2.5 0.68 }
    set results {}
    foreach {name period unc} $ALL_TARGETS {
        if {[lsearch -exact $::env(TARGETS) $name] < 0} { continue }
        open_solution -reset sol_$name
        set_part $PART
        create_clock -period $period -name default
        set_clock_uncertainty $unc default
        csynth_design

        set f [open ${STEP}_prj/sol_$name/syn/report/csynth.xml]; set txt [read $f]; close $f
        regexp {<TargetClockPeriod>([0-9.]+)</TargetClockPeriod>}       $txt -> tgt
        regexp {<EstimatedClockPeriod>([0-9.]+)</EstimatedClockPeriod>} $txt -> est
        regexp {<ClockUncertainty>([0-9.]+)</ClockUncertainty>}         $txt -> unc_v
        set slack [expr {$tgt - $unc_v - $est}]
        set mhz   [expr {1000.0 / $est}]
        lappend results [format "%-5s target %.2f ns  est %.3f ns  unc %.2f  slack %+.3f ns  fmax %.0f MHz  -> %.1f Gbps  %s" \
                             $name $tgt $est $unc_v $slack $mhz [expr {$DATA_WIDTH * $mhz / 1000.0}] [expr {$slack >= 0 ? "PASS" : "FAIL"}]]
    }
    puts "\n================ TIMING SUMMARY ================"
    foreach r $results { puts $r }
    puts "II / latency of each stage: build/hls/csynth_prj/sol_*/syn/report/mrc_transport_protocol_top_csynth.rpt"
    puts "================================================"
}

if {$STEP == "cosim"} {
    open_solution -reset solution1
    set_part $PART
    create_clock -period 4 -name default
    csynth_design
    # -trace_level port is lighter; drop -wave_debug on headless machines
    cosim_design -trace_level all -wave_debug -tool xsim
}

exit