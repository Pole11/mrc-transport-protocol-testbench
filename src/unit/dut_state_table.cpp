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
#include "mrc_transport_protocol/tables/state_table.hpp"

// it receives (one request per call, priority rx_ibh_fsm > rx_req_fsm > tx_ibh > qp_interface):
// 1. rxIbh2stateTable_upd_req     responder state (resp_*, retryCounter), read or write
// 2. rxCtrl2stateTable_upd_req    requestor reliability state (req_old_unack, req_retries, req_error), read or write
// 3. txIbh2stateTable_upd_req     req_next_psn write, or read of the whole entry
// 4. qpi2stateTable_upd_req       connection setup (full, or remote psn only), or read of the whole entry
// it outputs (reads only, on the port of the request):
// 1. stateTable2rxIbh_rsp         responder view (requestor view if isResponse)
// 2. stateTable2rxCtrl_rsp        unack_psn, max_psn_sent = req_next_psn - 1, retries, error
// 3. stateTable2txIbh_rsp         the whole entry
// 4. stateTable2qpi_rsp           the whole entry

namespace
{
    #define STATE_TABLE_INSTID 2 // it is a static module (the table)!

    enum StateTablePort { RX_IBH, RX_CTRL, TX_IBH, QPI };

    struct StateTableTestCase
    {
        // misc
        const char* name;
        bool concurrent;            // pending together with the next row, rows of a group are listed in service order

        // input: request
        StateTablePort port;
        uint32_t qpn;               // 24 bits like dest_qp, the request structs keep 16
        bool write;
        bool isResponse;            // RX_IBH only
        bool selective;             // QPI only: remote psn only

        // the entry of qpn after this row: a write sends the fields its port owns, a read must return them
        stateTableEntry entry;
    };

    struct StateTableTestBench
    {
        hls::stream<rxStateReq>      rxIbh2stateTable_upd_req;
        hls::stream<reqStateReq>     rxCtrl2stateTable_upd_req;
        hls::stream<txStateReq>      txIbh2stateTable_upd_req;
        hls::stream<ifStateReq>      qpi2stateTable_upd_req;

        hls::stream<rxStateRsp>      stateTable2rxIbh_rsp;
        hls::stream<reqStateRsp>     stateTable2rxCtrl_rsp;
        hls::stream<stateTableEntry> stateTable2txIbh_rsp;
        hls::stream<stateTableEntry> stateTable2qpi_rsp;

        void tickOneCycle()
        {
            state_table<STATE_TABLE_INSTID>(
                rxIbh2stateTable_upd_req,
                rxCtrl2stateTable_upd_req,
                txIbh2stateTable_upd_req,
                qpi2stateTable_upd_req,
                stateTable2rxIbh_rsp,
                stateTable2rxCtrl_rsp,
                stateTable2txIbh_rsp,
                stateTable2qpi_rsp
            );
        }
    };

    rxStateReq buildRxStateReq(const StateTableTestCase& testCase)
    {
        rxStateReq request(testCase.qpn, testCase.isResponse);
        if (testCase.write) {
            request.write             = true;
            request.epsn              = testCase.isResponse ? testCase.entry.req_old_unack : testCase.entry.resp_epsn;
            request.retryCounter      = testCase.entry.retryCounter;
            request.bitmap            = testCase.entry.resp_bitmap;
            request.lastmap           = testCase.entry.resp_lastmap;
            request.prevAr            = testCase.entry.resp_prev_ar;
            request.maxRcvPsn         = testCase.entry.resp_max_rcv_psn;
            request.lowestUnsackedPsn = testCase.entry.resp_lowest_unsacked_psn;
            request.rcvdBytes         = testCase.entry.resp_rcvd_bytes;
            request.sackTriggerCnt    = testCase.entry.resp_sack_trigger_cnt;
        }
        return request;
    }

    ifStateReq buildIfStateReq(const StateTableTestCase& testCase)
    {
        if (!testCase.write)
            return ifStateReq(testCase.qpn);
        ifStateReq request(testCase.qpn, testCase.entry.req_next_psn, testCase.selective);
        request.local_psn = testCase.entry.resp_epsn;
        return request;
    }

