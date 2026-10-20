#include <cstdint>
#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "rocev2_config.hpp"
#include "util.hpp"
#include "tests.hpp"

#include "axi_utils.hpp"
#include "mrc_transport_protocol/rx/rx_data_res.hpp"

// it receives:
// 1. metadata          (op_code, partition_key, dest_qp, psn, validPSN, numPkg, ackReq, rtx, ts, msnInc, cack)
// 2. exhMetaFifo       (numPkg) <-- probably it can be removed
// 3. stateTable_rsp    (epsn, oldest_outstanding_psn (is it useful?), max_forward (is it useful?), retryCounter, bitmap, lastmap, (ackmap) prevAr)
// it outputs:
// 1. stateTable_upd_req        (qpn, epsn, retryCounter, isResponse, write, bitmap, lastmap, (ackmap) prevAr)
// 2. metaOut                   (op_code, partition_key, dest_qp, psn, validPSN, numPkg, ackReq, rtx, ts, msnInc, cack)
// 3. ibhEventFifo              (qpn, psn, validPsn, isNak)
// 4. ibhDropFifo
// 5. ibhDropMetaFifo           (isDrop, ackOnly)
// 6. regInvalidPsnDropCount

namespace
{
    #define RX_IBH_FSM_INSTID 3 // it is a static module (fsm state, cache, drop counter)!

    struct IbhFsmTestCase
    {
        // misc
        const char* name;

        // input: ibhMeta (msnInc and cack are outputs, validPSN / numPkg are only passed through)
        mrcOpCode op_code;
        uint16_t partition_key;
        uint32_t dest_qp;   // 24 bits
        uint32_t psn;       // 24 bits
        bool ackReq;
        bool rtx;
        bool ts;

        // input: state of dest_qp in the state table, written before the packet (like a QP setup) if preload
        bool preload;
        uint32_t epsn;
        ap_uint<MRC_MPR> bitmap;
        ap_uint<MRC_MPR> lastmap;
        //ap_uint<MRC_MPR> ackmap;
        bool prevAr;

        // expected: state table
        bool exp_read;              // false = cache hit, no read request
        bool exp_write;             // only accepted packets update the state
        uint32_t exp_epsn;
        ap_uint<MRC_MPR> exp_bitmap;
        ap_uint<MRC_MPR> exp_lastmap;
        //ap_uint<MRC_MPR> exp_ackmap;
        bool exp_prevAr;

        // expected: metaOut (only accepted packets), the other ibhMeta fields must be unchanged
        bool exp_metaOut;
        uint8_t exp_msnInc;
        bool exp_ackReq;
        uint32_t exp_cack;

        // expected: ibhEventFifo (only duplicates, an ACK with epsn - 1)
        bool exp_event;
        uint32_t exp_event_psn;

        // expected: ibhDropFifo, ibhDropMetaFifo, regInvalidPsnDropCount (cumulative)
        bool exp_drop;
        bool exp_isDrop;
        bool exp_ackOnly;
        uint32_t exp_regInvalidPsnDropCount;
    };

    struct RxIbhFsmTestBench
    {
#ifdef DBG_IBV
        hls::stream<psnPkg>         m_axis_dbg;
#endif
        hls::stream<ibhMeta>        metaIn;
        hls::stream<exhMeta>        exhMetaFifo;
        hls::stream<rxStateRsp>     stateTable_rsp;

        hls::stream<rxStateReq>     stateTable_upd_req;
        hls::stream<ibhMeta>        metaOut;
        hls::stream<ackEvent>       ibhEventFifo;
        hls::stream<bool>           ibhDropFifo;
        hls::stream<fwdPolicy>      ibhDropMetaFifo;
        hls::stream<ap_uint<16> >   stateTableInv;
        ap_uint<32>                 regInvalidPsnDropCount = 0;

        // the bench is the state table: it answers reads and applies writes, like state_table does
        std::map<uint32_t, rxStateRsp> stateTable;
        size_t reads = 0;
        std::vector<rxStateReq> writes;

