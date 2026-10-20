XILINX_HLS ?= /tools/Xilinx/Vitis_HLS/2022.1
# change me
NETSTACK   ?= ../Coyote-mrc/hw/services/network/hls

DUT_SRCS := $(NETSTACK)/mrc_transport_protocol/mrc_transport_protocol.cpp \
            $(NETSTACK)/mrc_transport_protocol/ib_utils.cpp \
            $(NETSTACK)/axi_utils.cpp

SRCS := $(wildcard src/*/*.cpp) \
        $(wildcard src/*.cpp) \
        $(DUT_SRCS)

INCS := -Iinclude -I$(NETSTACK) -I$(XILINX_HLS)/include

LOG = run.log

# Vitis HLS: projects are created in build/hls, TARGETS selects the csynth clocks (100G, 200G)
VITIS_HLS ?= $(XILINX_HLS)/bin/vitis_hls
HLS_DIR   := build/hls
TARGETS   ?= 100G 200G
HLS        = mkdir -p $(HLS_DIR) && cd $(HLS_DIR) && \
             TB_DIR=$(CURDIR) NETSTACK=$(abspath $(NETSTACK)) TARGETS="$(TARGETS)" STEP=$(1) \
             $(VITIS_HLS) -f $(CURDIR)/hls.tcl -l $(1).log

main: $(SRCS) $(wildcard include/*.hpp)
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

clean:
	rm -rf main build

.PHONY: run csim csynth cosim clean
SHELL := /bin/bash