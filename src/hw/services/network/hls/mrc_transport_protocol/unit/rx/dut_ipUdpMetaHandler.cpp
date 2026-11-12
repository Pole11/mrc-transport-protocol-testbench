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
#include "mrc_transport_protocol/rx/ipUdpMetaHandler.hpp"

// it receives (one of each per data packet, it waits for all three):
// 1. input             ipUdpMeta of the packet (their_address, their_port, my_port, length)
// 2. exHeaderInput     ExHeader from rx_process_exh (raw RETH + mrcExt)
// 3. dropMetaIn        fwdPolicy from rx_ibh_fsm (isDrop, ackOnly)
// it outputs, only if the packet is accepted (!isDrop && !ackOnly):
// 1. exh_lengthFifo    UDP length
// 2. exHeaderOutput    the ExHeader, unchanged

namespace
{
    #define IP_UDP_META_HANDLER_INSTID 2 // no static state, own instance like the other unit tests

    struct IpUdpMetaHandlerTestCase
    {
        // misc
        const char* name;

        // which inputs are written before the call
        // here you can differ the arrival of the different inputs
        bool sendUdpMeta;
        bool sendExHeader;
        bool sendPolicy;

        // input: fwdPolicy
        bool isDrop;
        bool ackOnly;

        // input: ipUdpMeta (address and ports are fixed, they are not forwarded)
        uint16_t length;

        // input: ExHeader
        ap_uint<128> reth;      // raw, the module does not look inside
        bool hasMeth;
        uint16_t rqmsn;
        uint16_t msn;
        bool hasTseth;
        uint16_t txTimestamp;
        bool tsr;
        bool hasImmDt;
        uint32_t immDt;

        // expected (when forwarded, length and ExHeader must be the inputs)
        bool exp_forwarded;
    };

    struct IpUdpMetaHandlerTestBench
    {
        hls::stream<ipUdpMeta>             input;
        hls::stream<ExHeader<DATA_WIDTH> > exHeaderInput;
        hls::stream<fwdPolicy>             dropMetaIn;

        hls::stream<ap_uint<16> >          exh_lengthFifo;
        hls::stream<ExHeader<DATA_WIDTH> > exHeaderOutput;

        void tickOneCycle()
        {
            ipUdpMetaHandler<DATA_WIDTH, IP_UDP_META_HANDLER_INSTID>(
                input,
                exHeaderInput,
                dropMetaIn,
                exh_lengthFifo,
                exHeaderOutput
            );
        }
    };

    ExHeader<DATA_WIDTH> buildExHeader(const IpUdpMetaHandlerTestCase& testCase)
    {
        ExHeader<DATA_WIDTH> header;
        header.setRawHeader(testCase.reth);
        header.mrc.hasMeth     = testCase.hasMeth;
        header.mrc.rqmsn       = testCase.rqmsn;
        header.mrc.msn         = testCase.msn;
        header.mrc.hasTseth    = testCase.hasTseth;
        header.mrc.txTimestamp = testCase.txTimestamp;
        header.mrc.tsr         = testCase.tsr;
        header.mrc.hasImmDt    = testCase.hasImmDt;
        header.mrc.immDt       = testCase.immDt;
        return header;
    }