        void tickOneCycle()
        {
            rx_ibh_fsm<RX_IBH_FSM_INSTID>(
#ifdef DBG_IBV
                m_axis_dbg,
    #endif
                metaIn,
                exhMetaFifo,
                stateTable_rsp,
                stateTable_upd_req,
                metaOut,
                ibhEventFifo,
                ibhDropFifo,
                ibhDropMetaFifo,
                stateTableInv,
                regInvalidPsnDropCount
            );
#ifdef DBG_IBV
            while (!m_axis_dbg.empty()) m_axis_dbg.read();
#endif

            while (!stateTable_upd_req.empty()) {
                const rxStateReq req = stateTable_upd_req.read();
                rxStateRsp& entry = stateTable[req.qpn];
                if (req.write) {
                    entry.epsn    = req.epsn;
                    entry.bitmap  = req.bitmap;
                    entry.lastmap = req.lastmap;
                    //entry.ackmap  = req.ackmap;
                    entry.prevAr  = req.prevAr;
                    writes.push_back(req);
                } else {
                    stateTable_rsp.write(entry);
                    ++reads;
                }
            }
        }
    };

    ibhMeta buildIbhMeta(const IbhFsmTestCase& testCase)
    {
        return ibhMeta(testCase.op_code, testCase.partition_key, testCase.dest_qp, testCase.psn, true, testCase.ackReq, testCase.rtx, testCase.ts);
    }

    rxStateRsp buildRxStateRsp(const IbhFsmTestCase& testCase)
    {
        rxStateRsp state(testCase.epsn, 0, testCase.epsn, 0x7);
        state.bitmap  = testCase.bitmap;
        state.lastmap = testCase.lastmap;
        //state.ackmap  = testCase.ackmap;
        state.prevAr  = testCase.prevAr;
        return state;
    }

    bool checkIbhFsm(RxIbhFsmTestBench& tb, const IbhFsmTestCase& tc)
    {
        bool returnValue = true;
        auto fail = [&](const std::string& msg) {
            std::cerr << "[FAIL " << tc.name << "] " << msg << std::endl;
            returnValue = false;
        };
        auto expect = [&](const std::string& field, const ap_uint<MRC_MPR>& got, const ap_uint<MRC_MPR>& exp) {
            if (got != exp) fail(field + " " + got.to_string(16) + " != " + exp.to_string(16));
        };

        // state table: one read on a cache miss, none on a hit; one write for an accepted packet
        expect("state table reads",  tb.reads,         tc.exp_read ? 1 : 0);
        expect("state table writes", tb.writes.size(), tc.exp_write ? 1 : 0);
        if (tc.exp_write && !tb.writes.empty()) {
            const rxStateReq& w = tb.writes[0];
            expect("write.qpn",     w.qpn,     tc.dest_qp & 0xFFFF);
            expect("write.epsn",    w.epsn,    tc.exp_epsn);
            expect("write.bitmap",  w.bitmap,  tc.exp_bitmap);
            expect("write.lastmap", w.lastmap, tc.exp_lastmap);
            //expect("write.ackmap",  w.ackmap,  tc.exp_ackmap);
            expect("write.prevAr",  w.prevAr,  tc.exp_prevAr);
        }

        // metaOut: accepted packets only, with msnInc / ackReq / cack computed and everything else unchanged
        expect("metaOut count", tb.metaOut.size(), tc.exp_metaOut ? 1 : 0);
        if (tc.exp_metaOut && !tb.metaOut.empty()) {
            const ibhMeta m = tb.metaOut.read();
            expect("metaOut.op_code",       m.op_code,       tc.op_code);
            expect("metaOut.partition_key", m.partition_key, tc.partition_key);
            expect("metaOut.dest_qp",       m.dest_qp,       tc.dest_qp);
            expect("metaOut.psn",           m.psn,           tc.psn);
            expect("metaOut.validPSN",      m.validPSN,      true);
            expect("metaOut.numPkg",        m.numPkg,        1);
            expect("metaOut.rtx",           m.rtx,           tc.rtx);
            expect("metaOut.ts",            m.ts,            tc.ts);
            expect("metaOut.msnInc",        m.msnInc,        tc.exp_msnInc);
            expect("metaOut.ackReq",        m.ackReq,        tc.exp_ackReq);
            expect("metaOut.cack",          m.cack,          tc.exp_cack);
        }

        // ibhEventFifo: duplicates only, an ACK (not a NAK) carrying epsn - 1
        expect("event count", tb.ibhEventFifo.size(), tc.exp_event ? 1 : 0);
        if (tc.exp_event && !tb.ibhEventFifo.empty()) {
            const ackEvent e = tb.ibhEventFifo.read();
            expect("event.qpn",      e.qpn,      tc.dest_qp);
            expect("event.psn",      e.psn,      tc.exp_event_psn);
            expect("event.validPsn", e.validPsn, true);
            expect("event.isNak",    e.isNak,    false);
        }

        // drop decision: one value on each stream for every packet
        expect("ibhDropFifo count", tb.ibhDropFifo.size(), 1);
        if (!tb.ibhDropFifo.empty()) expect("ibhDropFifo", tb.ibhDropFifo.read(), tc.exp_drop);
        expect("ibhDropMetaFifo count", tb.ibhDropMetaFifo.size(), 1);
        if (!tb.ibhDropMetaFifo.empty()) {
            const fwdPolicy p = tb.ibhDropMetaFifo.read();
            expect("fwdPolicy.isDrop",  p.isDrop,  tc.exp_isDrop);
            expect("fwdPolicy.ackOnly", p.ackOnly, tc.exp_ackOnly);
        }
        expect("regInvalidPsnDropCount", tb.regInvalidPsnDropCount, tc.exp_regInvalidPsnDropCount);

        // both inputs consumed
        if (!tb.metaIn.empty())         fail("metaIn not consumed");
        if (!tb.exhMetaFifo.empty())    fail("exhMetaFifo not consumed");
        if (!tb.stateTable_rsp.empty()) fail("stateTable_rsp not consumed");

        // whatever is left must not leak into the next test case
        while (!tb.metaOut.empty())         tb.metaOut.read();
        while (!tb.ibhEventFifo.empty())    tb.ibhEventFifo.read();
        while (!tb.ibhDropFifo.empty())     tb.ibhDropFifo.read();
        while (!tb.ibhDropMetaFifo.empty()) tb.ibhDropMetaFifo.read();
        while (!tb.metaIn.empty())          tb.metaIn.read();
        while (!tb.exhMetaFifo.empty())     tb.exhMetaFifo.read();
        while (!tb.stateTable_rsp.empty())  tb.stateTable_rsp.read();
        tb.reads = 0;
        tb.writes.clear();

        return returnValue;
    }

