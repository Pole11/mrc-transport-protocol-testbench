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
#include "mrc_transport_protocol/rx/rx_split_udp_meta.hpp"

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
        hls::stream<ap_uint<16> > dataLenOut;
        hls::stream<ipUdpMeta> ctrlOut;

        void tickOneCycle() 
        {
            rx_split_udp_meta<RX_SPLIT_UDP_META_INSTID>(
                input,
                splitIn,
                dataOut,
                dataLenOut,
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
        if (tb.dataLenOut.size() != expData) fail("dataLenOut: expected " + std::to_string(expData) + " length, got " + std::to_string(tb.dataLenOut.size()));
        if (tb.ctrlOut.size() != expCtrl) fail("ctrlOut: expected " + std::to_string(expCtrl) + " meta, got " + std::to_string(tb.ctrlOut.size()));
        
        // the meta is forwarded unchanged
        if (expData && !tb.dataOut.empty()) {
            const ipUdpMeta m = tb.dataOut.read();
            expect("dataOut.their_address", m.their_address, tc.exp_data_their_address);
            expect("dataOut.their_port",    m.their_port,    tc.exp_data_their_port);
            expect("dataOut.my_port",       m.my_port,       tc.exp_data_my_port);
            expect("dataOut.length",        m.length,        tc.exp_data_length);
        }
        if (expData && !tb.dataLenOut.empty())
            expect("dataLenOut",            tb.dataLenOut.read(), tc.exp_data_length);
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
        while (!tb.dataOut.empty())     tb.dataOut.read();
        while (!tb.dataLenOut.empty())  tb.dataLenOut.read();
        while (!tb.ctrlOut.empty())     tb.ctrlOut.read();
        while (!tb.input.empty())       tb.input.read();
        while (!tb.splitIn.empty())     tb.splitIn.read();

        return returnValue;
    }

    // the expected meta of the unused output is all zeros (not checked). Every row has different values,
    // so a meta left over by a previous row cannot pass as this row's output
    const SplitIpUdpMetaTestCase splitIpUdpMetaTestCases[] = {
        // typical RoCE values: IPv4 peer in the low 32 bits, source port from the sender, local port 4791
        {.name = "SPLIT 01 data RoCE", .their_address = ap_uint<128>("0x0A000002"), .their_port = 0xC000, .my_port = 0x12B7, .length = 1080, .splitIn = false,
         .exp_data_their_address = ap_uint<128>("0x0A000002"), .exp_data_their_port = 0xC000, .exp_data_my_port = 0x12B7, .exp_data_length = 1080, .exp_ctrl_their_address = 0, .exp_ctrl_their_port = 0, .exp_ctrl_my_port = 0, .exp_ctrl_length = 0},
        {.name = "SPLIT 02 ctrl RoCE", .their_address = ap_uint<128>("0x0A000002"), .their_port = 0xC000, .my_port = 0x12B7, .length = 28, .splitIn = true,
         .exp_data_their_address = 0, .exp_data_their_port = 0, .exp_data_my_port = 0, .exp_data_length = 0, .exp_ctrl_their_address = ap_uint<128>("0x0A000002"), .exp_ctrl_their_port = 0xC000, .exp_ctrl_my_port = 0x12B7, .exp_ctrl_length = 28},

        // all fields zero / all fields at their maximum
        {.name = "SPLIT 03 data all zero", .their_address = 0, .their_port = 0x0000, .my_port = 0x0000, .length = 0x0000, .splitIn = false,
         .exp_data_their_address = 0, .exp_data_their_port = 0x0000, .exp_data_my_port = 0x0000, .exp_data_length = 0x0000, .exp_ctrl_their_address = 0, .exp_ctrl_their_port = 0, .exp_ctrl_my_port = 0, .exp_ctrl_length = 0},
        {.name = "SPLIT 04 ctrl all zero", .their_address = 0, .their_port = 0x0000, .my_port = 0x0000, .length = 0x0000, .splitIn = true,
         .exp_data_their_address = 0, .exp_data_their_port = 0, .exp_data_my_port = 0, .exp_data_length = 0, .exp_ctrl_their_address = 0, .exp_ctrl_their_port = 0x0000, .exp_ctrl_my_port = 0x0000, .exp_ctrl_length = 0x0000},
        {.name = "SPLIT 05 data all ones", .their_address = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .their_port = 0xFFFF, .my_port = 0xFFFF, .length = 0xFFFF, .splitIn = false,
         .exp_data_their_address = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .exp_data_their_port = 0xFFFF, .exp_data_my_port = 0xFFFF, .exp_data_length = 0xFFFF, .exp_ctrl_their_address = 0, .exp_ctrl_their_port = 0, .exp_ctrl_my_port = 0, .exp_ctrl_length = 0},
        {.name = "SPLIT 06 ctrl all ones", .their_address = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .their_port = 0xFFFF, .my_port = 0xFFFF, .length = 0xFFFF, .splitIn = true,
         .exp_data_their_address = 0, .exp_data_their_port = 0, .exp_data_my_port = 0, .exp_data_length = 0, .exp_ctrl_their_address = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .exp_ctrl_their_port = 0xFFFF, .exp_ctrl_my_port = 0xFFFF, .exp_ctrl_length = 0xFFFF},

        // the whole 128-bit address is carried, not only the low 32 or 64 bits
        {.name = "SPLIT 07 data IPv6", .their_address = ap_uint<128>("0x20010DB8000000000000000000000001"), .their_port = 0xC001, .my_port = 0x12B7, .length = 500, .splitIn = false,
         .exp_data_their_address = ap_uint<128>("0x20010DB8000000000000000000000001"), .exp_data_their_port = 0xC001, .exp_data_my_port = 0x12B7, .exp_data_length = 500, .exp_ctrl_their_address = 0, .exp_ctrl_their_port = 0, .exp_ctrl_my_port = 0, .exp_ctrl_length = 0},
        {.name = "SPLIT 08 ctrl IPv6", .their_address = ap_uint<128>("0xFE800000000000000202B3FFFE1E8329"), .their_port = 0xC002, .my_port = 0x12B7, .length = 48, .splitIn = true,
         .exp_data_their_address = 0, .exp_data_their_port = 0, .exp_data_my_port = 0, .exp_data_length = 0, .exp_ctrl_their_address = ap_uint<128>("0xFE800000000000000202B3FFFE1E8329"), .exp_ctrl_their_port = 0xC002, .exp_ctrl_my_port = 0x12B7, .exp_ctrl_length = 48},
        {.name = "SPLIT 09 data bit 127", .their_address = ap_uint<128>("0x80000000000000000000000000000000"), .their_port = 0x0001, .my_port = 0x0001, .length = 1, .splitIn = false,
         .exp_data_their_address = ap_uint<128>("0x80000000000000000000000000000000"), .exp_data_their_port = 0x0001, .exp_data_my_port = 0x0001, .exp_data_length = 1, .exp_ctrl_their_address = 0, .exp_ctrl_their_port = 0, .exp_ctrl_my_port = 0, .exp_ctrl_length = 0},
        {.name = "SPLIT 10 ctrl bit 64", .their_address = ap_uint<128>("0x00000000000000010000000000000000"), .their_port = 0x0002, .my_port = 0x0002, .length = 2, .splitIn = true,
         .exp_data_their_address = 0, .exp_data_their_port = 0, .exp_data_my_port = 0, .exp_data_length = 0, .exp_ctrl_their_address = ap_uint<128>("0x00000000000000010000000000000000"), .exp_ctrl_their_port = 0x0002, .exp_ctrl_my_port = 0x0002, .exp_ctrl_length = 2},
        {.name = "SPLIT 11 data bit 32", .their_address = ap_uint<128>("0x00000000000000000000000100000000"), .their_port = 0x0003, .my_port = 0x0003, .length = 3, .splitIn = false,
         .exp_data_their_address = ap_uint<128>("0x00000000000000000000000100000000"), .exp_data_their_port = 0x0003, .exp_data_my_port = 0x0003, .exp_data_length = 3, .exp_ctrl_their_address = 0, .exp_ctrl_their_port = 0, .exp_ctrl_my_port = 0, .exp_ctrl_length = 0},

        // every field different, so two swapped fields are caught
        {.name = "SPLIT 12 data distinct", .their_address = ap_uint<128>("0x00112233445566778899AABBCCDDEEFF"), .their_port = 0x1234, .my_port = 0x5678, .length = 0x9ABC, .splitIn = false,
         .exp_data_their_address = ap_uint<128>("0x00112233445566778899AABBCCDDEEFF"), .exp_data_their_port = 0x1234, .exp_data_my_port = 0x5678, .exp_data_length = 0x9ABC, .exp_ctrl_their_address = 0, .exp_ctrl_their_port = 0, .exp_ctrl_my_port = 0, .exp_ctrl_length = 0},
        {.name = "SPLIT 13 ctrl distinct", .their_address = ap_uint<128>("0xFFEEDDCCBBAA99887766554433221100"), .their_port = 0x4321, .my_port = 0x8765, .length = 0xCBA9, .splitIn = true,
         .exp_data_their_address = 0, .exp_data_their_port = 0, .exp_data_my_port = 0, .exp_data_length = 0, .exp_ctrl_their_address = ap_uint<128>("0xFFEEDDCCBBAA99887766554433221100"), .exp_ctrl_their_port = 0x4321, .exp_ctrl_my_port = 0x8765, .exp_ctrl_length = 0xCBA9},

        // the routing only depends on splitIn: two in a row on each side, then alternating
        {.name = "SPLIT 14 ctrl twice (1)", .their_address = ap_uint<128>("0x0A000003"), .their_port = 0xC010, .my_port = 0x12B7, .length = 36, .splitIn = true,
         .exp_data_their_address = 0, .exp_data_their_port = 0, .exp_data_my_port = 0, .exp_data_length = 0, .exp_ctrl_their_address = ap_uint<128>("0x0A000003"), .exp_ctrl_their_port = 0xC010, .exp_ctrl_my_port = 0x12B7, .exp_ctrl_length = 36},
        {.name = "SPLIT 15 ctrl twice (2)", .their_address = ap_uint<128>("0x0A000004"), .their_port = 0xC011, .my_port = 0x12B7, .length = 60, .splitIn = true,
         .exp_data_their_address = 0, .exp_data_their_port = 0, .exp_data_my_port = 0, .exp_data_length = 0, .exp_ctrl_their_address = ap_uint<128>("0x0A000004"), .exp_ctrl_their_port = 0xC011, .exp_ctrl_my_port = 0x12B7, .exp_ctrl_length = 60},
        {.name = "SPLIT 16 data twice (1)", .their_address = ap_uint<128>("0x0A000005"), .their_port = 0xC012, .my_port = 0x12B7, .length = 1060, .splitIn = false,
         .exp_data_their_address = ap_uint<128>("0x0A000005"), .exp_data_their_port = 0xC012, .exp_data_my_port = 0x12B7, .exp_data_length = 1060, .exp_ctrl_their_address = 0, .exp_ctrl_their_port = 0, .exp_ctrl_my_port = 0, .exp_ctrl_length = 0},
        {.name = "SPLIT 17 data twice (2)", .their_address = ap_uint<128>("0x0A000006"), .their_port = 0xC013, .my_port = 0x12B7, .length = 1064, .splitIn = false,
         .exp_data_their_address = ap_uint<128>("0x0A000006"), .exp_data_their_port = 0xC013, .exp_data_my_port = 0x12B7, .exp_data_length = 1064, .exp_ctrl_their_address = 0, .exp_ctrl_their_port = 0, .exp_ctrl_my_port = 0, .exp_ctrl_length = 0},
        {.name = "SPLIT 18 ctrl alternate", .their_address = ap_uint<128>("0x0A000007"), .their_port = 0xC014, .my_port = 0x12B7, .length = 40, .splitIn = true,
         .exp_data_their_address = 0, .exp_data_their_port = 0, .exp_data_my_port = 0, .exp_data_length = 0, .exp_ctrl_their_address = ap_uint<128>("0x0A000007"), .exp_ctrl_their_port = 0xC014, .exp_ctrl_my_port = 0x12B7, .exp_ctrl_length = 40},
        {.name = "SPLIT 19 data alternate", .their_address = ap_uint<128>("0x0A000008"), .their_port = 0xC015, .my_port = 0x12B7, .length = 100, .splitIn = false,
         .exp_data_their_address = ap_uint<128>("0x0A000008"), .exp_data_their_port = 0xC015, .exp_data_my_port = 0x12B7, .exp_data_length = 100, .exp_ctrl_their_address = 0, .exp_ctrl_their_port = 0, .exp_ctrl_my_port = 0, .exp_ctrl_length = 0},
        {.name = "SPLIT 20 ctrl alternate", .their_address = ap_uint<128>("0x0A000009"), .their_port = 0xC016, .my_port = 0x12B7, .length = 32, .splitIn = true,
         .exp_data_their_address = 0, .exp_data_their_port = 0, .exp_data_my_port = 0, .exp_data_length = 0, .exp_ctrl_their_address = ap_uint<128>("0x0A000009"), .exp_ctrl_their_port = 0xC016, .exp_ctrl_my_port = 0x12B7, .exp_ctrl_length = 32},
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