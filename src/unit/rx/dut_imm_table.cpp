#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "rocev2_config.hpp"
#include "tests.hpp"

#include "mrc_transport_protocol/tables/imm_table.hpp"

// this module keeps, per QP, a ring of MRC_MAX_WIMM ImmDt indexed by RQMSN (6.3.3):
// 1. a request with hasImmDt stores immDt at slot rqmsn % MRC_MAX_WIMM, in whatever order the WriteIMMs arrive
// 2. a request with immInc != 0 outputs immInc immMeta {qpn, imm} on m_axis_rx_imm_meta, read from head, head + 1, ...
//    one per cycle, after the request has been read (store first, so a request can complete its own WriteIMM)
// head and immInc come from rx_exh_fsm (msn_table), so a row can use any head

namespace
{
    #define IMM_TABLE_INSTID 2 // own static table, not shared with the integration test (instances 0 and 1)

    struct ImmTableTestCase
    {
        const char* name;

        // input: rxImmReq
        uint16_t qpn;
        bool hasImmDt;
        uint16_t rqmsn;
        uint32_t immDt;
        uint16_t head;
        uint8_t immInc;

        // cycles run after writing the request, 0 = immInc + 1 (the read, then one completion per cycle)
        uint8_t cycles;

        // expected: ImmDt on m_axis_rx_imm_meta by the end of the cycles, in order, all with this row's qpn
        std::vector<uint32_t> exp_imm;
    };

    struct ImmTableTestBench
    {
        hls::stream<rxImmReq> rxExh2immTable_req;
        hls::stream<immMeta>  m_axis_rx_imm_meta;

        void tickOneCycle()
        {
            imm_table<IMM_TABLE_INSTID>(rxExh2immTable_req, m_axis_rx_imm_meta);
        }
    };

    bool runAndCheck(ImmTableTestBench& tb, const ImmTableTestCase& tc)
    {
        bool returnValue = true;
        auto fail = [&](const std::string& msg) {
            std::cerr << "[FAIL " << tc.name << "] " << msg << std::endl;
            returnValue = false;
        };
        auto expect = [&](const std::string& field, const ap_uint<32>& got, const ap_uint<32>& exp) {
            if (got != exp) fail(field + " " + got.to_string(16) + " != " + exp.to_string(16));
        };

        tb.rxExh2immTable_req.write(rxImmReq(tc.qpn, tc.hasImmDt, tc.rqmsn, tc.immDt, tc.head, tc.immInc));

        // never more than one completion per cycle
        const int cycles = tc.cycles ? tc.cycles : tc.immInc + 1;
        for (int c = 0; c < cycles; ++c) {
            const size_t before = tb.m_axis_rx_imm_meta.size();
            tb.tickOneCycle();
            if (tb.m_axis_rx_imm_meta.size() > before + 1) fail("more than one completion in cycle " + std::to_string(c));
        }

        if (!tb.rxExh2immTable_req.empty()) fail("request not consumed");
        expect("completion count", tb.m_axis_rx_imm_meta.size(), tc.exp_imm.size());

        for (uint32_t imm : tc.exp_imm) {
            if (tb.m_axis_rx_imm_meta.empty()) break;
            const immMeta m = tb.m_axis_rx_imm_meta.read();
            expect("qpn",   m.qpn, tc.qpn);
            expect("ImmDt", m.imm, imm);
        }

        // whatever is left (wrong counts above) must not leak into the next test case
        while (!tb.m_axis_rx_imm_meta.empty()) tb.m_axis_rx_imm_meta.read();

        return returnValue;
    }

