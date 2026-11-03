#pragma once

// integration
int runMrcTransportProtocolTest();   // src/integration/dut_mrc_transport_protocol.cpp

// unit
int runRxProcessIbhTest();          // src/unit/rx/dut_rx_process_ibh.cpp
int runSplitIpUdpMetaTest();        // src/unit/rx/dut_rx_split_udp_meta.cpp
int runSplitCtrlTest();             // src/unit/rx/dut_rx_split_ctrl.cpp
int runStripMrcEthTest();           // src/unit/rx/dut_rx_strip_mrc_eth.cpp
int runRxIbhFsmTest();              // src/unit/rx/dut_rx_ibh_fsm.cpp
int runRxProcessExhTest();          // src/unit/rx/dut_rx_process_exh.cpp
int runRxProcessCtrlTest();         // src/unit/rx/dut_rx_process_ctrl.cpp
int runIpUdpMetaHandlerTest();      // src/unit/rx/dut_ip_udp_meta_handler.cpp
int runMergeRxPkgsTest();           // src/unit/rx/dut_merge_rx_pkgs.cpp
int runRxExhPayloadTest();          // src/unit/rx/dut_rx_exh_payload.cpp

int runStateTableTest();            // src/unit/tables/dut_state_table.cpp