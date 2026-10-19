#include <iostream>

int runIbTransportProtocolTest();
int runRxProcessIbhTest();
int runSplitIpUdpMetaTest();

int main()
{
    const int ibTransportProtocolTestResult = runIbTransportProtocolTest();
    const int rxProcessIbhTestResult = runRxProcessIbhTest();
    const int runSplitIpUdpMetaTestResult = runSplitIpUdpMetaTest();

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


    return ibTransportProtocolTestResult || rxProcessIbhTestResult || runSplitIpUdpMetaTestResult;
}