    #define BIT(i) (ap_uint<MRC_MPR>(1) << (i))
    #define ALL_BUT_0 (~ap_uint<MRC_MPR>(1))

    // bit i of the maps is psn epsn + i. The state persists between rows (bench state table + module cache),
    // so the rows must stay in this order. Accepted packets: drop false, policy (false, false).
    // Duplicates (bit already set or behind epsn): drop, policy (false, true), ACK epsn - 1.
    // Out of window (epsn + 128 or more ahead): drop, policy (true, false), nothing else
    const IbhFsmTestCase ibhFsmTestCases[] = {
        /* those are the ones for ackmap
        // QP 1 (epsn 0x100): in order. The first packet is a cache miss, the next ones hit
        {"IBH FSM 01 in order ONLY a",       MRC_RDMA_WRITE_ONLY,     0xFFFF, 1, 0x100, 1,0,0, true,  0x100, 0, 0, 0,   true,  true,  0x101, 0, 0, 0,                 true,  1, true,  0x100,   false, 0,       false, false, false, 0},
        {"IBH FSM 02 in order ONLY",         MRC_RDMA_WRITE_ONLY,     0x1234, 1, 0x101, 0,1,1, false, 0,     0, 0, 0,   false, true,  0x102, 0, 0, 0,                 true,  1, false, 0x101,   false, 0,       false, false, false, 0},

        // out of order inside the window: 0x105 LAST a, 0x103 FIRST, 0x104 MIDDLE, then 0x102 fills the hole
        {"IBH FSM 03 ahead +3 LAST a",       MRC_RDMA_WRITE_LAST,     0xFFFF, 1, 0x105, 1,0,0, false, 0,     0, 0, 0,   false, true,  0x102, BIT(3), BIT(3), BIT(3),    true,  0, false, 0x101,   false, 0,       false, false, false, 0},
        {"IBH FSM 04 ahead +1 FIRST",        MRC_RDMA_WRITE_FIRST,    0xFFFF, 1, 0x103, 0,0,0, false, 0,     0, 0, 0,   false, true,  0x102, BIT(3)|BIT(1), BIT(3), BIT(3), true,  0, false, 0x101, false, 0,       false, false, false, 0},
        {"IBH FSM 05 ahead +2 MIDDLE",       MRC_RDMA_WRITE_MIDDLE,   0xFFFF, 1, 0x104, 0,0,0, false, 0,     0, 0, 0,   false, true,  0x102, BIT(3)|BIT(2)|BIT(1), BIT(3), BIT(3), true, 0, false, 0x101, false, 0, false, false, false, 0},
        {"IBH FSM 06 duplicate in window",   MRC_RDMA_WRITE_MIDDLE,   0xFFFF, 1, 0x104, 1,0,0, false, 0,     0, 0, 0,   false, false, 0,     0, 0, 0,                 false, 0, false, 0,       true,  0x101,   true,  false, true,  1},
        {"IBH FSM 07 hole filled",           MRC_RDMA_WRITE_ONLY,     0xFFFF, 1, 0x102, 0,0,0, false, 0,     0, 0, 0,   false, true,  0x106, 0, 0, 0,                 true,  2, true,  0x105,   false, 0,       false, false, false, 1},

        // behind epsn, and window edges (epsn 0x106)
        {"IBH FSM 08 behind by 1",           MRC_RDMA_WRITE_ONLY,     0xFFFF, 1, 0x105, 1,0,0, false, 0,     0, 0, 0,   false, false, 0,     0, 0, 0,                 false, 0, false, 0,       true,  0x105,   true,  false, true,  2},
        {"IBH FSM 09 far behind",            MRC_RDMA_WRITE_ONLY,     0xFFFF, 1, 0x000006, 1,0,0, false, 0,  0, 0, 0,   false, false, 0,     0, 0, 0,                 false, 0, false, 0,       true,  0x105,   true,  false, true,  3},
        {"IBH FSM 10 epsn + 128",            MRC_RDMA_WRITE_ONLY,     0xFFFF, 1, 0x186, 1,0,0, false, 0,     0, 0, 0,   false, false, 0,     0, 0, 0,                 false, 0, false, 0,       false, 0,       true,  true,  false, 4},
        {"IBH FSM 11 far ahead",             MRC_RDMA_WRITE_ONLY,     0xFFFF, 1, 0x500000, 1,0,0, false, 0,  0, 0, 0,   false, false, 0,     0, 0, 0,                 false, 0, false, 0,       false, 0,       true,  true,  false, 5},
        {"IBH FSM 12 epsn + 127 ONLY a",     MRC_RDMA_WRITE_ONLY,     0xFFFF, 1, 0x185, 1,0,0, false, 0,     0, 0, 0,   false, true,  0x106, BIT(127), BIT(127), BIT(127), true, 0, false, 0x105,   false, 0,       false, false, false, 5},

        // QP 2 (epsn 0x200): cache miss, then the IMM opcodes also count in msnInc
        {"IBH FSM 13 QP2 miss ONLY a",       MRC_RDMA_WRITE_ONLY,     0xFFFF, 2, 0x200, 1,0,0, true,  0x200, 0, 0, 0,   true,  true,  0x201, 0, 0, 0,                 true,  1, true,  0x200,   false, 0,       false, false, false, 5},
        {"IBH FSM 14 QP2 ONLY IMM",          MRC_RDMA_WRITE_ONLY_IMM, 0xFFFF, 2, 0x201, 0,0,0, false, 0,     0, 0, 0,   false, true,  0x202, 0, 0, 0,                 true,  1, false, 0x201,   false, 0,       false, false, false, 5},
        {"IBH FSM 15 QP2 LAST IMM",          MRC_RDMA_WRITE_LAST_IMM, 0xFFFF, 2, 0x202, 0,0,0, false, 0,     0, 0, 0,   false, true,  0x203, 0, 0, 0,                 true,  1, false, 0x202,   false, 0,       false, false, false, 5},

        // back to QP 1: miss, its own state (bit 127 from row 12) must come back from the state table
        {"IBH FSM 16 QP1 miss again",        MRC_RDMA_WRITE_ONLY,     0xFFFF, 1, 0x106, 1,0,0, false, 0,     0, 0, 0,   true,  true,  0x107, BIT(126), BIT(126), BIT(126), true, 1, true,  0x106,   false, 0,       false, false, false, 5},
        {"IBH FSM 17 QP1 hit",               MRC_RDMA_WRITE_ONLY,     0xFFFF, 1, 0x107, 0,0,0, false, 0,     0, 0, 0,   false, true,  0x108, BIT(125), BIT(125), BIT(125), true, 1, false, 0x107,   false, 0,       false, false, false, 5},

        // QP 3 (epsn 0xFFFFFE): out of order across the 24-bit wrap
        {"IBH FSM 18 wrap +2",               MRC_RDMA_WRITE_ONLY,     0xFFFF, 3, 0x000000, 0,0,0, true, 0xFFFFFE, 0, 0, 0, true, true, 0xFFFFFE, BIT(2), BIT(2), 0,  true,  0, false, 0xFFFFFD, false, 0,      false, false, false, 5},
        {"IBH FSM 19 wrap +1",               MRC_RDMA_WRITE_ONLY,     0xFFFF, 3, 0xFFFFFF, 0,0,0, false, 0,  0, 0, 0,   false, true,  0xFFFFFE, BIT(2)|BIT(1), BIT(2)|BIT(1), 0, true, 0, false, 0xFFFFFD, false, 0, false, false, false, 5},
        {"IBH FSM 20 wrap hole filled",      MRC_RDMA_WRITE_ONLY,     0xFFFF, 3, 0xFFFFFE, 0,0,0, false, 0,  0, 0, 0,   false, true,  0x000001, 0, 0, 0,              true,  3, false, 0x000000, false, 0,      false, false, false, 5},
        {"IBH FSM 21 wrap duplicate",        MRC_RDMA_WRITE_ONLY,     0xFFFF, 3, 0xFFFFFF, 0,0,0, false, 0,  0, 0, 0,   false, false, 0,     0, 0, 0,                 false, 0, false, 0,       true,  0x000000, true, false, true,  6},

        // QP 4 (epsn 0x1000, +1 ... +127 already arrived, all LAST, one with a): +0 empties the whole window
        {"IBH FSM 22 full window",           MRC_RDMA_WRITE_ONLY,     0xFFFF, 4, 0x1000, 0,0,0, true, 0x1000, ALL_BUT_0, ALL_BUT_0, BIT(64), true, true, 0x1080, 0, 0, 0, true, 128, true, 0x107F, false, 0,      false, false, false, 6},

        // QP 4 is set up again while it is in the cache
        // Cache should be cleared on QP setup
        {"IBH FSM 23 QP setup clears cache", MRC_RDMA_WRITE_ONLY,     0xFFFF, 4, 0x2000, 1,0,0, true, 0x2000, 0, 0, 0,  true,  true,  0x2001, 0, 0, 0,                 true,  1, true,  0x2000,  false, 0,       false, false, false, 6},
        */
        // QP 1 (epsn 0x100): in order. The first packet is a cache miss, the next ones hit
        {"IBH FSM 01 in order ONLY a",       MRC_RDMA_WRITE_ONLY,     0xFFFF, 1, 0x100, 1,0,0, true,  0x100, 0, 0, false,   true,  true,  0x101, 0, 0, false,                 true,  1, true,  0x100,   false, 0,       false, false, false, 0},
        {"IBH FSM 02 in order ONLY",         MRC_RDMA_WRITE_ONLY,     0x1234, 1, 0x101, 0,1,1, false, 0,     0, 0, false,   false, true,  0x102, 0, 0, false,                 true,  1, false, 0x101,   false, 0,       false, false, false, 0},

        // out of order inside the window: 0x105 LAST a, 0x103 FIRST, 0x104 MIDDLE, then 0x102 fills the hole
        {"IBH FSM 03 ahead +3 LAST a",       MRC_RDMA_WRITE_LAST,     0xFFFF, 1, 0x105, 1,0,0, false, 0,     0, 0, false,   false, true,  0x102, BIT(3), BIT(3), true,      true,  0, false, 0x101,   false, 0,       false, false, false, 0},
        {"IBH FSM 04 ahead +1 FIRST",        MRC_RDMA_WRITE_FIRST,    0xFFFF, 1, 0x103, 0,0,0, false, 0,     0, 0, false,   false, true,  0x102, BIT(3)|BIT(1), BIT(3), true, true,  0, false, 0x101, false, 0,       false, false, false, 0},
        {"IBH FSM 05 ahead +2 MIDDLE",       MRC_RDMA_WRITE_MIDDLE,   0xFFFF, 1, 0x104, 0,0,0, false, 0,     0, 0, false,   false, true,  0x102, BIT(3)|BIT(2)|BIT(1), BIT(3), true, true, 0, false, 0x101, false, 0, false, false, false, 0},
        {"IBH FSM 06 duplicate in window",   MRC_RDMA_WRITE_MIDDLE,   0xFFFF, 1, 0x104, 1,0,0, false, 0,     0, 0, false,   false, false, 0,     0, 0, false,                 false, 0, false, 0,       true,  0x101,   true,  false, true,  1},
        {"IBH FSM 07 hole filled",           MRC_RDMA_WRITE_ONLY,     0xFFFF, 1, 0x102, 0,0,0, false, 0,     0, 0, false,   false, true,  0x106, 0, 0, false,                 true,  2, true,  0x105,   false, 0,       false, false, false, 1},

        // behind epsn, and window edges (epsn 0x106)
        {"IBH FSM 08 behind by 1",           MRC_RDMA_WRITE_ONLY,     0xFFFF, 1, 0x105, 1,0,0, false, 0,     0, 0, false,   false, false, 0,     0, 0, false,                 false, 0, false, 0,       true,  0x105,   true,  false, true,  2},
        {"IBH FSM 09 far behind",            MRC_RDMA_WRITE_ONLY,     0xFFFF, 1, 0x000006, 1,0,0, false, 0,  0, 0, false,   false, false, 0,     0, 0, false,                 false, 0, false, 0,       true,  0x105,   true,  false, true,  3},
        {"IBH FSM 10 epsn + 128",            MRC_RDMA_WRITE_ONLY,     0xFFFF, 1, 0x186, 1,0,0, false, 0,     0, 0, false,   false, false, 0,     0, 0, false,                 false, 0, false, 0,       false, 0,       true,  true,  false, 4},
        {"IBH FSM 11 far ahead",             MRC_RDMA_WRITE_ONLY,     0xFFFF, 1, 0x500000, 1,0,0, false, 0,  0, 0, false,   false, false, 0,     0, 0, false,                 false, 0, false, 0,       false, 0,       true,  true,  false, 5},
        {"IBH FSM 12 epsn + 127 ONLY a",     MRC_RDMA_WRITE_ONLY,     0xFFFF, 1, 0x185, 1,0,0, false, 0,     0, 0, false,   false, true,  0x106, BIT(127), BIT(127), true,  true,  0, false, 0x105,   false, 0,       false, false, false, 5},

        // QP 2 (epsn 0x200): cache miss, then the IMM opcodes also count in msnInc
        {"IBH FSM 13 QP2 miss ONLY a",       MRC_RDMA_WRITE_ONLY,     0xFFFF, 2, 0x200, 1,0,0, true,  0x200, 0, 0, false,   true,  true,  0x201, 0, 0, false,                 true,  1, true,  0x200,   false, 0,       false, false, false, 5},
        {"IBH FSM 14 QP2 ONLY IMM",          MRC_RDMA_WRITE_ONLY_IMM, 0xFFFF, 2, 0x201, 0,0,0, false, 0,     0, 0, false,   false, true,  0x202, 0, 0, false,                 true,  1, false, 0x201,   false, 0,       false, false, false, 5},
        {"IBH FSM 15 QP2 LAST IMM",          MRC_RDMA_WRITE_LAST_IMM, 0xFFFF, 2, 0x202, 0,0,0, false, 0,     0, 0, false,   false, true,  0x203, 0, 0, false,                 true,  1, false, 0x202,   false, 0,       false, false, false, 5},

        // back to QP 1: miss, its own state (bit 127 and prevAr from row 12) must come back from the state table
        {"IBH FSM 16 QP1 miss again",        MRC_RDMA_WRITE_ONLY,     0xFFFF, 1, 0x106, 1,0,0, false, 0,     0, 0, false,   true,  true,  0x107, BIT(126), BIT(126), true,  true,  1, true,  0x106,   false, 0,       false, false, false, 5},
        // the AR of 0x185 is still pending and cack advanced: ACK even though 0x185 is not covered yet
        {"IBH FSM 17 QP1 hit",               MRC_RDMA_WRITE_ONLY,     0xFFFF, 1, 0x107, 0,0,0, false, 0,     0, 0, false,   false, true,  0x108, BIT(125), BIT(125), true,  true,  1, true,  0x107,   false, 0,       false, false, false, 5},

        // QP 3 (epsn 0xFFFFFE): out of order across the 24-bit wrap
        {"IBH FSM 18 wrap +2",               MRC_RDMA_WRITE_ONLY,     0xFFFF, 3, 0x000000, 0,0,0, true, 0xFFFFFE, 0, 0, false, true, true, 0xFFFFFE, BIT(2), BIT(2), false,  true,  0, false, 0xFFFFFD, false, 0,      false, false, false, 5},
        {"IBH FSM 19 wrap +1",               MRC_RDMA_WRITE_ONLY,     0xFFFF, 3, 0xFFFFFF, 0,0,0, false, 0,  0, 0, false,   false, true,  0xFFFFFE, BIT(2)|BIT(1), BIT(2)|BIT(1), false, true, 0, false, 0xFFFFFD, false, 0, false, false, false, 5},
        {"IBH FSM 20 wrap hole filled",      MRC_RDMA_WRITE_ONLY,     0xFFFF, 3, 0xFFFFFE, 0,0,0, false, 0,  0, 0, false,   false, true,  0x000001, 0, 0, false,              true,  3, false, 0x000000, false, 0,      false, false, false, 5},
        {"IBH FSM 21 wrap duplicate",        MRC_RDMA_WRITE_ONLY,     0xFFFF, 3, 0xFFFFFF, 0,0,0, false, 0,  0, 0, false,   false, false, 0,     0, 0, false,                 false, 0, false, 0,       true,  0x000000, true, false, true,  6},

        // QP 4 (epsn 0x1000, +1 ... +127 already arrived, all LAST, an AR among them set prevAr): +0 empties the whole window
        {"IBH FSM 22 full window",           MRC_RDMA_WRITE_ONLY,     0xFFFF, 4, 0x1000, 0,0,0, true, 0x1000, ALL_BUT_0, ALL_BUT_0, true, true, true, 0x1080, 0, 0, false, true, 128, true, 0x107F, false, 0,      false, false, false, 6},

        // QP 4 is set up again while it is in the cache
        // Cache should be cleared on QP setup
        {"IBH FSM 23 QP setup clears cache", MRC_RDMA_WRITE_ONLY,     0xFFFF, 4, 0x2000, 1,0,0, true, 0x2000, 0, 0, false,  true,  true,  0x2001, 0, 0, false,                 true,  1, true,  0x2000,  false, 0,       false, false, false, 6},
    };
} // namespace

int runRxIbhFsmTest()
{
    RxIbhFsmTestBench tb;

    int failures = 0;
    const size_t testCaseCount = sizeof(ibhFsmTestCases) / sizeof(ibhFsmTestCases[0]);
    for (size_t tcIndex = 0; tcIndex < testCaseCount; ++tcIndex) {
        const IbhFsmTestCase& tc = ibhFsmTestCases[tcIndex];
        std::cout << "\n" << tc.name << "\n" << std::endl;

        if (tc.preload) {
            tb.stateTable[tc.dest_qp & 0xFFFF] = buildRxStateRsp(tc);
            tb.stateTableInv.write(tc.dest_qp & 0xFFFF); // like state_table does on a QP setup
        }
        tb.metaIn.write(buildIbhMeta(tc));
        tb.exhMetaFifo.write(exhMeta(1));

        // 5 is the number of cycles needed for every input to be processed (would be 4 without cache! Reason about it)
        for (int c = 0; c < 5; ++c) tb.tickOneCycle(); // LOAD, PROCESS, spare
        failures += !checkIbhFsm(tb, tc);
    }

    return failures;
}