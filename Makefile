XILINX_HLS ?= /tools/Xilinx/Vitis_HLS/2022.1
NETSTACK   := ../Coyote-mrc/hw/services/network/hls # change me

SRCS := $(wildcard src/*/*.cpp) \
        $(wildcard src/*.cpp) \
        $(NETSTACK)/ib_transport_protocol/ib_transport_protocol.cpp \
        $(NETSTACK)/ib_transport_protocol/ib_utils.cpp \
        $(NETSTACK)/axi_utils.cpp

INCS := -Iinclude -I$(NETSTACK) -I$(XILINX_HLS)/include

LOG = run.log

main: $(SRCS) $(wildcard include/*.hpp)
	g++ -std=c++14 -O1 -w $(INCS) $(SRCS) -o $@

run: main
	./main 2>&1 | tee $(LOG)

clean:
	rm -f main

.PHONY: run clean
SHELL := /bin/bash