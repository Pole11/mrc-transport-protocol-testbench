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
#include "mrc_transport_protocol/rx/rx_data_res.hpp"

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
        {"UDPMETA 01 accepted",               true,  true,  true,  false, false, 1068,   ap_uint<128>("0x00000400123400000000000000001000"), true,  0x0001, 0x0002, false, 0,      false, false, 0,          true},
        {"UDPMETA 02 duplicate (ackOnly)",    true,  true,  true,  false, true,  1069,   ap_uint<128>("0x00000400123400000000000000002000"), true,  0x0003, 0x0004, false, 0,      false, false, 0,          false},
        {"UDPMETA 03 out of window (drop)",   true,  true,  true,  true,  false, 1070,   ap_uint<128>("0x00000400123400000000000000003000"), true,  0x0005, 0x0006, false, 0,      false, false, 0,          false},
        // never produced by rx_ibh_fsm. CURRENT BEHAVIOUR: dropped
        {"UDPMETA 04 drop and ackOnly",       true,  true,  true,  true,  true,  1071,   ap_uint<128>("0x00000400123400000000000000004000"), true,  0x0007, 0x0008, false, 0,      false, false, 0,          false},

        // what rx_process_exh can send: METH + TSETH, ImmDt, empty ExHeader (no RETH, NO_HEADER)
        {"UDPMETA 05 TSETH",                  true,  true,  true,  false, false, 1072,   ap_uint<128>("0x00000834123400000000000000020000"), true,  0x0009, 0x000A, true,  0xABCD, true,  false, 0,          true},
        {"UDPMETA 06 ImmDt",                  true,  true,  true,  false, false, 344,    ap_uint<128>("0x0000012C567800000000000000030000"), true,  0x000B, 0x000C, false, 0,      false, true,  0xDEADBEEF, true},
        {"UDPMETA 07 empty ExHeader",         true,  true,  true,  false, false, 48,     ap_uint<128>("0x0"),                                false, 0x0000, 0x0000, false, 0,      false, false, 0,          true},

        // extremes: length is forwarded as it is, not checked and not reduced (e.g. by UDP + BTH + ICRC)
        {"UDPMETA 08 all ones",               true,  true,  true,  false, false, 0xFFFF, ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), true,  0xFFFF, 0xFFFF, true,  0xFFFF, true,  true,  0xFFFFFFFF, true},
        {"UDPMETA 09 all zero",               true,  true,  true,  false, false, 0,      ap_uint<128>("0x0"),                                false, 0x0000, 0x0000, false, 0,      false, false, 0,          true},

        // every byte different (the whole 128-bit RETH is carried), then one flag at a time, so two swapped fields are caught
        {"UDPMETA 10 distinct",               true,  true,  true,  false, false, 0x1B1C, ap_uint<128>("0x0102030405060708090A0B0C0D0E0F10"), true,  0x1112, 0x1314, true,  0x1516, true,  true,  0x1718191A, true},
        {"UDPMETA 11 hasMeth only",           true,  true,  true,  false, false, 101,    ap_uint<128>("0x11"),                               true,  0x0021, 0x0022, false, 0x0023, false, false, 0x00000024, true},
        {"UDPMETA 12 hasTseth only",          true,  true,  true,  false, false, 102,    ap_uint<128>("0x12"),                               false, 0x0031, 0x0032, true,  0x0033, false, false, 0x00000034, true},
        {"UDPMETA 13 tsr only",               true,  true,  true,  false, false, 103,    ap_uint<128>("0x13"),                               false, 0x0041, 0x0042, false, 0x0043, true,  false, 0x00000044, true},
        {"UDPMETA 14 hasImmDt only",          true,  true,  true,  false, false, 104,    ap_uint<128>("0x14"),                               false, 0x0051, 0x0052, false, 0x0053, false, true,  0x00000054, true},

        // no state between packets: rejected and accepted packets in any order
        {"UDPMETA 15 drop",                   true,  true,  true,  true,  false, 201,    ap_uint<128>("0x00000400AAAA00000000000000050000"), true,  0x0061, 0x0062, false, 0,      false, false, 0,          false},
        {"UDPMETA 16 duplicate",              true,  true,  true,  false, true,  202,    ap_uint<128>("0x00000400AAAA00000000000000050400"), true,  0x0061, 0x0063, false, 0,      false, false, 0,          false},
        {"UDPMETA 17 accepted after rejected",true,  true,  true,  false, false, 203,    ap_uint<128>("0x00000400AAAA00000000000000050800"), true,  0x0061, 0x0064, false, 0,      false, false, 0,          true},
        {"UDPMETA 18 accepted twice",         true,  true,  true,  false, false, 204,    ap_uint<128>("0x00000400AAAA00000000000000050C00"), true,  0x0061, 0x0065, true,  0x0066, false, false, 0,          true},

        // an input is missing: nothing is read (the others stay in their FIFO) and nothing is written, even if
        // the policy would accept the packet. rx_ibh_fsm (state table lookup) is usually the last one to arrive
        {"UDPMETA 19 only ipUdpMeta",         true,  false, false, false, false, 301,    ap_uint<128>("0x21"),                               true,  0x0071, 0x0072, false, 0,      false, false, 0,          false},
        {"UDPMETA 20 only ExHeader",          false, true,  false, false, false, 302,    ap_uint<128>("0x22"),                               true,  0x0073, 0x0074, false, 0,      false, false, 0,          false},
        {"UDPMETA 21 only fwdPolicy",         false, false, true,  false, false, 303,    ap_uint<128>("0x23"),                               true,  0x0075, 0x0076, false, 0,      false, false, 0,          false},
        // populate all the ports, except one
        {"UDPMETA 22 no fwdPolicy",           true,  true,  false, false, false, 304,    ap_uint<128>("0x24"),                               true,  0x0077, 0x0078, false, 0,      false, false, 0,          false},
        {"UDPMETA 23 no ExHeader",            true,  false, true,  false, false, 305,    ap_uint<128>("0x25"),                               true,  0x0079, 0x007A, false, 0,      false, false, 0,          false},
        {"UDPMETA 24 no ipUdpMeta",           false, true,  true,  false, false, 306,    ap_uint<128>("0x26"),                               true,  0x007B, 0x007C, false, 0,      false, false, 0,          false},
        {"UDPMETA 25 accepted after missing", true,  true,  true,  false, false, 307,    ap_uint<128>("0x27"),                               true,  0x007D, 0x007E, false, 0,      false, false, 0,          true},
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