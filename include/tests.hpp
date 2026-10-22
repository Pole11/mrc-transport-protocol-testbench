#pragma once

// integration
int runMrcTransportProtocolTest();   // src/integration/dut_mrc_transport_protocol.cpp

// unit
int runRxProcessIbhTest();          // src/unit/dut_rx_process_ibh.cpp
int runSplitIpUdpMetaTest();        // src/unit/dut_rx_split_udp_meta.cpp
int runSplitCtrlTest();             // src/unit/dut_rx_split_ctrl.cpp
int runStripMrcEthTest();           // src/unit/dut_rx_strip_mrc_eth.cpp
int runRxIbhFsmTest();              // src/unit/dut_rx_ibh_fsm.cpp