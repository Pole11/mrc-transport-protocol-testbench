XILINX_HLS    ?= /tools/Xilinx/Vitis_HLS/2022.1
XILINX_VIVADO ?= /tools/Xilinx/Vivado/2022.1
# change me
COYOTE        ?= ../pole11.Coyote
NETSTACK      ?= $(COYOTE)/hw/services/network/hls# change me

DUT_SRCS := $(NETSTACK)/mrc_transport_protocol/mrc_transport_protocol.cpp \
            $(NETSTACK)/mrc_transport_protocol/ib_utils.cpp \
            $(NETSTACK)/axi_utils.cpp \
            $(NETSTACK)/rocev2/rocev2.cpp \
            $(NETSTACK)/ipv4/ipv4.cpp \
            $(NETSTACK)/ipv4/ipv4_utils.cpp \
            $(NETSTACK)/udp/udp.cpp \
            $(NETSTACK)/crc/crc.cpp

# tests mirror the Coyote tree: src/hw/... are the HLS tests, src/hdl/... the xsim testbenches
TB_HLS := src/hw/services/network/hls/mrc_transport_protocol
TB_HDL := src/hdl

SRCS := $(shell find src -name '*.cpp') \
        $(DUT_SRCS)

INCS := -Iinclude -I$(NETSTACK) -I$(XILINX_HLS)/include

LOG = run.log

# Vitis HLS: projects are created in build/hls, TARGETS selects the csynth clocks (100G, 200G)
VITIS_HLS ?= $(XILINX_HLS)/bin/vitis_hls
HLS_DIR   := build/hls
TARGETS   ?= 100G 200G
HLS        = mkdir -p $(HLS_DIR) && cd $(HLS_DIR) && \
             TB_DIR=$(CURDIR) TB_HLS=$(CURDIR)/$(TB_HLS) NETSTACK=$(abspath $(NETSTACK)) TARGETS="$(TARGETS)" STEP=$(1) \
             $(VITIS_HLS) -f $(CURDIR)/hls.tcl -l $(1).log

main: $(SRCS) $(wildcard include/*.hpp) $(wildcard $(NETSTACK)/mrc_transport_protocol/*.hpp $(NETSTACK)/mrc_transport_protocol/*/*.hpp)
	g++ -std=c++14 -O1 -w $(INCS) $(SRCS) -o $@

run: main
	set -o pipefail; ./main 2>&1 | tee $(LOG)

csim:
	$(call HLS,csim) | tee $(LOG)

csynth:
	$(call HLS,csynth) | tee $(LOG)

csynth-gui:
	vitis_hls -p build/hls/csynth_prj &

cosim:
	$(call HLS,cosim) | tee $(LOG)

# HDL tests with xsim, compiled in build/hdl. LYNX_PKG is the lynx_pkg.sv of a Coyote build with RDMA, written by make project
# (<build_dir>/<project>_shell/hdl/lynx_pkg.sv). The copy in build/hdl gets N_REGIONS = 3, whatever N_REGIONS the build had
LYNX_PKG   ?=
HDL_DIR    := build/hdl
COYOTE_HDL  = $(abspath $(COYOTE)/hw/hdl)
XSIM_BIN    = $(XILINX_VIVADO)/bin
HDL_TESTS   = 3

# $(call XSIM,top,sources): compile, elaborate and run one testbench
XSIM = cd $(abspath $(HDL_DIR)) && $(XSIM_BIN)/xvlog -sv -i $(COYOTE_HDL)/pkg lynx_pkg.sv $(COYOTE_HDL)/pkg/lynx_intf.sv $(2) && \
       $(XSIM_BIN)/xelab --timescale 1ns/1ps $(1) -s $(1) && $(XSIM_BIN)/xsim $(1) -R

hdl:
	@test -f "$(LYNX_PKG)" || { echo "LYNX_PKG must be a lynx_pkg.sv, e.g. make hdl LYNX_PKG=<build_dir>/<project>_shell/hdl/lynx_pkg.sv"; exit 1; }
	mkdir -p $(HDL_DIR)
	sed -E 's/(localparam integer N_REGIONS = )[0-9]+/\13/' $(LYNX_PKG) > $(HDL_DIR)/lynx_pkg.sv
	set -o pipefail; ( \
	  $(call XSIM,tb_rdma_imm_arbiter,$(COYOTE_HDL)/network/rdma/rdma_imm_arbiter.sv $(CURDIR)/src/hdl/network/rdma/tb_rdma_imm_arbiter.sv) && \
	  $(call XSIM,tb_rdma_imm_order,$(COYOTE_HDL)/common/queues/fifo.sv $(COYOTE_HDL)/common/queues/queue_meta.sv \
	    $(COYOTE_HDL)/user/cq/rdma_imm_order.sv $(CURDIR)/src/hdl/user/cq/tb_rdma_imm_order.sv) && \
	  $(call XSIM,tb_rdma_imm_csr,$(COYOTE_HDL)/pkg/axi_intf.sv $(COYOTE_HDL)/common/queues/fifo.sv $(COYOTE_HDL)/common/queues/queue_meta.sv \
	    $(COYOTE_HDL)/user/cq/rdma_imm_csr.sv $(CURDIR)/src/hdl/user/cq/tb_rdma_imm_csr.sv) \
	) | tee $(CURDIR)/$(LOG)
	test $$(grep -c '^PASS' $(LOG)) -eq $(HDL_TESTS)

all: cosim hdl

clean:
	rm -rf main build

.PHONY: all run csim csynth cosim hdl clean
SHELL := /bin/bash