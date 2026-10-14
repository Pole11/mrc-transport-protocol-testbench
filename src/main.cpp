#include <iostream>

int runIbTransportProtocolTest();

int main()
{
    const int ibTransportProtocolTestResult = runIbTransportProtocolTest();
    if (ibTransportProtocolTestResult == 0)
        std::cout << "[PASS] ib_transport_protocol test\n";
    else
        std::cerr << "[FAIL] ib_transport_protocol test\n";

    return ibTransportProtocolTestResult;
}