    bool checkStateTable(StateTableTestBench& tb, const StateTableTestCase& tc)
    {
        bool returnValue = true;
        auto fail = [&](const std::string& msg) {
            std::cerr << "[FAIL " << tc.name << "] " << msg << std::endl;
            returnValue = false;
        };
        auto expect = [&](const std::string& field, const ap_uint<MRC_MPR>& got, const ap_uint<MRC_MPR>& exp) {
            if (got != exp) fail(field + " " + got.to_string(16) + " != " + exp.to_string(16));
        };

        // a read gets exactly one response, on its own port, in the call that serves it. A write gets none
        expect("rxIbh rsp count",  tb.stateTable2rxIbh_rsp.size(),  !tc.write && tc.port == RX_IBH);
        expect("rxCtrl rsp count", tb.stateTable2rxCtrl_rsp.size(), !tc.write && tc.port == RX_CTRL);
        expect("txIbh rsp count",  tb.stateTable2txIbh_rsp.size(),  !tc.write && tc.port == TX_IBH);
        expect("qpi rsp count",    tb.stateTable2qpi_rsp.size(),    !tc.write && tc.port == QPI);

        // an invalid QP has no state: only the presence of the response is checked
        const stateTableEntry& e = tc.entry;
        if (!tc.write && tc.qpn < MAX_QPS) {
            if (tc.port == RX_IBH && !tb.stateTable2rxIbh_rsp.empty()) {
                rxStateRsp r = tb.stateTable2rxIbh_rsp.read();
                if (tc.isResponse) {
                    expect("epsn (req_old_unack)",                   r.epsn,                   e.req_old_unack);
                    expect("oldest_outstanding_psn (req_old_valid)", r.oldest_outstanding_psn, e.req_old_valid);
                    expect("max_forward (req_next_psn - 1)",         r.max_forward,            (ap_uint<24>) (e.req_next_psn - 1));
                } else {
                    expect("epsn",                   r.epsn,                   e.resp_epsn);
                    expect("oldest_outstanding_psn", r.oldest_outstanding_psn, e.resp_old_outstanding);
                    expect("max_forward",            r.max_forward,            e.resp_epsn);
                    expect("retryCounter",           r.retryCounter,           e.retryCounter);
                    expect("bitmap",                 r.bitmap,                 e.resp_bitmap);
                    expect("lastmap",                r.lastmap,                e.resp_lastmap);
                    expect("prevAr",                 r.prevAr,                 e.resp_prev_ar);
                    expect("maxRcvPsn",              r.maxRcvPsn,              e.resp_max_rcv_psn);
                    expect("lowestUnsackedPsn",      r.lowestUnsackedPsn,      e.resp_lowest_unsacked_psn);
                    expect("rcvdBytes",              r.rcvdBytes,              e.resp_rcvd_bytes);
                    expect("sackTriggerCnt",         r.sackTriggerCnt,         e.resp_sack_trigger_cnt);
                }
            }
            if (tc.port == RX_CTRL && !tb.stateTable2rxCtrl_rsp.empty()) {
                reqStateRsp r = tb.stateTable2rxCtrl_rsp.read();
                expect("unack_psn",    r.unack_psn,    e.req_old_unack);
                expect("max_psn_sent", r.max_psn_sent, (ap_uint<24>) (e.req_next_psn - 1));
                expect("retries",      r.retries,      e.req_retries);
                expect("error",        r.error,        e.req_error);
            }
            if ((tc.port == TX_IBH && !tb.stateTable2txIbh_rsp.empty()) || (tc.port == QPI && !tb.stateTable2qpi_rsp.empty())) {
                stateTableEntry s = tc.port == TX_IBH ? tb.stateTable2txIbh_rsp.read() : tb.stateTable2qpi_rsp.read();
                expect("resp_epsn",                s.resp_epsn,                e.resp_epsn);
                expect("resp_old_outstanding",     s.resp_old_outstanding,     e.resp_old_outstanding);
                expect("resp_bitmap",              s.resp_bitmap,              e.resp_bitmap);
                expect("resp_lastmap",             s.resp_lastmap,             e.resp_lastmap);
                expect("resp_prev_ar",             s.resp_prev_ar,             e.resp_prev_ar);
                expect("resp_max_rcv_psn",         s.resp_max_rcv_psn,         e.resp_max_rcv_psn);
                expect("resp_lowest_unsacked_psn", s.resp_lowest_unsacked_psn, e.resp_lowest_unsacked_psn);
                expect("resp_rcvd_bytes",          s.resp_rcvd_bytes,          e.resp_rcvd_bytes);
                expect("resp_sack_trigger_cnt",    s.resp_sack_trigger_cnt,    e.resp_sack_trigger_cnt);
                expect("req_next_psn",             s.req_next_psn,             e.req_next_psn);
                expect("req_old_unack",            s.req_old_unack,            e.req_old_unack);
                expect("req_old_valid",            s.req_old_valid,            e.req_old_valid);
                expect("retryCounter",             s.retryCounter,             e.retryCounter);
                expect("req_retries",              s.req_retries,              e.req_retries);
                expect("req_error",                s.req_error,                e.req_error);
            }
        }

        // after the last row of a group every request has been served
        if (!tc.concurrent) {
            if (!tb.rxIbh2stateTable_upd_req.empty())  fail("rxIbh request not consumed");
            if (!tb.rxCtrl2stateTable_upd_req.empty()) fail("rxCtrl request not consumed");
            if (!tb.txIbh2stateTable_upd_req.empty())  fail("txIbh request not consumed");
            if (!tb.qpi2stateTable_upd_req.empty())    fail("qpi request not consumed");
            while (!tb.rxIbh2stateTable_upd_req.empty())  tb.rxIbh2stateTable_upd_req.read();
            while (!tb.rxCtrl2stateTable_upd_req.empty()) tb.rxCtrl2stateTable_upd_req.read();
            while (!tb.txIbh2stateTable_upd_req.empty())  tb.txIbh2stateTable_upd_req.read();
            while (!tb.qpi2stateTable_upd_req.empty())    tb.qpi2stateTable_upd_req.read();
        }

        // whatever is left must not leak into the next test case
        while (!tb.stateTable2rxIbh_rsp.empty())  tb.stateTable2rxIbh_rsp.read();
        while (!tb.stateTable2rxCtrl_rsp.empty()) tb.stateTable2rxCtrl_rsp.read();
        while (!tb.stateTable2txIbh_rsp.empty())  tb.stateTable2txIbh_rsp.read();
        while (!tb.stateTable2qpi_rsp.empty())    tb.stateTable2qpi_rsp.read();

        return returnValue;
    }

