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


    return mrcTransportProtocolTestResult || 
        rxProcessIbhTestResult || 
        runSplitIpUdpMetaTestResult || 
        runSplitCtrlTestResult ||
        runStripMrcEthTestResult ||
        runRxIbhFsmTestResult;
}
