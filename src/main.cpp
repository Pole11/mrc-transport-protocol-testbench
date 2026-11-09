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
    const int runImmTableTestResult = runImmTableTest();

    std::cout << std::endl << "-- RECAP --" << std::endl;

    if (mrcTransportProtocolTestResult == 0)
        std::cout << "[PASS] mrc_transport_protocol test" << std::endl;
    else
        std::cerr << "[FAIL] mrc_transport_protocol test" << std::endl;

    if (rxProcessIbhTestResult == 0)
        std::cout << "[PASS] rx_process_ibh test" << std::endl;
    else
        std::cerr << "[FAIL] rx_process_ibh test" << std::endl;

    if (runSplitIpUdpMetaTestResult == 0)
        std::cout << "[PASS] rx_split_udp_meta test" << std::endl;
    else
        std::cerr << "[FAIL] rx_split_udp_meta test" << std::endl;

    if (runSplitCtrlTestResult == 0)
        std::cout << "[PASS] rx_split_ctrl test" << std::endl;
    else
        std::cerr << "[FAIL] rx_split_ctrl test" << std::endl;

    if (runStripMrcEthTestResult == 0)
        std::cout << "[PASS] rx_strip_mrc_eth test" << std::endl;
    else
        std::cerr << "[FAIL] rx_strip_mrc_eth test" << std::endl;

    if (runRxIbhFsmTestResult == 0)
        std::cout << "[PASS] rx_ibh_fsm test" << std::endl;
    else
        std::cerr << "[FAIL] rx_ibh_fsm test" << std::endl;

    if (runRxProcessExhTestResult == 0)
        std::cout << "[PASS] rx_process_exh test" << std::endl;
    else
        std::cerr << "[FAIL] rx_process_exh test" << std::endl;

    if (runRxProcessCtrlTestResult == 0)
        std::cout << "[PASS] rx_process_ctrl test" << std::endl;
    else
        std::cerr << "[FAIL] rx_process_ctrl test" << std::endl;

    if (runIpUdpMetaHandlerTestResult == 0)
        std::cout << "[PASS] ipUdpMetaHandler test" << std::endl;
    else
        std::cerr << "[FAIL] ipUdpMetaHandler test" << std::endl;

    if (runStateTableTestResult == 0)
        std::cout << "[PASS] state_table test" << std::endl;
    else
        std::cerr << "[FAIL] state_table test" << std::endl;

    if (runMergeRxPkgsTestResult == 0)
        std::cout << "[PASS] merge_rx_pkgs test" << std::endl;
    else
        std::cerr << "[FAIL] merge_rx_pkgs test" << std::endl;

    if (runRxExhPayloadTestResult == 0)
        std::cout << "[PASS] rx_exh_payload test" << std::endl;
    else
        std::cerr << "[FAIL] rx_exh_payload test" << std::endl;

    if (runDropOooIbhTestResult == 0)
        std::cout << "[PASS] drop_ooo_ibh test" << std::endl;
    else
        std::cerr << "[FAIL] drop_ooo_ibh test" << std::endl;

    if (runImmTableTestResult == 0)
        std::cout << "[PASS] imm_table test" << std::endl;
    else
        std::cerr << "[FAIL] imm_table test" << std::endl;

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
        runDropOooIbhTestResult ||
        runImmTableTestResult;
}
