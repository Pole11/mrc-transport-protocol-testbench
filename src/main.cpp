#include <iostream>

#include "tests.hpp"

int main()
{
    const int mrcTransportProtocolTestResult = runMrcTransportProtocolTest();
    const int rxProcessIbhTestResult = runRxProcessIbhTest();
    const int runSplitIpUdpMetaTestResult = runSplitIpUdpMetaTest();
    const int runSplitCtrlTestResult = runSplitCtrlTest();
    const int runStripMrcEthTestResult = runStripMrcEthTest();
    const int runRxIbhFsmTestResult = runRxIbhFsmTest();
    const int runRxProcessExhTestResult = runRxProcessExhTest();
    const int runRxProcessCtrlTestResult = runRxProcessCtrlTest();
    const int runIpUdpMetaHandlerTestResult = runIpUdpMetaHandlerTest();
    const int runStateTableTestResult = runStateTableTest();
    const int runMergeRxPkgsTestResult = runMergeRxPkgsTest();
    const int runRxExhPayloadTestResult = runRxExhPayloadTest();
    const int runDropOooIbhTestResult = runDropOooIbhTest();

    if (mrcTransportProtocolTestResult == 0)
        std::cout << "[PASS] mrc_transport_protocol test\n";
    else
        std::cerr << "[FAIL] mrc_transport_protocol test\n";

    if (rxProcessIbhTestResult == 0)
        std::cout << "[PASS] rx_process_ibh test\n";
    else
        std::cerr << "[FAIL] rx_process_ibh test\n";

    if (runSplitIpUdpMetaTestResult == 0)
        std::cout << "[PASS] rx_split_udp_meta test\n";
    else
        std::cerr << "[FAIL] rx_split_udp_meta test\n";

    if (runSplitCtrlTestResult == 0)
        std::cout << "[PASS] rx_split_ctrl test\n";
    else
        std::cerr << "[FAIL] rx_split_ctrl test\n";

    if (runStripMrcEthTestResult == 0)
        std::cout << "[PASS] rx_strip_mrc_eth test\n";
    else
        std::cerr << "[FAIL] rx_strip_mrc_eth test\n";

    if (runRxIbhFsmTestResult == 0)
        std::cout << "[PASS] rx_ibh_fsm test\n";
    else
        std::cerr << "[FAIL] rx_ibh_fsm test\n";

    if (runRxProcessExhTestResult == 0)
        std::cout << "[PASS] rx_process_exh test\n";
    else
        std::cerr << "[FAIL] rx_process_exh test\n";

    if (runRxProcessCtrlTestResult == 0)
        std::cout << "[PASS] rx_process_ctrl test\n";
    else
        std::cerr << "[FAIL] rx_process_ctrl test\n";

    if (runIpUdpMetaHandlerTestResult == 0)
        std::cout << "[PASS] ipUdpMetaHandler test\n";
    else
        std::cerr << "[FAIL] ipUdpMetaHandler test\n";

    if (runStateTableTestResult == 0)
        std::cout << "[PASS] state_table test\n";
    else
        std::cerr << "[FAIL] state_table test\n";

    if (runMergeRxPkgsTestResult == 0)
        std::cout << "[PASS] merge_rx_pkgs test\n";
    else
        std::cerr << "[FAIL] merge_rx_pkgs test\n";

    if (runRxExhPayloadTestResult == 0)
        std::cout << "[PASS] rx_exh_payload test\n";
    else
        std::cerr << "[FAIL] rx_exh_payload test\n";

    if (runDropOooIbhTestResult == 0)
        std::cout << "[PASS] drop_ooo_ibh test\n";
    else
        std::cerr << "[FAIL] drop_ooo_ibh test\n";

    return mrcTransportProtocolTestResult || 
        rxProcessIbhTestResult || 
        runSplitIpUdpMetaTestResult || 
        runSplitCtrlTestResult ||
        runStripMrcEthTestResult ||
        runRxIbhFsmTestResult ||
        runRxProcessExhTestResult ||
        runRxProcessCtrlTestResult || 
        runIpUdpMetaHandlerTestResult ||
        runStateTableTestResult ||
        runMergeRxPkgsTestResult ||
        runRxExhPayloadTestResult ||
        runDropOooIbhTestResult;
}
