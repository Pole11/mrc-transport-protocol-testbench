#include <cstdint>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "rocev2_config.hpp"
#include "util.hpp"
#include "tests.hpp"

#include "axi_utils.hpp"
#include "mrc_transport_protocol/rx/rx_common.hpp"

// this module must output the input ipUdpMeta (remote address, remote port, local port, length)
// 1. to dataOut if splitIn = False
// 2. to ctrlOut if splitIn = True

namespace 
{
    #define RX_SPLIT_UDP_META_INSTID 2 // own instance (mrc_transport_protocol uses 0 and 1)

    struct SplitIpUdpMetaTestCase 
    {
        // misc
        const char* name;

        // ipUdpMeta
        ap_uint<128> their_address;
        uint16_t their_port;
        uint16_t my_port;
        uint16_t	length;
        bool splitIn;

        // expected
        ap_uint<128> exp_data_their_address;
        uint16_t exp_data_their_port;
        uint16_t exp_data_my_port;
        uint16_t	exp_data_length;
        ap_uint<128> exp_ctrl_their_address;
        uint16_t exp_ctrl_their_port;
        uint16_t exp_ctrl_my_port;
        uint16_t	exp_ctrl_length;
    };

    struct RxUdpMetaTestBench
    {
        hls::stream<ipUdpMeta> input;
        hls::stream<bool>      splitIn;

        hls::stream<ipUdpMeta> dataOut;
        hls::stream<ipUdpMeta> ctrlOut;

        void tickOneCycle() 
        {
            rx_split_udp_meta<RX_SPLIT_UDP_META_INSTID>(
                input,
                splitIn,
                dataOut,
                ctrlOut
            );
        }
    };

    ipUdpMeta buildIpUdpMeta(const SplitIpUdpMetaTestCase& testCase)
    {
        return ipUdpMeta(testCase.their_address, testCase.their_port, testCase.my_port, testCase.length);
    }

    bool checkWhichOutputIsUsed(RxUdpMetaTestBench& tb, const SplitIpUdpMetaTestCase& tc) 
    {
        bool returnValue = true;
        auto fail = [&](const std::string& msg) {
            std::cerr << "[FAIL " << tc.name << "] " << msg << std::endl;
            returnValue = false;
        };
        auto expect = [&](const std::string& field, const ap_uint<128>& got, const ap_uint<128>& exp) {
            if (got != exp) fail(field + " " + got.to_string(16) + " != " + exp.to_string(16));
        };

        // exactly one meta, on the output chosen by splitIn, none on the other
        const size_t expData = tc.splitIn ? 0 : 1;
        const size_t expCtrl = tc.splitIn ? 1 : 0;
        if (tb.dataOut.size() != expData) fail("dataOut: expected " + std::to_string(expData) + " meta, got " + std::to_string(tb.dataOut.size()));
        if (tb.ctrlOut.size() != expCtrl) fail("ctrlOut: expected " + std::to_string(expCtrl) + " meta, got " + std::to_string(tb.ctrlOut.size()));

        // the meta is forwarded unchanged
        if (expData && !tb.dataOut.empty()) {
            const ipUdpMeta m = tb.dataOut.read();
            expect("dataOut.their_address", m.their_address, tc.exp_data_their_address);
            expect("dataOut.their_port",    m.their_port,    tc.exp_data_their_port);
            expect("dataOut.my_port",       m.my_port,       tc.exp_data_my_port);
            expect("dataOut.length",        m.length,        tc.exp_data_length);
        }
        if (expCtrl && !tb.ctrlOut.empty()) {
            const ipUdpMeta m = tb.ctrlOut.read();
            expect("ctrlOut.their_address", m.their_address, tc.exp_ctrl_their_address);
            expect("ctrlOut.their_port",    m.their_port,    tc.exp_ctrl_their_port);
            expect("ctrlOut.my_port",       m.my_port,       tc.exp_ctrl_my_port);
            expect("ctrlOut.length",        m.length,        tc.exp_ctrl_length);
        }

        // both inputs consumed
        if (!tb.input.empty())   fail("input meta not consumed");
        if (!tb.splitIn.empty()) fail("splitIn flag not consumed");

        // whatever is left (wrong counts above) must not leak into the next test case
        while (!tb.dataOut.empty()) tb.dataOut.read();
        while (!tb.ctrlOut.empty()) tb.ctrlOut.read();
        while (!tb.input.empty())   tb.input.read();
        while (!tb.splitIn.empty()) tb.splitIn.read();

        return returnValue;
    }

