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
#include "mrc_transport_protocol/rx/rx_ibh_fsm.hpp"

// it receives:
// 1. metadata          (op_code, partition_key, dest_qp, psn, validPSN, numPkg, ackReq, rtx, ts, msnInc, immInc, cack)
// 2. exhMetaFifo       (numPkg) <-- probably it can be removed
// 3. stateTable_rsp    (epsn, oldest_outstanding_psn (is it useful?), max_forward (is it useful?), retryCounter, bitmap, lastmap, immmap, prevAr)
// it outputs:
// 1. stateTable_upd_req        (qpn, epsn, retryCounter, isResponse, write, bitmap, lastmap, immmap, prevAr)
// 2. metaOut                   (op_code, partition_key, dest_qp, psn, validPSN, numPkg, ackReq, rtx, ts, msnInc, immInc, cack)
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
        uint16_t udp_len;   // input: udpLenIn

        // input: state of dest_qp in the state table, written before the packet (like a QP setup) if preload
        bool preload;
        uint32_t epsn;
        ap_uint<MRC_MPR> bitmap;
        ap_uint<MRC_MPR> lastmap;
        ap_uint<MRC_MPR> immmap;
        bool prevAr;

        // expected: state table
        //bool exp_read;              // false = cache hit, no read request
        bool exp_write;             // only accepted packets update the state
        uint32_t exp_epsn;
        ap_uint<MRC_MPR> exp_bitmap;
        ap_uint<MRC_MPR> exp_lastmap;
        ap_uint<MRC_MPR> exp_immmap;
        bool exp_prevAr;
        uint32_t exp_maxRcvPsn;
        uint32_t exp_lowestUnsackedPsn;
        uint32_t exp_rcvdBytes;
        uint32_t exp_sackTriggerCnt;

        // expected: metaOut (only accepted packets), the other ibhMeta fields must be unchanged
        bool exp_metaOut;
        uint8_t exp_msnInc;
        uint8_t exp_immInc;
        bool exp_ackReq;
        uint32_t exp_cack;
        uint32_t exp_sackOffset;    // only checked when exp_ackReq
        uint32_t exp_oooCount;

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
        hls::stream<ap_uint<16> >   udpLenIn;
        hls::stream<exhMeta>        exhMetaFifo;
        hls::stream<rxStateRsp>     stateTable_rsp;

        hls::stream<rxStateReq>     stateTable_upd_req;
        hls::stream<ibhMeta>        metaOut;
        hls::stream<ackEvent>       ibhEventFifo;
        hls::stream<bool>           ibhDropFifo;
        hls::stream<fwdPolicy>      ibhDropMetaFifo;
        //hls::stream<ap_uint<16> >   stateTableInv;
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
                udpLenIn,
                exhMetaFifo,
                stateTable_rsp,
                stateTable_upd_req,
                metaOut,
                ibhEventFifo,
                ibhDropFifo,
                ibhDropMetaFifo,
                //stateTableInv,
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
                    entry.immmap  = req.immmap;
                    entry.prevAr  = req.prevAr;
                    entry.maxRcvPsn         = req.maxRcvPsn;
                    entry.lowestUnsackedPsn = req.lowestUnsackedPsn;
                    entry.rcvdBytes         = req.rcvdBytes;
                    entry.sackTriggerCnt    = req.sackTriggerCnt;
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
        state.immmap  = testCase.immmap;
        state.prevAr  = testCase.prevAr;
        // like a QP setup, except max_rcv_psn which must match the preloaded bitmap
        state.maxRcvPsn = testCase.epsn - 1;
        for (int i = 0; i < MRC_MPR; i++)
            if (testCase.bitmap[i]) state.maxRcvPsn = testCase.epsn + i;
        state.lowestUnsackedPsn = testCase.epsn - 1;
        state.rcvdBytes         = 0;
        state.sackTriggerCnt    = 0;
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

        //// state table: one read on a cache miss, none on a hit; one write for an accepted packet
        //expect("state table reads",  tb.reads,         tc.exp_read ? 1 : 0);
        // state table: one read per packet; one write for an accepted packet or a duplicate
        expect("state table reads",  tb.reads,         1);
        expect("state table writes", tb.writes.size(), tc.exp_write ? 1 : 0);
        if (tc.exp_write && !tb.writes.empty()) {
            const rxStateReq& w = tb.writes[0];
            expect("write.qpn",                 w.qpn,                  tc.dest_qp & 0xFFFF);
            expect("write.epsn",                w.epsn,                 tc.exp_epsn);
            expect("write.bitmap",              w.bitmap,               tc.exp_bitmap);
            expect("write.lastmap",             w.lastmap,              tc.exp_lastmap);
            expect("write.immmap",              w.immmap,               tc.exp_immmap);
            expect("write.prevAr",              w.prevAr,               tc.exp_prevAr);
            expect("write.maxRcvPsn",           w.maxRcvPsn,            tc.exp_maxRcvPsn);
            expect("write.lowestUnsackedPsn",   w.lowestUnsackedPsn,    tc.exp_lowestUnsackedPsn);
            expect("write.rcvdBytes",           w.rcvdBytes,            tc.exp_rcvdBytes);
            expect("write.sackTriggerCnt",      w.sackTriggerCnt,       tc.exp_sackTriggerCnt);
        }

        // metaOut: accepted packets only, with msnInc / immInc / ackReq / cack computed and everything else unchanged
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
            expect("metaOut.immInc",        m.immInc,        tc.exp_immInc);
            expect("metaOut.ackReq",        m.ackReq,        tc.exp_ackReq);
            expect("metaOut.cack",          m.cack,          tc.exp_cack);
            expect("metaOut.oooCount",      m.oooCount,      tc.exp_oooCount);
            if (tc.exp_ackReq) expect("metaOut.sackOffset", m.sackOffset, tc.exp_sackOffset);
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
        if (!tb.udpLenIn.empty())       fail("udpLenIn not consumed");

        // whatever is left must not leak into the next test case
        while (!tb.metaOut.empty())         tb.metaOut.read();
        while (!tb.ibhEventFifo.empty())    tb.ibhEventFifo.read();
        while (!tb.ibhDropFifo.empty())     tb.ibhDropFifo.read();
        while (!tb.ibhDropMetaFifo.empty()) tb.ibhDropMetaFifo.read();
        while (!tb.metaIn.empty())          tb.metaIn.read();
        while (!tb.udpLenIn.empty())        tb.udpLenIn.read();
        while (!tb.exhMetaFifo.empty())     tb.exhMetaFifo.read();
        while (!tb.stateTable_rsp.empty())  tb.stateTable_rsp.read();
        tb.reads = 0;
        tb.writes.clear();

        return returnValue;
    }

    #define BIT(i) (ap_uint<MRC_MPR>(1) << (i))
    #define ALL_BUT_0 (~ap_uint<MRC_MPR>(1))
    #define ODD_BITS ap_uint<MRC_MPR>("0xAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA")


    // bit i of the maps is psn epsn + i. The state persists between rows (bench state table), so the rows must stay in this order
    // accepted packets: drop false, policy (false, false).
    // duplicates (bit already set or behind epsn): drop, policy (false, true), ACK epsn - 1, state written back (rcvd_bytes counted, sack_trigger_cnt reset)
    // out of window (epsn + 128 or more ahead): drop, policy (true, false), nothing else.
    // preloaded QPs start like after a QP setup: lowest_unsacked_psn = epsn - 1, counters 0, max_rcv_psn from the bitmap.
    // rows: designated initializers in the order of IbhFsmTestCase, a field that is not listed is 0 / false.
    // one line per group: packet, preloaded state, expected state write, expected metaOut, expected event and drop
    const IbhFsmTestCase ibhFsmTestCases[] = {
        {.name = "IBH FSM 01 in order ONLY a", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 1, .psn = 0x100, .ackReq = true, .udp_len = 1080,
         .preload = true, .epsn = 0x100,
         .exp_write = true, .exp_epsn = 0x101, .exp_maxRcvPsn = 0x100, .exp_lowestUnsackedPsn = 0x140, .exp_rcvdBytes = 1120,
         .exp_metaOut = true, .exp_msnInc = 1, .exp_ackReq = true, .exp_cack = 0x100},
        {.name = "IBH FSM 02 in order ONLY rtx", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0x1234, .dest_qp = 1, .psn = 0x101, .rtx = true, .ts = true, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x102, .exp_maxRcvPsn = 0x101, .exp_lowestUnsackedPsn = 0x141, .exp_rcvdBytes = 2240,
         .exp_metaOut = true, .exp_msnInc = 1, .exp_ackReq = true, .exp_cack = 0x101},
        {.name = "IBH FSM 03 ahead +3 LAST a", .op_code = MRC_RDMA_WRITE_LAST, .partition_key = 0xFFFF, .dest_qp = 1, .psn = 0x105, .ackReq = true, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x102, .exp_bitmap = BIT(3), .exp_lastmap = BIT(3), .exp_prevAr = true, .exp_maxRcvPsn = 0x105, .exp_lowestUnsackedPsn = 0x141, .exp_rcvdBytes = 3360,
         .exp_metaOut = true, .exp_msnInc = 0, .exp_ackReq = true, .exp_cack = 0x101, .exp_oooCount = 1},
        {.name = "IBH FSM 04 ahead +1 FIRST", .op_code = MRC_RDMA_WRITE_FIRST, .partition_key = 0xFFFF, .dest_qp = 1, .psn = 0x103, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x102, .exp_bitmap = BIT(1)|BIT(3), .exp_lastmap = BIT(3), .exp_prevAr = true, .exp_maxRcvPsn = 0x105, .exp_lowestUnsackedPsn = 0x103, .exp_rcvdBytes = 4480, .exp_sackTriggerCnt = 1036,
         .exp_metaOut = true, .exp_msnInc = 0, .exp_cack = 0x101, .exp_oooCount = 2},
        {.name = "IBH FSM 05 ahead +2 MIDDLE", .op_code = MRC_RDMA_WRITE_MIDDLE, .partition_key = 0xFFFF, .dest_qp = 1, .psn = 0x104, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x102, .exp_bitmap = BIT(1)|BIT(2)|BIT(3), .exp_lastmap = BIT(3), .exp_prevAr = true, .exp_maxRcvPsn = 0x105, .exp_lowestUnsackedPsn = 0x103, .exp_rcvdBytes = 5600, .exp_sackTriggerCnt = 2072,
         .exp_metaOut = true, .exp_msnInc = 0, .exp_cack = 0x101, .exp_oooCount = 3},
        {.name = "IBH FSM 06 duplicate in window", .op_code = MRC_RDMA_WRITE_MIDDLE, .partition_key = 0xFFFF, .dest_qp = 1, .psn = 0x104, .ackReq = true, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x102, .exp_bitmap = BIT(1)|BIT(2)|BIT(3), .exp_lastmap = BIT(3), .exp_prevAr = true, .exp_maxRcvPsn = 0x105, .exp_lowestUnsackedPsn = 0x141, .exp_rcvdBytes = 6720,
         .exp_event = true, .exp_event_psn = 0x101, .exp_drop = true, .exp_ackOnly = true, .exp_regInvalidPsnDropCount = 1},
        {.name = "IBH FSM 07 hole filled", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 1, .psn = 0x102, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x106, .exp_maxRcvPsn = 0x105, .exp_lowestUnsackedPsn = 0x145, .exp_rcvdBytes = 7840,
         .exp_metaOut = true, .exp_msnInc = 2, .exp_ackReq = true, .exp_cack = 0x105,
         .exp_regInvalidPsnDropCount = 1},
        {.name = "IBH FSM 08 behind by 1", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 1, .psn = 0x105, .ackReq = true, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x106, .exp_maxRcvPsn = 0x105, .exp_lowestUnsackedPsn = 0x145, .exp_rcvdBytes = 8960,
         .exp_event = true, .exp_event_psn = 0x105, .exp_drop = true, .exp_ackOnly = true, .exp_regInvalidPsnDropCount = 2},
        {.name = "IBH FSM 09 far behind", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 1, .psn = 0x6, .ackReq = true, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x106, .exp_maxRcvPsn = 0x105, .exp_lowestUnsackedPsn = 0x145, .exp_rcvdBytes = 10080,
         .exp_event = true, .exp_event_psn = 0x105, .exp_drop = true, .exp_ackOnly = true, .exp_regInvalidPsnDropCount = 3},
        {.name = "IBH FSM 10 epsn + 128", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 1, .psn = 0x186, .ackReq = true, .udp_len = 1080,
         .exp_drop = true, .exp_isDrop = true, .exp_regInvalidPsnDropCount = 4},
        {.name = "IBH FSM 11 far ahead", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 1, .psn = 0x500000, .ackReq = true, .udp_len = 1080,
         .exp_drop = true, .exp_isDrop = true, .exp_regInvalidPsnDropCount = 5},
        {.name = "IBH FSM 12 epsn + 127 ONLY a", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 1, .psn = 0x185, .ackReq = true, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x106, .exp_bitmap = BIT(127), .exp_lastmap = BIT(127), .exp_prevAr = true, .exp_maxRcvPsn = 0x185, .exp_lowestUnsackedPsn = 0x185, .exp_rcvdBytes = 11200,
         .exp_metaOut = true, .exp_msnInc = 0, .exp_ackReq = true, .exp_cack = 0x105, .exp_sackOffset = 64, .exp_oooCount = 1,
         .exp_regInvalidPsnDropCount = 5},
        {.name = "IBH FSM 13 QP2 ONLY a", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 2, .psn = 0x200, .ackReq = true, .udp_len = 1080,
         .preload = true, .epsn = 0x200,
         .exp_write = true, .exp_epsn = 0x201, .exp_maxRcvPsn = 0x200, .exp_lowestUnsackedPsn = 0x240, .exp_rcvdBytes = 1120,
         .exp_metaOut = true, .exp_msnInc = 1, .exp_ackReq = true, .exp_cack = 0x200,
         .exp_regInvalidPsnDropCount = 5},
        {.name = "IBH FSM 14 QP2 ONLY IMM", .op_code = MRC_RDMA_WRITE_ONLY_IMM, .partition_key = 0xFFFF, .dest_qp = 2, .psn = 0x201, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x202, .exp_maxRcvPsn = 0x201, .exp_lowestUnsackedPsn = 0x240, .exp_rcvdBytes = 2240, .exp_sackTriggerCnt = 1032,
         .exp_metaOut = true, .exp_msnInc = 1, .exp_immInc = 1, .exp_cack = 0x201,
         .exp_regInvalidPsnDropCount = 5},
        {.name = "IBH FSM 15 QP2 LAST IMM", .op_code = MRC_RDMA_WRITE_LAST_IMM, .partition_key = 0xFFFF, .dest_qp = 2, .psn = 0x202, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x203, .exp_maxRcvPsn = 0x202, .exp_lowestUnsackedPsn = 0x240, .exp_rcvdBytes = 3360, .exp_sackTriggerCnt = 2064,
         .exp_metaOut = true, .exp_msnInc = 1, .exp_immInc = 1, .exp_cack = 0x202,
         .exp_regInvalidPsnDropCount = 5},
        {.name = "IBH FSM 16 QP1 again", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 1, .psn = 0x106, .ackReq = true, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x107, .exp_bitmap = BIT(126), .exp_lastmap = BIT(126), .exp_prevAr = true, .exp_maxRcvPsn = 0x185, .exp_lowestUnsackedPsn = 0x185, .exp_rcvdBytes = 12320,
         .exp_metaOut = true, .exp_msnInc = 1, .exp_ackReq = true, .exp_cack = 0x106, .exp_sackOffset = 63, .exp_oooCount = 1,
         .exp_regInvalidPsnDropCount = 5},
        {.name = "IBH FSM 17 QP1 AR still pending", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 1, .psn = 0x107, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x108, .exp_bitmap = BIT(125), .exp_lastmap = BIT(125), .exp_prevAr = true, .exp_maxRcvPsn = 0x185, .exp_lowestUnsackedPsn = 0x185, .exp_rcvdBytes = 13440, .exp_sackTriggerCnt = 1036,
         .exp_metaOut = true, .exp_msnInc = 1, .exp_cack = 0x107, .exp_oooCount = 1,
         .exp_regInvalidPsnDropCount = 5},
        {.name = "IBH FSM 18 wrap +2", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 3, .psn = 0x0, .udp_len = 1080,
         .preload = true, .epsn = 0xFFFFFE,
         .exp_write = true, .exp_epsn = 0xFFFFFE, .exp_bitmap = BIT(2), .exp_lastmap = BIT(2), .exp_lowestUnsackedPsn = 0xFFFFFD, .exp_rcvdBytes = 1120, .exp_sackTriggerCnt = 1036,
         .exp_metaOut = true, .exp_msnInc = 0, .exp_cack = 0xFFFFFD, .exp_oooCount = 1,
         .exp_regInvalidPsnDropCount = 5},
        {.name = "IBH FSM 19 wrap +1", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 3, .psn = 0xFFFFFF, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0xFFFFFE, .exp_bitmap = BIT(1)|BIT(2), .exp_lastmap = BIT(1)|BIT(2), .exp_lowestUnsackedPsn = 0xFFFFFD, .exp_rcvdBytes = 2240, .exp_sackTriggerCnt = 2072,
         .exp_metaOut = true, .exp_msnInc = 0, .exp_cack = 0xFFFFFD, .exp_oooCount = 2,
         .exp_regInvalidPsnDropCount = 5},
        {.name = "IBH FSM 20 wrap hole filled", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 3, .psn = 0xFFFFFE, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x1, .exp_rcvdBytes = 3360, .exp_sackTriggerCnt = 3108,
         .exp_metaOut = true, .exp_msnInc = 3, .exp_cack = 0x0,
         .exp_regInvalidPsnDropCount = 5},
        {.name = "IBH FSM 21 wrap duplicate", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 3, .psn = 0xFFFFFF, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x1, .exp_lowestUnsackedPsn = 0x40, .exp_rcvdBytes = 4480,
         .exp_event = true, .exp_event_psn = 0x0, .exp_drop = true, .exp_ackOnly = true, .exp_regInvalidPsnDropCount = 6},
        {.name = "IBH FSM 22 full window", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 4, .psn = 0x1000, .udp_len = 1080,
         .preload = true, .epsn = 0x1000, .bitmap = ALL_BUT_0, .lastmap = ALL_BUT_0, .immmap = ODD_BITS, .prevAr = true,
         .exp_write = true, .exp_epsn = 0x1080, .exp_maxRcvPsn = 0x107F, .exp_lowestUnsackedPsn = 0x10BF, .exp_rcvdBytes = 1120,
         .exp_metaOut = true, .exp_msnInc = 128, .exp_immInc = 64, .exp_ackReq = true, .exp_cack = 0x107F,
         .exp_regInvalidPsnDropCount = 6},
        {.name = "IBH FSM 23 QP setup again", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 4, .psn = 0x2000, .ackReq = true, .udp_len = 1080,
         .preload = true, .epsn = 0x2000,
         .exp_write = true, .exp_epsn = 0x2001, .exp_maxRcvPsn = 0x2000, .exp_lowestUnsackedPsn = 0x2040, .exp_rcvdBytes = 1120,
         .exp_metaOut = true, .exp_msnInc = 1, .exp_ackReq = true, .exp_cack = 0x2000,
         .exp_regInvalidPsnDropCount = 6},
        {.name = "IBH FSM 24 small pkt counts 1KB", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 5, .psn = 0x500, .udp_len = 100,
         .preload = true, .epsn = 0x500,
         .exp_write = true, .exp_epsn = 0x501, .exp_maxRcvPsn = 0x500, .exp_lowestUnsackedPsn = 0x500, .exp_rcvdBytes = 140, .exp_sackTriggerCnt = 1024,
         .exp_metaOut = true, .exp_msnInc = 1, .exp_cack = 0x500,
         .exp_regInvalidPsnDropCount = 6},
        {.name = "IBH FSM 25 large pkt", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 5, .psn = 0x501, .udp_len = 9000,
         .exp_write = true, .exp_epsn = 0x502, .exp_maxRcvPsn = 0x501, .exp_lowestUnsackedPsn = 0x501, .exp_rcvdBytes = 9180, .exp_sackTriggerCnt = 9980,
         .exp_metaOut = true, .exp_msnInc = 1, .exp_cack = 0x501,
         .exp_regInvalidPsnDropCount = 6},
        {.name = "IBH FSM 26 trigger cnt > thr", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 5, .psn = 0x502, .udp_len = 8000,
         .exp_write = true, .exp_epsn = 0x503, .exp_maxRcvPsn = 0x502, .exp_lowestUnsackedPsn = 0x542, .exp_rcvdBytes = 17220,
         .exp_metaOut = true, .exp_msnInc = 1, .exp_ackReq = true, .exp_cack = 0x502,
         .exp_regInvalidPsnDropCount = 6},
        {.name = "IBH FSM 27 rtx triggers SACK", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 5, .psn = 0x503, .rtx = true, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x504, .exp_maxRcvPsn = 0x503, .exp_lowestUnsackedPsn = 0x543, .exp_rcvdBytes = 18340,
         .exp_metaOut = true, .exp_msnInc = 1, .exp_ackReq = true, .exp_cack = 0x503,
         .exp_regInvalidPsnDropCount = 6},
        {.name = "IBH FSM 28 +10 no AR", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 6, .psn = 0x60A, .udp_len = 1080,
         .preload = true, .epsn = 0x600,
         .exp_write = true, .exp_epsn = 0x600, .exp_bitmap = BIT(10), .exp_lastmap = BIT(10), .exp_maxRcvPsn = 0x60A, .exp_lowestUnsackedPsn = 0x5FF, .exp_rcvdBytes = 1120, .exp_sackTriggerCnt = 1036,
         .exp_metaOut = true, .exp_msnInc = 0, .exp_cack = 0x5FF, .exp_oooCount = 1,
         .exp_regInvalidPsnDropCount = 6},
        {.name = "IBH FSM 29 +100 AR", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 6, .psn = 0x664, .ackReq = true, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x600, .exp_bitmap = BIT(10)|BIT(100), .exp_lastmap = BIT(10)|BIT(100), .exp_prevAr = true, .exp_maxRcvPsn = 0x664, .exp_lowestUnsackedPsn = 0x63F, .exp_rcvdBytes = 2240,
         .exp_metaOut = true, .exp_msnInc = 0, .exp_ackReq = true, .exp_cack = 0x5FF, .exp_oooCount = 2,
         .exp_regInvalidPsnDropCount = 6},
        {.name = "IBH FSM 30 +101 AR right edge", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 6, .psn = 0x665, .ackReq = true, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x600, .exp_bitmap = BIT(10)|BIT(100)|BIT(101), .exp_lastmap = BIT(10)|BIT(100)|BIT(101), .exp_prevAr = true, .exp_maxRcvPsn = 0x665, .exp_lowestUnsackedPsn = 0x665, .exp_rcvdBytes = 3360,
         .exp_metaOut = true, .exp_msnInc = 0, .exp_ackReq = true, .exp_cack = 0x5FF, .exp_sackOffset = 38, .exp_oooCount = 3,
         .exp_regInvalidPsnDropCount = 6},
        {.name = "IBH FSM 31 +5 below lowest", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 6, .psn = 0x605, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x600, .exp_bitmap = BIT(5)|BIT(10)|BIT(100)|BIT(101), .exp_lastmap = BIT(5)|BIT(10)|BIT(100)|BIT(101), .exp_prevAr = true, .exp_maxRcvPsn = 0x665, .exp_lowestUnsackedPsn = 0x605, .exp_rcvdBytes = 4480, .exp_sackTriggerCnt = 1036,
         .exp_metaOut = true, .exp_msnInc = 0, .exp_cack = 0x5FF, .exp_oooCount = 4,
         .exp_regInvalidPsnDropCount = 6},
        {.name = "IBH FSM 32 +0 cack advances", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 6, .psn = 0x600, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x601, .exp_bitmap = BIT(4)|BIT(9)|BIT(99)|BIT(100), .exp_lastmap = BIT(4)|BIT(9)|BIT(99)|BIT(100), .exp_prevAr = true, .exp_maxRcvPsn = 0x665, .exp_lowestUnsackedPsn = 0x605, .exp_rcvdBytes = 5600, .exp_sackTriggerCnt = 2072,
         .exp_metaOut = true, .exp_msnInc = 1, .exp_cack = 0x600, .exp_oooCount = 4,
         .exp_regInvalidPsnDropCount = 6},
        {.name = "IBH FSM 33 duplicate resets cnt", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 6, .psn = 0x605, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x601, .exp_bitmap = BIT(4)|BIT(9)|BIT(99)|BIT(100), .exp_lastmap = BIT(4)|BIT(9)|BIT(99)|BIT(100), .exp_prevAr = true, .exp_maxRcvPsn = 0x665, .exp_lowestUnsackedPsn = 0x645, .exp_rcvdBytes = 6720,
         .exp_event = true, .exp_event_psn = 0x600, .exp_drop = true, .exp_ackOnly = true, .exp_regInvalidPsnDropCount = 7},
        {.name = "IBH FSM 34 TSETH is not payload", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 5, .psn = 0x504, .ts = true, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x505, .exp_maxRcvPsn = 0x504, .exp_lowestUnsackedPsn = 0x543, .exp_rcvdBytes = 19460, .exp_sackTriggerCnt = 1032,
         .exp_metaOut = true, .exp_msnInc = 1, .exp_cack = 0x504,
         .exp_regInvalidPsnDropCount = 7},
        // two WriteIMMs ahead of a hole are marked in immmap but not completed; the FIRST that closes the hole completes both
        {.name = "IBH FSM 35 +2 LAST IMM", .op_code = MRC_RDMA_WRITE_LAST_IMM, .partition_key = 0xFFFF, .dest_qp = 7, .psn = 0x702, .udp_len = 1080,
         .preload = true, .epsn = 0x700,
         .exp_write = true, .exp_epsn = 0x700, .exp_bitmap = BIT(2), .exp_lastmap = BIT(2), .exp_immmap = BIT(2), .exp_maxRcvPsn = 0x702, .exp_lowestUnsackedPsn = 0x6FF, .exp_rcvdBytes = 1120, .exp_sackTriggerCnt = 1032,
         .exp_metaOut = true, .exp_msnInc = 0, .exp_cack = 0x6FF, .exp_oooCount = 1,
         .exp_regInvalidPsnDropCount = 7},
        {.name = "IBH FSM 36 +1 ONLY IMM", .op_code = MRC_RDMA_WRITE_ONLY_IMM, .partition_key = 0xFFFF, .dest_qp = 7, .psn = 0x701, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x700, .exp_bitmap = BIT(1)|BIT(2), .exp_lastmap = BIT(1)|BIT(2), .exp_immmap = BIT(1)|BIT(2), .exp_maxRcvPsn = 0x702, .exp_lowestUnsackedPsn = 0x6FF, .exp_rcvdBytes = 2240, .exp_sackTriggerCnt = 2064,
         .exp_metaOut = true, .exp_msnInc = 0, .exp_cack = 0x6FF, .exp_oooCount = 2,
         .exp_regInvalidPsnDropCount = 7},
        {.name = "IBH FSM 37 +0 FIRST closes hole", .op_code = MRC_RDMA_WRITE_FIRST, .partition_key = 0xFFFF, .dest_qp = 7, .psn = 0x700, .udp_len = 1080,
         .exp_write = true, .exp_epsn = 0x703, .exp_maxRcvPsn = 0x702, .exp_lowestUnsackedPsn = 0x702, .exp_rcvdBytes = 3360, .exp_sackTriggerCnt = 3100,
         .exp_metaOut = true, .exp_msnInc = 2, .exp_immInc = 2, .exp_cack = 0x702,
         .exp_regInvalidPsnDropCount = 7},
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
            //tb.stateTableInv.write(tc.dest_qp & 0xFFFF); // like state_table does on a QP setup
        }
        tb.metaIn.write(buildIbhMeta(tc));
        tb.udpLenIn.write(tc.udp_len);
        tb.exhMetaFifo.write(exhMeta(1));

        // 5 is the number of cycles needed for every input to be processed (would be 4 without cache! Reason about it)
        for (int c = 0; c < 4; ++c) tb.tickOneCycle(); // LOAD, PROCESS, spare
        failures += !checkIbhFsm(tb, tc);
    }

    return failures;
}