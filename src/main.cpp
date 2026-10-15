#include <iostream>

int runIbTransportProtocolTest();
int runRxProcessIbhTest();

int main()
{
    const int ibTransportProtocolTestResult = runIbTransportProtocolTest();
    const int rxProcessIbhTestResult = runRxProcessIbhTest();

    if (ibTransportProtocolTestResult == 0)
        std::cout << "[PASS] ib_transport_protocol test\n";
    else
        std::cerr << "[FAIL] ib_transport_protocol test\n";

    if (rxProcessIbhTestResult == 0)
        std::cout << "[PASS] rx_process_ibh test\n";
    else
        std::cerr << "[FAIL] rx_process_ibh test\n";


    return ibTransportProtocolTestResult || rxProcessIbhTestResult;
}
