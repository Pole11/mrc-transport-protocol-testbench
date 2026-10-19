#include <iostream>

int runIbTransportProtocolTest();
int runRxProcessIbhTest();
int runSplitIpUdpMetaTest();
int runSplitCtrlTest();
int runStripMrcEthTest();

int main()
{
    const int ibTransportProtocolTestResult = runIbTransportProtocolTest();
    const int rxProcessIbhTestResult = runRxProcessIbhTest();
    const int runSplitIpUdpMetaTestResult = runSplitIpUdpMetaTest();
    const int runSplitCtrlTestResult = runSplitCtrlTest();
    const int runStripMrcEthTestResult = runStripMrcEthTest();

    if (ibTransportProtocolTestResult == 0)
        std::cout << "[PASS] ib_transport_protocol test\n";
    else
        std::cerr << "[FAIL] ib_transport_protocol test\n";

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


    return ibTransportProtocolTestResult || 
        rxProcessIbhTestResult || 
        runSplitIpUdpMetaTestResult || 
        runSplitCtrlTestResult ||
        runStripMrcEthTestResult;
}