    // The table keeps its content between rows (QP 1, 2, 3 and the first / last QP 0, 255).
    // entry: resp_epsn, resp_old_outstanding, resp_bitmap, resp_lastmap, resp_prev_ar, resp_max_rcv_psn, resp_lowest_unsacked_psn,
    //        resp_rcvd_bytes, resp_sack_trigger_cnt, req_next_psn, req_old_unack, req_old_valid, retryCounter, req_retries, req_error
    // QPI write: local_psn = resp_epsn, remote_psn = req_next_psn. Setup writes retryCounter = 0xF, 3 bits: 7 (CURRENT BEHAVIOUR)
    const StateTableTestCase stateTableTestCases[] = {
        // setup, then the same entry read through every port
        {"STATE 01 setup QP1",                false, QPI,     1,        true,  false, false, {0x000100, 0x000100, 0, 0, false, 0x0000FF, 0x0000FF, 0x00000000, 0x000000, 0x000A00, 0x000A00, 0x000A00, 7, 0, false}},
        {"STATE 02 QPI read QP1",             false, QPI,     1,        false, false, false, {0x000100, 0x000100, 0, 0, false, 0x0000FF, 0x0000FF, 0x00000000, 0x000000, 0x000A00, 0x000A00, 0x000A00, 7, 0, false}},
        {"STATE 03 TX read QP1",              false, TX_IBH,  1,        false, false, false, {0x000100, 0x000100, 0, 0, false, 0x0000FF, 0x0000FF, 0x00000000, 0x000000, 0x000A00, 0x000A00, 0x000A00, 7, 0, false}},
        {"STATE 04 RX read QP1",              false, RX_IBH,  1,        false, false, false, {0x000100, 0x000100, 0, 0, false, 0x0000FF, 0x0000FF, 0x00000000, 0x000000, 0x000A00, 0x000A00, 0x000A00, 7, 0, false}},
        {"STATE 05 CTRL read QP1",            false, RX_CTRL, 1,        false, false, false, {0x000100, 0x000100, 0, 0, false, 0x0000FF, 0x0000FF, 0x00000000, 0x000000, 0x000A00, 0x000A00, 0x000A00, 7, 0, false}},

        // every port writes only its own fields (retryCounter belongs to RX, req_retries to CTRL)
        {"STATE 06 RX write QP1",             false, RX_IBH,  1,        true,  false, false, {0x000105, 0x000100, ap_uint<128>("0x0A"), ap_uint<128>("0x08"), true, 0x000108, 0x000104, 0x00001234, 0x000400, 0x000A00, 0x000A00, 0x000A00, 3, 0, false}},
        {"STATE 07 QPI read QP1",             false, QPI,     1,        false, false, false, {0x000105, 0x000100, ap_uint<128>("0x0A"), ap_uint<128>("0x08"), true, 0x000108, 0x000104, 0x00001234, 0x000400, 0x000A00, 0x000A00, 0x000A00, 3, 0, false}},
        {"STATE 08 CTRL write QP1",           false, RX_CTRL, 1,        true,  false, false, {0x000105, 0x000100, ap_uint<128>("0x0A"), ap_uint<128>("0x08"), true, 0x000108, 0x000104, 0x00001234, 0x000400, 0x000A00, 0x000A05, 0x000A00, 3, 2, true}},
        {"STATE 09 TX read QP1",              false, TX_IBH,  1,        false, false, false, {0x000105, 0x000100, ap_uint<128>("0x0A"), ap_uint<128>("0x08"), true, 0x000108, 0x000104, 0x00001234, 0x000400, 0x000A00, 0x000A05, 0x000A00, 3, 2, true}},
        {"STATE 10 TX write QP1",             false, TX_IBH,  1,        true,  false, false, {0x000105, 0x000100, ap_uint<128>("0x0A"), ap_uint<128>("0x08"), true, 0x000108, 0x000104, 0x00001234, 0x000400, 0x000A10, 0x000A05, 0x000A00, 3, 2, true}},
        {"STATE 11 QPI read QP1",             false, QPI,     1,        false, false, false, {0x000105, 0x000100, ap_uint<128>("0x0A"), ap_uint<128>("0x08"), true, 0x000108, 0x000104, 0x00001234, 0x000400, 0x000A10, 0x000A05, 0x000A00, 3, 2, true}},
        {"STATE 12 CTRL read QP1",            false, RX_CTRL, 1,        false, false, false, {0x000105, 0x000100, ap_uint<128>("0x0A"), ap_uint<128>("0x08"), true, 0x000108, 0x000104, 0x00001234, 0x000400, 0x000A10, 0x000A05, 0x000A00, 3, 2, true}},
        {"STATE 13 RX read QP1",              false, RX_IBH,  1,        false, false, false, {0x000105, 0x000100, ap_uint<128>("0x0A"), ap_uint<128>("0x08"), true, 0x000108, 0x000104, 0x00001234, 0x000400, 0x000A10, 0x000A05, 0x000A00, 3, 2, true}},

        // remote psn only update on a QP with responder state: the requestor side is reset, the responder side is kept.
        // Then a full setup (reconnection) resets everything
        {"STATE 14 QPI selective QP1",        false, QPI,     1,        true,  false, true,  {0x000105, 0x000100, ap_uint<128>("0x0A"), ap_uint<128>("0x08"), true, 0x000108, 0x000104, 0x00001234, 0x000400, 0x000B00, 0x000B00, 0x000B00, 3, 0, false}},
        {"STATE 15 QPI read QP1",             false, QPI,     1,        false, false, false, {0x000105, 0x000100, ap_uint<128>("0x0A"), ap_uint<128>("0x08"), true, 0x000108, 0x000104, 0x00001234, 0x000400, 0x000B00, 0x000B00, 0x000B00, 3, 0, false}},
        {"STATE 16 setup QP1 again",          false, QPI,     1,        true,  false, false, {0x000200, 0x000200, 0, 0, false, 0x0001FF, 0x0001FF, 0x00000000, 0x000000, 0x000C00, 0x000C00, 0x000C00, 7, 0, false}},
        {"STATE 17 TX read QP1",              false, TX_IBH,  1,        false, false, false, {0x000200, 0x000200, 0, 0, false, 0x0001FF, 0x0001FF, 0x00000000, 0x000000, 0x000C00, 0x000C00, 0x000C00, 7, 0, false}},

        // first and last QP, every QP has its own entry
        {"STATE 18 setup QP0",                false, QPI,     0,        true,  false, false, {0x000010, 0x000010, 0, 0, false, 0x00000F, 0x00000F, 0x00000000, 0x000000, 0x000020, 0x000020, 0x000020, 7, 0, false}},
        {"STATE 19 setup QP255",              false, QPI,     255,      true,  false, false, {0x0FF000, 0x0FF000, 0, 0, false, 0x0FEFFF, 0x0FEFFF, 0x00000000, 0x000000, 0x0FF100, 0x0FF100, 0x0FF100, 7, 0, false}},
        {"STATE 20 RX write QP255",           false, RX_IBH,  255,      true,  false, false, {0x0FF002, 0x0FF000, ap_uint<128>("0x80000000000000000000000000000002"), ap_uint<128>("0x80000000000000000000000000000000"), false, 0x0FF081, 0x0FF001, 0x00080000, 0x001000, 0x0FF100, 0x0FF100, 0x0FF100, 5, 0, false}},
        {"STATE 21 TX read QP0",              false, TX_IBH,  0,        false, false, false, {0x000010, 0x000010, 0, 0, false, 0x00000F, 0x00000F, 0x00000000, 0x000000, 0x000020, 0x000020, 0x000020, 7, 0, false}},
        {"STATE 22 TX read QP255",            false, TX_IBH,  255,      false, false, false, {0x0FF002, 0x0FF000, ap_uint<128>("0x80000000000000000000000000000002"), ap_uint<128>("0x80000000000000000000000000000000"), false, 0x0FF081, 0x0FF001, 0x00080000, 0x001000, 0x0FF100, 0x0FF100, 0x0FF100, 5, 0, false}},

        // underflow: psn 0 - 1 wraps to 0xFFFFFF (setup with local psn 0, max_psn_sent with next psn 0).
        // overflow: every field at its maximum is stored and returned whole (bitmap bit 127, rcvdBytes bit 31)
        {"STATE 23 setup QP3 psn 0",          false, QPI,     3,        true,  false, false, {0x000000, 0x000000, 0, 0, false, 0xFFFFFF, 0xFFFFFF, 0x00000000, 0x000000, 0x000000, 0x000000, 0x000000, 7, 0, false}},
        {"STATE 24 QPI read QP3",             false, QPI,     3,        false, false, false, {0x000000, 0x000000, 0, 0, false, 0xFFFFFF, 0xFFFFFF, 0x00000000, 0x000000, 0x000000, 0x000000, 0x000000, 7, 0, false}},
        {"STATE 25 CTRL read next psn 0",     false, RX_CTRL, 3,        false, false, false, {0x000000, 0x000000, 0, 0, false, 0xFFFFFF, 0xFFFFFF, 0x00000000, 0x000000, 0x000000, 0x000000, 0x000000, 7, 0, false}},
        {"STATE 26 RX write all ones",        false, RX_IBH,  3,        true,  false, false, {0xFFFFFF, 0x000000, ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), true, 0xFFFFFF, 0xFFFFFF, 0xFFFFFFFF, 0xFFFFFF, 0x000000, 0x000000, 0x000000, 7, 0, false}},
        {"STATE 27 RX read all ones",         false, RX_IBH,  3,        false, false, false, {0xFFFFFF, 0x000000, ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), true, 0xFFFFFF, 0xFFFFFF, 0xFFFFFFFF, 0xFFFFFF, 0x000000, 0x000000, 0x000000, 7, 0, false}},
        {"STATE 28 CTRL write all ones",      false, RX_CTRL, 3,        true,  false, false, {0xFFFFFF, 0x000000, ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), true, 0xFFFFFF, 0xFFFFFF, 0xFFFFFFFF, 0xFFFFFF, 0x000000, 0xFFFFFF, 0x000000, 7, 7, true}},
        {"STATE 29 TX write next psn max",    false, TX_IBH,  3,        true,  false, false, {0xFFFFFF, 0x000000, ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), true, 0xFFFFFF, 0xFFFFFF, 0xFFFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0x000000, 7, 7, true}},
        {"STATE 30 CTRL read next psn max",   false, RX_CTRL, 3,        false, false, false, {0xFFFFFF, 0x000000, ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), true, 0xFFFFFF, 0xFFFFFF, 0xFFFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0x000000, 7, 7, true}},
        {"STATE 31 TX write next psn wrap",   false, TX_IBH,  3,        true,  false, false, {0xFFFFFF, 0x000000, ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), true, 0xFFFFFF, 0xFFFFFF, 0xFFFFFFFF, 0xFFFFFF, 0x000000, 0xFFFFFF, 0x000000, 7, 7, true}},
        {"STATE 32 CTRL read next psn wrap",  false, RX_CTRL, 3,        false, false, false, {0xFFFFFF, 0x000000, ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), true, 0xFFFFFF, 0xFFFFFF, 0xFFFFFFFF, 0xFFFFFF, 0x000000, 0xFFFFFF, 0x000000, 7, 7, true}},
        {"STATE 33 TX read QP3",              false, TX_IBH,  3,        false, false, false, {0xFFFFFF, 0x000000, ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), true, 0xFFFFFF, 0xFFFFFF, 0xFFFFFFFF, 0xFFFFFF, 0x000000, 0xFFFFFF, 0x000000, 7, 7, true}},

        // concurrent requests: one served per call, rx > ctrl > tx > qpi, a read sees the writes served before it
        {"STATE 34 setup QP2",                false, QPI,     2,        true,  false, false, {0x000300, 0x000300, 0, 0, false, 0x0002FF, 0x0002FF, 0x00000000, 0x000000, 0x000D00, 0x000D00, 0x000D00, 7, 0, false}},
        {"STATE 35 all ports: RX write",      true,  RX_IBH,  2,        true,  false, false, {0x000301, 0x000300, ap_uint<128>("0x2"), ap_uint<128>("0x2"), false, 0x000302, 0x000300, 0x00000840, 0x000400, 0x000D00, 0x000D00, 0x000D00, 6, 0, false}},
        {"STATE 36 all ports: CTRL read",     true,  RX_CTRL, 2,        false, false, false, {0x000301, 0x000300, ap_uint<128>("0x2"), ap_uint<128>("0x2"), false, 0x000302, 0x000300, 0x00000840, 0x000400, 0x000D00, 0x000D00, 0x000D00, 6, 0, false}},
        {"STATE 37 all ports: TX write",      true,  TX_IBH,  2,        true,  false, false, {0x000301, 0x000300, ap_uint<128>("0x2"), ap_uint<128>("0x2"), false, 0x000302, 0x000300, 0x00000840, 0x000400, 0x000D08, 0x000D00, 0x000D00, 6, 0, false}},
        {"STATE 38 all ports: QPI read",      false, QPI,     2,        false, false, false, {0x000301, 0x000300, ap_uint<128>("0x2"), ap_uint<128>("0x2"), false, 0x000302, 0x000300, 0x00000840, 0x000400, 0x000D08, 0x000D00, 0x000D00, 6, 0, false}},
        // TX write already pending, the CTRL read is served first: it gets the old max_psn_sent
        {"STATE 39 CTRL read before TX write",true,  RX_CTRL, 2,        false, false, false, {0x000301, 0x000300, ap_uint<128>("0x2"), ap_uint<128>("0x2"), false, 0x000302, 0x000300, 0x00000840, 0x000400, 0x000D08, 0x000D00, 0x000D00, 6, 0, false}},
        {"STATE 40 TX write after CTRL read", false, TX_IBH,  2,        true,  false, false, {0x000301, 0x000300, ap_uint<128>("0x2"), ap_uint<128>("0x2"), false, 0x000302, 0x000300, 0x00000840, 0x000400, 0x000D10, 0x000D00, 0x000D00, 6, 0, false}},
        {"STATE 41 CTRL read QP2",            false, RX_CTRL, 2,        false, false, false, {0x000301, 0x000300, ap_uint<128>("0x2"), ap_uint<128>("0x2"), false, 0x000302, 0x000300, 0x00000840, 0x000400, 0x000D10, 0x000D00, 0x000D00, 6, 0, false}},
        // CTRL write and TX read pending together: the TX read already sees the write
        {"STATE 42 CTRL write before TX read",true,  RX_CTRL, 2,        true,  false, false, {0x000301, 0x000300, ap_uint<128>("0x2"), ap_uint<128>("0x2"), false, 0x000302, 0x000300, 0x00000840, 0x000400, 0x000D10, 0x000D04, 0x000D00, 6, 1, false}},
        {"STATE 43 TX read after CTRL write", false, TX_IBH,  2,        false, false, false, {0x000301, 0x000300, ap_uint<128>("0x2"), ap_uint<128>("0x2"), false, 0x000302, 0x000300, 0x00000840, 0x000400, 0x000D10, 0x000D04, 0x000D00, 6, 1, false}},
        // requests on the same port are served in order, a read right after a write of the same QP sees it,
        // the QPI read waits for all of them
        {"STATE 44 RX queue: write",          true,  RX_IBH,  2,        true,  false, false, {0x000303, 0x000300, 0, 0, false, 0x000302, 0x000302, 0x00001080, 0x000800, 0x000D10, 0x000D04, 0x000D00, 6, 1, false}},
        {"STATE 45 RX queue: read",           true,  RX_IBH,  2,        false, false, false, {0x000303, 0x000300, 0, 0, false, 0x000302, 0x000302, 0x00001080, 0x000800, 0x000D10, 0x000D04, 0x000D00, 6, 1, false}},
        {"STATE 46 RX queue: write",          true,  RX_IBH,  2,        true,  false, false, {0x000304, 0x000300, ap_uint<128>("0x4"), ap_uint<128>("0x4"), true, 0x000306, 0x000303, 0x000018C0, 0x000C00, 0x000D10, 0x000D04, 0x000D00, 6, 1, false}},
        {"STATE 47 RX queue: read",           true,  RX_IBH,  2,        false, false, false, {0x000304, 0x000300, ap_uint<128>("0x4"), ap_uint<128>("0x4"), true, 0x000306, 0x000303, 0x000018C0, 0x000C00, 0x000D10, 0x000D04, 0x000D00, 6, 1, false}},
        {"STATE 48 RX queue: QPI read last",  false, QPI,     2,        false, false, false, {0x000304, 0x000300, ap_uint<128>("0x4"), ap_uint<128>("0x4"), true, 0x000306, 0x000303, 0x000018C0, 0x000C00, 0x000D10, 0x000D04, 0x000D00, 6, 1, false}},
        // writes to different QPs at the same time, QP2 untouched. Then reads of different QPs at the same time
        {"STATE 49 RX write QP0",             true,  RX_IBH,  0,        true,  false, false, {0x000011, 0x000010, 0, 0, false, 0x000010, 0x000010, 0x00000040, 0x000400, 0x000020, 0x000020, 0x000020, 4, 0, false}},
        {"STATE 50 CTRL write QP255",         true,  RX_CTRL, 255,      true,  false, false, {0x0FF002, 0x0FF000, ap_uint<128>("0x80000000000000000000000000000002"), ap_uint<128>("0x80000000000000000000000000000000"), false, 0x0FF081, 0x0FF001, 0x00080000, 0x001000, 0x0FF100, 0x0FF101, 0x0FF100, 5, 1, false}},
        {"STATE 51 TX write QP1",             true,  TX_IBH,  1,        true,  false, false, {0x000200, 0x000200, 0, 0, false, 0x0001FF, 0x0001FF, 0x00000000, 0x000000, 0x000C01, 0x000C00, 0x000C00, 7, 0, false}},
        {"STATE 52 QPI read QP2",             false, QPI,     2,        false, false, false, {0x000304, 0x000300, ap_uint<128>("0x4"), ap_uint<128>("0x4"), true, 0x000306, 0x000303, 0x000018C0, 0x000C00, 0x000D10, 0x000D04, 0x000D00, 6, 1, false}},
        {"STATE 53 RX read QP0",              true,  RX_IBH,  0,        false, false, false, {0x000011, 0x000010, 0, 0, false, 0x000010, 0x000010, 0x00000040, 0x000400, 0x000020, 0x000020, 0x000020, 4, 0, false}},
        {"STATE 54 CTRL read QP255",          true,  RX_CTRL, 255,      false, false, false, {0x0FF002, 0x0FF000, ap_uint<128>("0x80000000000000000000000000000002"), ap_uint<128>("0x80000000000000000000000000000000"), false, 0x0FF081, 0x0FF001, 0x00080000, 0x001000, 0x0FF100, 0x0FF101, 0x0FF100, 5, 1, false}},
        {"STATE 55 TX read QP1",              false, TX_IBH,  1,        false, false, false, {0x000200, 0x000200, 0, 0, false, 0x0001FF, 0x0001FF, 0x00000000, 0x000000, 0x000C01, 0x000C00, 0x000C00, 7, 0, false}},

        // malformed: rx request with isResponse (not sent since step B). CURRENT BEHAVIOUR: the read returns the requestor
        // view, the write overwrites req_old_unack, which belongs to rx_req_fsm
        {"STATE 56 RX isResponse read",       false, RX_IBH,  1,        false, true,  false, {0x000200, 0x000200, 0, 0, false, 0x0001FF, 0x0001FF, 0x00000000, 0x000000, 0x000C01, 0x000C00, 0x000C00, 7, 0, false}},
        {"STATE 57 RX isResponse write",      false, RX_IBH,  1,        true,  true,  false, {0x000200, 0x000200, 0, 0, false, 0x0001FF, 0x0001FF, 0x00000000, 0x000000, 0x000C01, 0x000ABC, 0x000C00, 7, 0, false}},
        {"STATE 58 CTRL read QP1",            false, RX_CTRL, 1,        false, false, false, {0x000200, 0x000200, 0, 0, false, 0x0001FF, 0x0001FF, 0x00000000, 0x000000, 0x000C01, 0x000ABC, 0x000C00, 7, 0, false}},
        // malformed: 24-bit dest_qp. CURRENT BEHAVIOUR: the request keeps 16 bits, 0x010003 writes QP3
        {"STATE 59 RX write qpn 0x010003",    false, RX_IBH,  0x010003, true,  false, false, {0x000005, 0x000000, 0, 0, false, 0x000004, 0x000004, 0x000000C0, 0x000400, 0x000000, 0xFFFFFF, 0x000000, 2, 7, true}},
        {"STATE 60 RX read QP3",              false, RX_IBH,  3,        false, false, false, {0x000005, 0x000000, 0, 0, false, 0x000004, 0x000004, 0x000000C0, 0x000400, 0x000000, 0xFFFFFF, 0x000000, 2, 7, true}},

        // invalid QP (>= MAX_QPS): a write changes nothing, a read is still answered (or the client would wait forever).
        // 256, 0x0101, 0x0103 and 0xFFFF are QP 0, 1, 3 and 255 if only the low 8 bits are used
        {"STATE 61 RX write qpn 256",         false, RX_IBH,  256,      true,  false, false, {0x00ABCD, 0x000000, ap_uint<128>("0xFF"), ap_uint<128>("0xFF"), true, 0x00ABCD, 0x00ABCD, 0x0BADBEEF, 0x00ABCD, 0x000000, 0x000000, 0x000000, 1, 0, false}},
        {"STATE 62 CTRL write qpn 0x0101",    false, RX_CTRL, 0x0101,   true,  false, false, {0x000000, 0x000000, 0, 0, false, 0x000000, 0x000000, 0x00000000, 0x000000, 0x000000, 0x00DEAD, 0x000000, 0, 5, true}},
        {"STATE 63 TX write qpn 0x0103",      false, TX_IBH,  0x0103,   true,  false, false, {0x000000, 0x000000, 0, 0, false, 0x000000, 0x000000, 0x00000000, 0x000000, 0x00BEEF, 0x000000, 0x000000, 0, 0, false}},
        {"STATE 64 setup qpn 0xFFFF",         false, QPI,     0xFFFF,   true,  false, false, {0x0F0F0F, 0x0F0F0F, 0, 0, false, 0x0F0F0E, 0x0F0F0E, 0x00000000, 0x000000, 0x0E0E0E, 0x0E0E0E, 0x0E0E0E, 7, 0, false}},
        {"STATE 65 RX read qpn 256",          false, RX_IBH,  256,      false, false, false, {0x000000, 0x000000, 0, 0, false, 0x000000, 0x000000, 0x00000000, 0x000000, 0x000000, 0x000000, 0x000000, 0, 0, false}},
        {"STATE 66 CTRL read qpn 0x0101",     false, RX_CTRL, 0x0101,   false, false, false, {0x000000, 0x000000, 0, 0, false, 0x000000, 0x000000, 0x00000000, 0x000000, 0x000000, 0x000000, 0x000000, 0, 0, false}},
        {"STATE 67 TX read qpn 0x0103",       false, TX_IBH,  0x0103,   false, false, false, {0x000000, 0x000000, 0, 0, false, 0x000000, 0x000000, 0x00000000, 0x000000, 0x000000, 0x000000, 0x000000, 0, 0, false}},
        {"STATE 68 QPI read qpn 0xFFFF",      false, QPI,     0xFFFF,   false, false, false, {0x000000, 0x000000, 0, 0, false, 0x000000, 0x000000, 0x00000000, 0x000000, 0x000000, 0x000000, 0x000000, 0, 0, false}},
        {"STATE 69 QP0 untouched",            false, TX_IBH,  0,        false, false, false, {0x000011, 0x000010, 0, 0, false, 0x000010, 0x000010, 0x00000040, 0x000400, 0x000020, 0x000020, 0x000020, 4, 0, false}},
        {"STATE 70 QP1 untouched",            false, TX_IBH,  1,        false, false, false, {0x000200, 0x000200, 0, 0, false, 0x0001FF, 0x0001FF, 0x00000000, 0x000000, 0x000C01, 0x000ABC, 0x000C00, 7, 0, false}},
        {"STATE 71 QP3 untouched",            false, TX_IBH,  3,        false, false, false, {0x000005, 0x000000, 0, 0, false, 0x000004, 0x000004, 0x000000C0, 0x000400, 0x000000, 0xFFFFFF, 0x000000, 2, 7, true}},
        {"STATE 72 QP255 untouched",          false, TX_IBH,  255,      false, false, false, {0x0FF002, 0x0FF000, ap_uint<128>("0x80000000000000000000000000000002"), ap_uint<128>("0x80000000000000000000000000000000"), false, 0x0FF081, 0x0FF001, 0x00080000, 0x001000, 0x0FF100, 0x0FF101, 0x0FF100, 5, 1, false}},
    };
} // namespace