    // the table keeps its content between rows. Every ImmDt is different, so a value read from the wrong slot is caught
    const ImmTableTestCase immTableTestCases[] = {
        // store, release, and both in the same request (a WriteIMM completed by its own LAST_IMM / ONLY_IMM)
        {.name = "IMM 01 store only",          .qpn = 5, .hasImmDt = true,  .rqmsn = 1, .immDt = 0x11111111, .head = 1, .immInc = 0, .cycles = 0, .exp_imm = {}},
        {.name = "IMM 02 release only",        .qpn = 5, .hasImmDt = false, .rqmsn = 0, .immDt = 0,          .head = 1, .immInc = 1, .cycles = 0, .exp_imm = {0x11111111}},
        {.name = "IMM 03 store and release",   .qpn = 5, .hasImmDt = true,  .rqmsn = 2, .immDt = 0x22222222, .head = 2, .immInc = 1, .cycles = 0, .exp_imm = {0x22222222}},

        // arrival 4, 5, 3: released in RQMSN order 3, 4, 5 by the request that stores 3
        {.name = "IMM 04 +1 ahead",            .qpn = 5, .hasImmDt = true,  .rqmsn = 4, .immDt = 0x44444444, .head = 3, .immInc = 0, .cycles = 0, .exp_imm = {}},
        {.name = "IMM 05 +2 ahead",            .qpn = 5, .hasImmDt = true,  .rqmsn = 5, .immDt = 0x55555555, .head = 3, .immInc = 0, .cycles = 0, .exp_imm = {}},
        {.name = "IMM 06 hole closed",         .qpn = 5, .hasImmDt = true,  .rqmsn = 3, .immDt = 0x33333333, .head = 3, .immInc = 3, .cycles = 0, .exp_imm = {0x33333333, 0x44444444, 0x55555555}},

        // a request without ImmDt (a plain packet closing a hole) releases what is held,
        // and its rqmsn / immDt are ignored: they point at a held slot, so a wrong store would overwrite 0x77777777
        {.name = "IMM 07 held 6",              .qpn = 5, .hasImmDt = true,  .rqmsn = 6, .immDt = 0x66666666, .head = 6, .immInc = 0, .cycles = 0, .exp_imm = {}},
        {.name = "IMM 08 held 7",              .qpn = 5, .hasImmDt = true,  .rqmsn = 7, .immDt = 0x77777777, .head = 6, .immInc = 0, .cycles = 0, .exp_imm = {}},
        {.name = "IMM 09 plain closes hole",   .qpn = 5, .hasImmDt = false, .rqmsn = 7, .immDt = 0xDEADBEEF, .head = 6, .immInc = 2, .cycles = 0, .exp_imm = {0x66666666, 0x77777777}},

        // ring wrap: RQMSN 31, 32, 33 are slots 31, 0, 1. Slot 1 still holds RQMSN 1 (row 01) and must be overwritten
        {.name = "IMM 10 rqmsn 31 slot 31",    .qpn = 5, .hasImmDt = true,  .rqmsn = 31, .immDt = 0x0000001F, .head = 31, .immInc = 0, .cycles = 0, .exp_imm = {}},
        {.name = "IMM 11 rqmsn 32 slot 0",     .qpn = 5, .hasImmDt = true,  .rqmsn = 32, .immDt = 0x00000020, .head = 31, .immInc = 0, .cycles = 0, .exp_imm = {}},
        {.name = "IMM 12 rqmsn 33 slot 1",     .qpn = 5, .hasImmDt = true,  .rqmsn = 33, .immDt = 0x00000021, .head = 31, .immInc = 3, .cycles = 0, .exp_imm = {0x0000001F, 0x00000020, 0x00000021}},

        // RQMSN wrap 0xFFFF -> 0x0000 (slots 31 and 0)
        {.name = "IMM 13 rqmsn 0xFFFF",        .qpn = 5, .hasImmDt = true,  .rqmsn = 0xFFFF, .immDt = 0xFFFF0000, .head = 0xFFFF, .immInc = 0, .cycles = 0, .exp_imm = {}},
        {.name = "IMM 14 rqmsn 0x0000",        .qpn = 5, .hasImmDt = true,  .rqmsn = 0x0000, .immDt = 0x0000FFFF, .head = 0xFFFF, .immInc = 2, .cycles = 0, .exp_imm = {0xFFFF0000, 0x0000FFFF}},

        // the same RQMSN on two QPs: every QP has its own ring and its qpn on the completion
        {.name = "IMM 15 QP 6 store",          .qpn = 6, .hasImmDt = true,  .rqmsn = 1, .immDt = 0x60000001, .head = 1, .immInc = 0, .cycles = 0, .exp_imm = {}},
        {.name = "IMM 16 QP 7 store",          .qpn = 7, .hasImmDt = true,  .rqmsn = 1, .immDt = 0x70000001, .head = 1, .immInc = 0, .cycles = 0, .exp_imm = {}},
        {.name = "IMM 17 QP 6 release",        .qpn = 6, .hasImmDt = false, .rqmsn = 0, .immDt = 0,          .head = 1, .immInc = 1, .cycles = 0, .exp_imm = {0x60000001}},
        {.name = "IMM 18 QP 7 release",        .qpn = 7, .hasImmDt = false, .rqmsn = 0, .immDt = 0,          .head = 1, .immInc = 1, .cycles = 0, .exp_imm = {0x70000001}},

        // first and last QP
        {.name = "IMM 19 QP 0",                .qpn = 0,           .hasImmDt = true, .rqmsn = 1, .immDt = 0x00000001, .head = 1, .immInc = 1, .cycles = 0, .exp_imm = {0x00000001}},
        {.name = "IMM 20 QP MAX_QPS - 1",      .qpn = MAX_QPS - 1, .hasImmDt = true, .rqmsn = 1, .immDt = 0xFF000001, .head = 1, .immInc = 1, .cycles = 0, .exp_imm = {0xFF000001}},

        // a request written while completions are still going out waits for them: row 23 only runs the read,
        // row 24 gets the three of row 23 first, then its own (read in cycle 4, completion in cycle 5)
        {.name = "IMM 21 QP 8 held 1",         .qpn = 8, .hasImmDt = true,  .rqmsn = 1, .immDt = 0x80000001, .head = 1, .immInc = 0, .cycles = 0, .exp_imm = {}},
        {.name = "IMM 22 QP 8 held 2",         .qpn = 8, .hasImmDt = true,  .rqmsn = 2, .immDt = 0x80000002, .head = 1, .immInc = 0, .cycles = 0, .exp_imm = {}},
        {.name = "IMM 23 QP 8 burst, 1 cycle", .qpn = 8, .hasImmDt = true,  .rqmsn = 3, .immDt = 0x80000003, .head = 1, .immInc = 3, .cycles = 1, .exp_imm = {}},
        {.name = "IMM 24 QP 8 queued",         .qpn = 8, .hasImmDt = true,  .rqmsn = 4, .immDt = 0x80000004, .head = 4, .immInc = 1, .cycles = 5, .exp_imm = {0x80000001, 0x80000002, 0x80000003, 0x80000004}},

        // all 32 bits of ImmDt are kept
        {.name = "IMM 25 ImmDt all ones",      .qpn = 9, .hasImmDt = true,  .rqmsn = 1, .immDt = 0xFFFFFFFF, .head = 1, .immInc = 1, .cycles = 0, .exp_imm = {0xFFFFFFFF}},
        {.name = "IMM 26 ImmDt bit 31 and 0",  .qpn = 9, .hasImmDt = true,  .rqmsn = 2, .immDt = 0x80000001, .head = 2, .immInc = 1, .cycles = 0, .exp_imm = {0x80000001}},
    };
} // namespace

int runImmTableTest()
{
    ImmTableTestBench tb;

    int failures = 0;
    const size_t testCaseCount = sizeof(immTableTestCases) / sizeof(immTableTestCases[0]);
    for (size_t tcIndex = 0; tcIndex < testCaseCount; ++tcIndex) {
        const ImmTableTestCase& tc = immTableTestCases[tcIndex];
        std::cout << "\n" << tc.name << "\n" << std::endl;
        failures += !runAndCheck(tb, tc);
    }

    // nothing comes out once every request has been served
    for (int c = 0; c < 8; ++c)
        tb.tickOneCycle();
    if (!tb.m_axis_rx_imm_meta.empty()) {
        std::cerr << "[FAIL] " << tb.m_axis_rx_imm_meta.size() << " completion(s) after the last row" << std::endl;
        ++failures;
    }

    return failures;
}