    // the expected meta of the unused output is all zeros (not checked). Every row has different values,
    // so a meta left over by a previous row cannot pass as this row's output
    const SplitIpUdpMetaTestCase splitIpUdpMetaTestCases[] = {
        // typical RoCE values: IPv4 peer in the low 32 bits, source port from the sender, local port 4791
        {"SPLIT 01 data RoCE",          ap_uint<128>("0x0A000002"), 0xC000, 0x12B7, 1080,   false, ap_uint<128>("0x0A000002"), 0xC000, 0x12B7, 1080,   0, 0, 0, 0},
        {"SPLIT 02 ctrl RoCE",          ap_uint<128>("0x0A000002"), 0xC000, 0x12B7, 28,     true,  0, 0, 0, 0,     ap_uint<128>("0x0A000002"), 0xC000, 0x12B7, 28},

        // all fields zero / all fields at their maximum
        {"SPLIT 03 data all zero",      0, 0x0000, 0x0000, 0x0000, false, 0, 0x0000, 0x0000, 0x0000, 0, 0, 0, 0},
        {"SPLIT 04 ctrl all zero",      0, 0x0000, 0x0000, 0x0000, true,  0, 0, 0, 0, 0, 0x0000, 0x0000, 0x0000},
        {"SPLIT 05 data all ones",      ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), 0xFFFF, 0xFFFF, 0xFFFF, false, ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0, 0},
        {"SPLIT 06 ctrl all ones",      ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), 0xFFFF, 0xFFFF, 0xFFFF, true,  0, 0, 0, 0, ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), 0xFFFF, 0xFFFF, 0xFFFF},

        // the whole 128-bit address is carried, not only the low 32 or 64 bits
        {"SPLIT 07 data IPv6",          ap_uint<128>("0x20010DB8000000000000000000000001"), 0xC001, 0x12B7, 500, false, ap_uint<128>("0x20010DB8000000000000000000000001"), 0xC001, 0x12B7, 500, 0, 0, 0, 0},
        {"SPLIT 08 ctrl IPv6",          ap_uint<128>("0xFE800000000000000202B3FFFE1E8329"), 0xC002, 0x12B7, 48,  true,  0, 0, 0, 0, ap_uint<128>("0xFE800000000000000202B3FFFE1E8329"), 0xC002, 0x12B7, 48},
        {"SPLIT 09 data bit 127",       ap_uint<128>("0x80000000000000000000000000000000"), 0x0001, 0x0001, 1,   false, ap_uint<128>("0x80000000000000000000000000000000"), 0x0001, 0x0001, 1,   0, 0, 0, 0},
        {"SPLIT 10 ctrl bit 64",        ap_uint<128>("0x00000000000000010000000000000000"), 0x0002, 0x0002, 2,   true,  0, 0, 0, 0, ap_uint<128>("0x00000000000000010000000000000000"), 0x0002, 0x0002, 2},
        {"SPLIT 11 data bit 32",        ap_uint<128>("0x00000000000000000000000100000000"), 0x0003, 0x0003, 3,   false, ap_uint<128>("0x00000000000000000000000100000000"), 0x0003, 0x0003, 3,   0, 0, 0, 0},

        // every field different, so two swapped fields are caught
        {"SPLIT 12 data distinct",      ap_uint<128>("0x00112233445566778899AABBCCDDEEFF"), 0x1234, 0x5678, 0x9ABC, false, ap_uint<128>("0x00112233445566778899AABBCCDDEEFF"), 0x1234, 0x5678, 0x9ABC, 0, 0, 0, 0},
        {"SPLIT 13 ctrl distinct",      ap_uint<128>("0xFFEEDDCCBBAA99887766554433221100"), 0x4321, 0x8765, 0xCBA9, true,  0, 0, 0, 0, ap_uint<128>("0xFFEEDDCCBBAA99887766554433221100"), 0x4321, 0x8765, 0xCBA9},

        // the routing only depends on splitIn: two in a row on each side, then alternating
        {"SPLIT 14 ctrl twice (1)",     ap_uint<128>("0x0A000003"), 0xC010, 0x12B7, 36,   true,  0, 0, 0, 0, ap_uint<128>("0x0A000003"), 0xC010, 0x12B7, 36},
        {"SPLIT 15 ctrl twice (2)",     ap_uint<128>("0x0A000004"), 0xC011, 0x12B7, 60,   true,  0, 0, 0, 0, ap_uint<128>("0x0A000004"), 0xC011, 0x12B7, 60},
        {"SPLIT 16 data twice (1)",     ap_uint<128>("0x0A000005"), 0xC012, 0x12B7, 1060, false, ap_uint<128>("0x0A000005"), 0xC012, 0x12B7, 1060, 0, 0, 0, 0},
        {"SPLIT 17 data twice (2)",     ap_uint<128>("0x0A000006"), 0xC013, 0x12B7, 1064, false, ap_uint<128>("0x0A000006"), 0xC013, 0x12B7, 1064, 0, 0, 0, 0},
        {"SPLIT 18 ctrl alternate",     ap_uint<128>("0x0A000007"), 0xC014, 0x12B7, 40,   true,  0, 0, 0, 0, ap_uint<128>("0x0A000007"), 0xC014, 0x12B7, 40},
        {"SPLIT 19 data alternate",     ap_uint<128>("0x0A000008"), 0xC015, 0x12B7, 100,  false, ap_uint<128>("0x0A000008"), 0xC015, 0x12B7, 100,  0, 0, 0, 0},
        {"SPLIT 20 ctrl alternate",     ap_uint<128>("0x0A000009"), 0xC016, 0x12B7, 32,   true,  0, 0, 0, 0, ap_uint<128>("0x0A000009"), 0xC016, 0x12B7, 32},
    };
} // namespace

int runSplitIpUdpMetaTest() 
{
    RxUdpMetaTestBench tb;

    int failures = 0;
    const size_t testCaseCount = sizeof(splitIpUdpMetaTestCases) / sizeof(splitIpUdpMetaTestCases[0]);
    for (size_t tcIndex = 0; tcIndex < testCaseCount; ++tcIndex) {
        const SplitIpUdpMetaTestCase& tc = splitIpUdpMetaTestCases[tcIndex];
        std::cout << "\n" << tc.name << "\n" << std::endl;

        tb.input.write(buildIpUdpMeta(tc));
        tb.splitIn.write(tc.splitIn);

        tb.tickOneCycle(); // one meta per call
        failures += !checkWhichOutputIsUsed(tb, tc);
    }

    return failures;
}