    bool checkUdpMetaHandled(IpUdpMetaHandlerTestBench& tb, const IpUdpMetaHandlerTestCase& tc)
    {
        bool returnValue = true;
        auto fail = [&](const std::string& msg) {
            std::cerr << "[FAIL " << tc.name << "] " << msg << std::endl;
            returnValue = false;
        };
        auto expect = [&](const std::string& field, const ap_uint<128>& got, const ap_uint<128>& exp) {
            if (got != exp) fail(field + " " + got.to_string(16) + " != " + exp.to_string(16));
        };

        // one length and one ExHeader if the packet is accepted, nothing otherwise
        expect("exh_lengthFifo count", tb.exh_lengthFifo.size(), tc.exp_forwarded);
        expect("exHeaderOutput count", tb.exHeaderOutput.size(), tc.exp_forwarded);
        if (tc.exp_forwarded && !tb.exh_lengthFifo.empty())
            expect("length", tb.exh_lengthFifo.read(), tc.length);
        if (tc.exp_forwarded && !tb.exHeaderOutput.empty()) {
            ExHeader<DATA_WIDTH> h = tb.exHeaderOutput.read();
            expect("reth",            h.getRawHeader(),  tc.reth);
            expect("mrc.hasMeth",     h.mrc.hasMeth,     tc.hasMeth);
            expect("mrc.rqmsn",       h.mrc.rqmsn,       tc.rqmsn);
            expect("mrc.msn",         h.mrc.msn,         tc.msn);
            expect("mrc.hasTseth",    h.mrc.hasTseth,    tc.hasTseth);
            expect("mrc.txTimestamp", h.mrc.txTimestamp, tc.txTimestamp);
            expect("mrc.tsr",         h.mrc.tsr,         tc.tsr);
            expect("mrc.hasImmDt",    h.mrc.hasImmDt,    tc.hasImmDt);
            expect("mrc.immDt",       h.mrc.immDt,       tc.immDt);
        }

        // the three inputs are consumed together, or none of them is (the one that was sent is still there)
        const bool allSent = tc.sendUdpMeta && tc.sendExHeader && tc.sendPolicy;
        expect("input left",         tb.input.size(),         tc.sendUdpMeta  && !allSent);
        expect("exHeaderInput left", tb.exHeaderInput.size(), tc.sendExHeader && !allSent);
        expect("dropMetaIn left",    tb.dropMetaIn.size(),    tc.sendPolicy   && !allSent);

        // whatever is left must not leak into the next test case
        while (!tb.exh_lengthFifo.empty()) tb.exh_lengthFifo.read();
        while (!tb.exHeaderOutput.empty()) tb.exHeaderOutput.read();
        while (!tb.input.empty())          tb.input.read();
        while (!tb.exHeaderInput.empty())  tb.exHeaderInput.read();
        while (!tb.dropMetaIn.empty())     tb.dropMetaIn.read();

        return returnValue;
    }