int runStateTableTest()
{
    StateTableTestBench tb;

    int failures = 0;
    size_t written = 0;
    const size_t testCaseCount = sizeof(stateTableTestCases) / sizeof(stateTableTestCases[0]);
    for (size_t tcIndex = 0; tcIndex < testCaseCount; ++tcIndex) {
        const StateTableTestCase& tc = stateTableTestCases[tcIndex];
        std::cout << "\n" << tc.name << "\n" << std::endl;

        // a row and the rows concurrent with it are written together, before the first call
        if (written == tcIndex) {
            do {
                const StateTableTestCase& request = stateTableTestCases[written];
                switch (request.port) {
                case RX_IBH:  tb.rxIbh2stateTable_upd_req.write(buildRxStateReq(request)); break;
                case RX_CTRL: tb.rxCtrl2stateTable_upd_req.write(request.write ? reqStateReq(request.qpn, request.entry.req_old_unack, request.entry.req_retries, request.entry.req_error) : reqStateReq(request.qpn)); break;
                case TX_IBH:  tb.txIbh2stateTable_upd_req.write(request.write ? txStateReq(request.qpn, request.entry.req_next_psn) : txStateReq(request.qpn)); break;
                case QPI:     tb.qpi2stateTable_upd_req.write(buildIfStateReq(request)); break;
                }
            } while (stateTableTestCases[written++].concurrent);
        }

        tb.tickOneCycle(); // one request per call
        failures += !checkStateTable(tb, tc);
    }

    return failures;
}