    // rx_ibh_fsm policies: in window (false, false) accepted, duplicate (false, true) only re-acked,
    // out of window (true, false) dropped. Only accepted packets reach rx_exh_fsm, which reads one length and
    // one ExHeader per packet. Every row has different values, so an output left over by a previous row
    // cannot pass as this row's. Length never 0xC000 / 0x12B7 (their_port / my_port)
    const IpUdpMetaHandlerTestCase ipUdpMetaHandlerTestCases[] = {
        // the four policies, same kind of packet (WRITE ONLY, METH)
        {.name = "UDPMETA 01 accepted", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = false, .ackOnly = false, .length = 1068,
         .reth = ap_uint<128>("0x00000400123400000000000000001000"), .hasMeth = true, .rqmsn = 0x0001, .msn = 0x0002, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .immDt = 0,
         .exp_forwarded = true},
        {.name = "UDPMETA 02 duplicate (ackOnly)", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = false, .ackOnly = true, .length = 1069,
         .reth = ap_uint<128>("0x00000400123400000000000000002000"), .hasMeth = true, .rqmsn = 0x0003, .msn = 0x0004, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .immDt = 0,
         .exp_forwarded = false},
        {.name = "UDPMETA 03 out of window (drop)", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = true, .ackOnly = false, .length = 1070,
         .reth = ap_uint<128>("0x00000400123400000000000000003000"), .hasMeth = true, .rqmsn = 0x0005, .msn = 0x0006, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .immDt = 0,
         .exp_forwarded = false},
        // never produced by rx_ibh_fsm. CURRENT BEHAVIOUR: dropped
        {.name = "UDPMETA 04 drop and ackOnly", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = true, .ackOnly = true, .length = 1071,
         .reth = ap_uint<128>("0x00000400123400000000000000004000"), .hasMeth = true, .rqmsn = 0x0007, .msn = 0x0008, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .immDt = 0,
         .exp_forwarded = false},

        // what rx_process_exh can send: METH + TSETH, ImmDt, empty ExHeader (no RETH, NO_HEADER)
        {.name = "UDPMETA 05 TSETH", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = false, .ackOnly = false, .length = 1072,
         .reth = ap_uint<128>("0x00000834123400000000000000020000"), .hasMeth = true, .rqmsn = 0x0009, .msn = 0x000A, .hasTseth = true, .txTimestamp = 0xABCD, .tsr = true, .hasImmDt = false, .immDt = 0,
         .exp_forwarded = true},
        {.name = "UDPMETA 06 ImmDt", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = false, .ackOnly = false, .length = 344,
         .reth = ap_uint<128>("0x0000012C567800000000000000030000"), .hasMeth = true, .rqmsn = 0x000B, .msn = 0x000C, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = true, .immDt = 0xDEADBEEF,
         .exp_forwarded = true},
        {.name = "UDPMETA 07 empty ExHeader", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = false, .ackOnly = false, .length = 48,
         .reth = ap_uint<128>("0x0"), .hasMeth = false, .rqmsn = 0x0000, .msn = 0x0000, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .immDt = 0, .exp_forwarded = true},

        // extremes: length is forwarded as it is, not checked and not reduced (e.g. by UDP + BTH + ICRC)
        {.name = "UDPMETA 08 all ones", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = false, .ackOnly = false, .length = 0xFFFF,
         .reth = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .hasMeth = true, .rqmsn = 0xFFFF, .msn = 0xFFFF, .hasTseth = true, .txTimestamp = 0xFFFF, .tsr = true, .hasImmDt = true, .immDt = 0xFFFFFFFF,
         .exp_forwarded = true},
        {.name = "UDPMETA 09 all zero", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = false, .ackOnly = false, .length = 0,
         .reth = ap_uint<128>("0x0"), .hasMeth = false, .rqmsn = 0x0000, .msn = 0x0000, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .immDt = 0, .exp_forwarded = true},

        // every byte different (the whole 128-bit RETH is carried), then one flag at a time, so two swapped fields are caught
        {.name = "UDPMETA 10 distinct", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = false, .ackOnly = false, .length = 0x1B1C,
         .reth = ap_uint<128>("0x0102030405060708090A0B0C0D0E0F10"), .hasMeth = true, .rqmsn = 0x1112, .msn = 0x1314, .hasTseth = true, .txTimestamp = 0x1516, .tsr = true, .hasImmDt = true, .immDt = 0x1718191A,
         .exp_forwarded = true},
        {.name = "UDPMETA 11 hasMeth only", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = false, .ackOnly = false, .length = 101,
         .reth = ap_uint<128>("0x11"), .hasMeth = true, .rqmsn = 0x0021, .msn = 0x0022, .hasTseth = false, .txTimestamp = 0x0023, .tsr = false, .hasImmDt = false, .immDt = 0x00000024,
         .exp_forwarded = true},
        {.name = "UDPMETA 12 hasTseth only", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = false, .ackOnly = false, .length = 102,
         .reth = ap_uint<128>("0x12"), .hasMeth = false, .rqmsn = 0x0031, .msn = 0x0032, .hasTseth = true, .txTimestamp = 0x0033, .tsr = false, .hasImmDt = false, .immDt = 0x00000034,
         .exp_forwarded = true},
        {.name = "UDPMETA 13 tsr only", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = false, .ackOnly = false, .length = 103,
         .reth = ap_uint<128>("0x13"), .hasMeth = false, .rqmsn = 0x0041, .msn = 0x0042, .hasTseth = false, .txTimestamp = 0x0043, .tsr = true, .hasImmDt = false, .immDt = 0x00000044,
         .exp_forwarded = true},
        {.name = "UDPMETA 14 hasImmDt only", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = false, .ackOnly = false, .length = 104,
         .reth = ap_uint<128>("0x14"), .hasMeth = false, .rqmsn = 0x0051, .msn = 0x0052, .hasTseth = false, .txTimestamp = 0x0053, .tsr = false, .hasImmDt = true, .immDt = 0x00000054,
         .exp_forwarded = true},

        // no state between packets: rejected and accepted packets in any order
        {.name = "UDPMETA 15 drop", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = true, .ackOnly = false, .length = 201,
         .reth = ap_uint<128>("0x00000400AAAA00000000000000050000"), .hasMeth = true, .rqmsn = 0x0061, .msn = 0x0062, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .immDt = 0,
         .exp_forwarded = false},
        {.name = "UDPMETA 16 duplicate", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = false, .ackOnly = true, .length = 202,
         .reth = ap_uint<128>("0x00000400AAAA00000000000000050400"), .hasMeth = true, .rqmsn = 0x0061, .msn = 0x0063, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .immDt = 0,
         .exp_forwarded = false},
        {.name = "UDPMETA 17 accepted after rejected", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = false, .ackOnly = false, .length = 203,
         .reth = ap_uint<128>("0x00000400AAAA00000000000000050800"), .hasMeth = true, .rqmsn = 0x0061, .msn = 0x0064, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .immDt = 0,
         .exp_forwarded = true},
        {.name = "UDPMETA 18 accepted twice", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = false, .ackOnly = false, .length = 204,
         .reth = ap_uint<128>("0x00000400AAAA00000000000000050C00"), .hasMeth = true, .rqmsn = 0x0061, .msn = 0x0065, .hasTseth = true, .txTimestamp = 0x0066, .tsr = false, .hasImmDt = false, .immDt = 0,
         .exp_forwarded = true},

        // an input is missing: nothing is read (the others stay in their FIFO) and nothing is written, even if
        // the policy would accept the packet. rx_ibh_fsm (state table lookup) is usually the last one to arrive
        {.name = "UDPMETA 19 only ipUdpMeta", .sendUdpMeta = true, .sendExHeader = false, .sendPolicy = false,
         .isDrop = false, .ackOnly = false, .length = 301,
         .reth = ap_uint<128>("0x21"), .hasMeth = true, .rqmsn = 0x0071, .msn = 0x0072, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .immDt = 0, .exp_forwarded = false},
        {.name = "UDPMETA 20 only ExHeader", .sendUdpMeta = false, .sendExHeader = true, .sendPolicy = false,
         .isDrop = false, .ackOnly = false, .length = 302,
         .reth = ap_uint<128>("0x22"), .hasMeth = true, .rqmsn = 0x0073, .msn = 0x0074, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .immDt = 0, .exp_forwarded = false},
        {.name = "UDPMETA 21 only fwdPolicy", .sendUdpMeta = false, .sendExHeader = false, .sendPolicy = true,
         .isDrop = false, .ackOnly = false, .length = 303,
         .reth = ap_uint<128>("0x23"), .hasMeth = true, .rqmsn = 0x0075, .msn = 0x0076, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .immDt = 0, .exp_forwarded = false},
        // populate all the ports, except one
        {.name = "UDPMETA 22 no fwdPolicy", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = false,
         .isDrop = false, .ackOnly = false, .length = 304,
         .reth = ap_uint<128>("0x24"), .hasMeth = true, .rqmsn = 0x0077, .msn = 0x0078, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .immDt = 0, .exp_forwarded = false},
        {.name = "UDPMETA 23 no ExHeader", .sendUdpMeta = true, .sendExHeader = false, .sendPolicy = true,
         .isDrop = false, .ackOnly = false, .length = 305,
         .reth = ap_uint<128>("0x25"), .hasMeth = true, .rqmsn = 0x0079, .msn = 0x007A, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .immDt = 0, .exp_forwarded = false},
        {.name = "UDPMETA 24 no ipUdpMeta", .sendUdpMeta = false, .sendExHeader = true, .sendPolicy = true,
         .isDrop = false, .ackOnly = false, .length = 306,
         .reth = ap_uint<128>("0x26"), .hasMeth = true, .rqmsn = 0x007B, .msn = 0x007C, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .immDt = 0, .exp_forwarded = false},
        {.name = "UDPMETA 25 accepted after missing", .sendUdpMeta = true, .sendExHeader = true, .sendPolicy = true,
         .isDrop = false, .ackOnly = false, .length = 307,
         .reth = ap_uint<128>("0x27"), .hasMeth = true, .rqmsn = 0x007D, .msn = 0x007E, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .immDt = 0, .exp_forwarded = true},
    };
} // namespace

int runIpUdpMetaHandlerTest()
{
    IpUdpMetaHandlerTestBench tb;

    int failures = 0;
    const size_t testCaseCount = sizeof(ipUdpMetaHandlerTestCases) / sizeof(ipUdpMetaHandlerTestCases[0]);
    for (size_t tcIndex = 0; tcIndex < testCaseCount; ++tcIndex) {
        const IpUdpMetaHandlerTestCase& tc = ipUdpMetaHandlerTestCases[tcIndex];
        std::cout << "\n" << tc.name << "\n" << std::endl;

        if (tc.sendUdpMeta)  tb.input.write(ipUdpMeta(ap_uint<128>("0x0A000002"), 0xC000, RDMA_DEFAULT_PORT, tc.length));
        if (tc.sendExHeader) tb.exHeaderInput.write(buildExHeader(tc));
        if (tc.sendPolicy)   tb.dropMetaIn.write(fwdPolicy(tc.isDrop, tc.ackOnly));

        tb.tickOneCycle(); // one packet per call
        failures += !checkUdpMetaHandled(tb, tc);
    }

    return failures;
}