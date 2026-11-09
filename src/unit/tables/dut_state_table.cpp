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
// 2. rxCtrl2stateTable_upd_req    requestor reliability state (req_old_unack, req_retries, error -> state ERROR), read or write
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
        
        // an RX / CTRL read must report valid = false: qpn >= MAX_QPS, never set up, or in ERROR
        bool exp_invalid;
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
            request.immmap            = testCase.entry.resp_immmap;
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

        // qpn >= MAX_QPS has no state: only valid = false is checked (TX and QPI reads: only the presence of the response)
        if (!tc.write && tc.qpn >= MAX_QPS) {
            if (tc.port == RX_IBH && !tb.stateTable2rxIbh_rsp.empty())  expect("valid", tb.stateTable2rxIbh_rsp.read().valid, !tc.exp_invalid);
            if (tc.port == RX_CTRL && !tb.stateTable2rxCtrl_rsp.empty()) expect("valid", tb.stateTable2rxCtrl_rsp.read().valid, !tc.exp_invalid);
        }

        const stateTableEntry& e = tc.entry;
        if (!tc.write && tc.qpn < MAX_QPS) {
            if (tc.port == RX_IBH && !tb.stateTable2rxIbh_rsp.empty()) {
                rxStateRsp r = tb.stateTable2rxIbh_rsp.read();
                if (tc.isResponse) {
                    expect("epsn (req_old_unack)",                   r.epsn,                   e.req_old_unack);
                    expect("oldest_outstanding_psn (req_old_valid)", r.oldest_outstanding_psn, e.req_old_valid);
                    expect("max_forward (req_next_psn - 1)",         r.max_forward,            (ap_uint<24>) (e.req_next_psn - 1));
                } else {
                    expect("valid",                  r.valid,                  !tc.exp_invalid);
                    expect("epsn",                   r.epsn,                   e.resp_epsn);                    
                    expect("oldest_outstanding_psn", r.oldest_outstanding_psn, e.resp_old_outstanding);
                    expect("max_forward",            r.max_forward,            e.resp_epsn);
                    expect("retryCounter",           r.retryCounter,           e.retryCounter);
                    expect("bitmap",                 r.bitmap,                 e.resp_bitmap);
                    expect("lastmap",                r.lastmap,                e.resp_lastmap);
                    expect("immmap",                 r.immmap,                 e.resp_immmap);
                    expect("prevAr",                 r.prevAr,                 e.resp_prev_ar);
                    expect("maxRcvPsn",              r.maxRcvPsn,              e.resp_max_rcv_psn);
                    expect("lowestUnsackedPsn",      r.lowestUnsackedPsn,      e.resp_lowest_unsacked_psn);
                    expect("rcvdBytes",              r.rcvdBytes,              e.resp_rcvd_bytes);
                    expect("sackTriggerCnt",         r.sackTriggerCnt,         e.resp_sack_trigger_cnt);
                }
            }
            if (tc.port == RX_CTRL && !tb.stateTable2rxCtrl_rsp.empty()) {
                reqStateRsp r = tb.stateTable2rxCtrl_rsp.read();
                expect("valid",        r.valid,        !tc.exp_invalid);
                expect("unack_psn",    r.unack_psn,    e.req_old_unack);
                expect("max_psn_sent", r.max_psn_sent, (ap_uint<24>) (e.req_next_psn - 1));
                expect("retries",      r.retries,      e.req_retries);
                expect("error",        r.error,        e.state == ERROR);
            }
            if ((tc.port == TX_IBH && !tb.stateTable2txIbh_rsp.empty()) || (tc.port == QPI && !tb.stateTable2qpi_rsp.empty())) {
                stateTableEntry s = tc.port == TX_IBH ? tb.stateTable2txIbh_rsp.read() : tb.stateTable2qpi_rsp.read();
                expect("resp_epsn",                s.resp_epsn,                e.resp_epsn);
                expect("resp_old_outstanding",     s.resp_old_outstanding,     e.resp_old_outstanding);
                expect("resp_bitmap",              s.resp_bitmap,              e.resp_bitmap);
                expect("resp_lastmap",             s.resp_lastmap,             e.resp_lastmap);
                expect("resp_immmap",              s.resp_immmap,              e.resp_immmap);
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
                expect("state",                    s.state,                    e.state);
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
    // entry: designated initializers in the order of stateTableEntry, a field that is not listed is 0 / false / RESET
    // QPI write: local_psn = resp_epsn, remote_psn = req_next_psn. Setup writes retryCounter = 0xF, 3 bits: 7 (CURRENT BEHAVIOUR)
    const StateTableTestCase stateTableTestCases[] = {
        // setup, then the same entry read through every port
        {.name = "STATE 01 setup QP1", .concurrent = false,
         .port = QPI, .qpn = 1, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000100, .resp_old_outstanding = 0x000100, .resp_max_rcv_psn = 0x0000FF, .resp_lowest_unsacked_psn = 0x0000FF, .req_next_psn = 0x000A00, .req_old_unack = 0x000A00, .req_old_valid = 0x000A00, .retryCounter = 7, .state = READY_SEND}},
        {.name = "STATE 02 QPI read QP1", .concurrent = false,
         .port = QPI, .qpn = 1, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000100, .resp_old_outstanding = 0x000100, .resp_max_rcv_psn = 0x0000FF, .resp_lowest_unsacked_psn = 0x0000FF, .req_next_psn = 0x000A00, .req_old_unack = 0x000A00, .req_old_valid = 0x000A00, .retryCounter = 7, .state = READY_SEND}},
        {.name = "STATE 03 TX read QP1", .concurrent = false,
         .port = TX_IBH, .qpn = 1, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000100, .resp_old_outstanding = 0x000100, .resp_max_rcv_psn = 0x0000FF, .resp_lowest_unsacked_psn = 0x0000FF, .req_next_psn = 0x000A00, .req_old_unack = 0x000A00, .req_old_valid = 0x000A00, .retryCounter = 7, .state = READY_SEND}},
        {.name = "STATE 04 RX read QP1", .concurrent = false,
         .port = RX_IBH, .qpn = 1, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000100, .resp_old_outstanding = 0x000100, .resp_max_rcv_psn = 0x0000FF, .resp_lowest_unsacked_psn = 0x0000FF, .req_next_psn = 0x000A00, .req_old_unack = 0x000A00, .req_old_valid = 0x000A00, .retryCounter = 7, .state = READY_SEND}},
        {.name = "STATE 05 CTRL read QP1", .concurrent = false,
         .port = RX_CTRL, .qpn = 1, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000100, .resp_old_outstanding = 0x000100, .resp_max_rcv_psn = 0x0000FF, .resp_lowest_unsacked_psn = 0x0000FF, .req_next_psn = 0x000A00, .req_old_unack = 0x000A00, .req_old_valid = 0x000A00, .retryCounter = 7, .state = READY_SEND}},

        // every port writes only its own fields (retryCounter belongs to RX, req_retries to CTRL).
        // A CTRL write with error puts the QP in ERROR: RX and CTRL reads report it invalid until the next full setup
        {.name = "STATE 06 RX write QP1", .concurrent = false,
         .port = RX_IBH, .qpn = 1, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000105, .resp_old_outstanding = 0x000100, .resp_bitmap = ap_uint<128>("0x0A"), .resp_lastmap = ap_uint<128>("0x08"), .resp_immmap = ap_uint<128>("0x08"), .resp_prev_ar = true, .resp_max_rcv_psn = 0x000108, .resp_lowest_unsacked_psn = 0x000104, .resp_rcvd_bytes = 0x00001234, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000A00, .req_old_unack = 0x000A00, .req_old_valid = 0x000A00, .retryCounter = 3, .state = READY_SEND}},
        {.name = "STATE 07 QPI read QP1", .concurrent = false,
         .port = QPI, .qpn = 1, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000105, .resp_old_outstanding = 0x000100, .resp_bitmap = ap_uint<128>("0x0A"), .resp_lastmap = ap_uint<128>("0x08"), .resp_immmap = ap_uint<128>("0x08"), .resp_prev_ar = true, .resp_max_rcv_psn = 0x000108, .resp_lowest_unsacked_psn = 0x000104, .resp_rcvd_bytes = 0x00001234, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000A00, .req_old_unack = 0x000A00, .req_old_valid = 0x000A00, .retryCounter = 3, .state = READY_SEND}},
        {.name = "STATE 08 CTRL write QP1", .concurrent = false,
         .port = RX_CTRL, .qpn = 1, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000105, .resp_old_outstanding = 0x000100, .resp_bitmap = ap_uint<128>("0x0A"), .resp_lastmap = ap_uint<128>("0x08"), .resp_immmap = ap_uint<128>("0x08"), .resp_prev_ar = true, .resp_max_rcv_psn = 0x000108, .resp_lowest_unsacked_psn = 0x000104, .resp_rcvd_bytes = 0x00001234, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000A00, .req_old_unack = 0x000A05, .req_old_valid = 0x000A00, .retryCounter = 3, .req_retries = 2, .state = ERROR}, .exp_invalid = true},
        {.name = "STATE 09 TX read QP1", .concurrent = false,
         .port = TX_IBH, .qpn = 1, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000105, .resp_old_outstanding = 0x000100, .resp_bitmap = ap_uint<128>("0x0A"), .resp_lastmap = ap_uint<128>("0x08"), .resp_immmap = ap_uint<128>("0x08"), .resp_prev_ar = true, .resp_max_rcv_psn = 0x000108, .resp_lowest_unsacked_psn = 0x000104, .resp_rcvd_bytes = 0x00001234, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000A00, .req_old_unack = 0x000A05, .req_old_valid = 0x000A00, .retryCounter = 3, .req_retries = 2, .state = ERROR}, .exp_invalid = true},
        {.name = "STATE 10 TX write QP1", .concurrent = false,
         .port = TX_IBH, .qpn = 1, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000105, .resp_old_outstanding = 0x000100, .resp_bitmap = ap_uint<128>("0x0A"), .resp_lastmap = ap_uint<128>("0x08"), .resp_immmap = ap_uint<128>("0x08"), .resp_prev_ar = true, .resp_max_rcv_psn = 0x000108, .resp_lowest_unsacked_psn = 0x000104, .resp_rcvd_bytes = 0x00001234, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000A10, .req_old_unack = 0x000A05, .req_old_valid = 0x000A00, .retryCounter = 3, .req_retries = 2, .state = ERROR}, .exp_invalid = true},
        {.name = "STATE 11 QPI read QP1", .concurrent = false,
         .port = QPI, .qpn = 1, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000105, .resp_old_outstanding = 0x000100, .resp_bitmap = ap_uint<128>("0x0A"), .resp_lastmap = ap_uint<128>("0x08"), .resp_immmap = ap_uint<128>("0x08"), .resp_prev_ar = true, .resp_max_rcv_psn = 0x000108, .resp_lowest_unsacked_psn = 0x000104, .resp_rcvd_bytes = 0x00001234, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000A10, .req_old_unack = 0x000A05, .req_old_valid = 0x000A00, .retryCounter = 3, .req_retries = 2, .state = ERROR}, .exp_invalid = true},
        {.name = "STATE 12 CTRL read QP1", .concurrent = false,
         .port = RX_CTRL, .qpn = 1, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000105, .resp_old_outstanding = 0x000100, .resp_bitmap = ap_uint<128>("0x0A"), .resp_lastmap = ap_uint<128>("0x08"), .resp_immmap = ap_uint<128>("0x08"), .resp_prev_ar = true, .resp_max_rcv_psn = 0x000108, .resp_lowest_unsacked_psn = 0x000104, .resp_rcvd_bytes = 0x00001234, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000A10, .req_old_unack = 0x000A05, .req_old_valid = 0x000A00, .retryCounter = 3, .req_retries = 2, .state = ERROR}, .exp_invalid = true},
        {.name = "STATE 13 RX read QP1", .concurrent = false,
         .port = RX_IBH, .qpn = 1, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000105, .resp_old_outstanding = 0x000100, .resp_bitmap = ap_uint<128>("0x0A"), .resp_lastmap = ap_uint<128>("0x08"), .resp_immmap = ap_uint<128>("0x08"), .resp_prev_ar = true, .resp_max_rcv_psn = 0x000108, .resp_lowest_unsacked_psn = 0x000104, .resp_rcvd_bytes = 0x00001234, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000A10, .req_old_unack = 0x000A05, .req_old_valid = 0x000A00, .retryCounter = 3, .req_retries = 2, .state = ERROR}, .exp_invalid = true},

        // remote psn only update on a QP with responder state: the requestor PSNs and retries are reset, the responder side is kept
        // and the QP stays in ERROR. Then a full setup (reconnection) resets everything and moves it back to READY_SEND
        {.name = "STATE 14 QPI selective QP1", .concurrent = false,
         .port = QPI, .qpn = 1, .write = true, .isResponse = false, .selective = true,
         .entry = {.resp_epsn = 0x000105, .resp_old_outstanding = 0x000100, .resp_bitmap = ap_uint<128>("0x0A"), .resp_lastmap = ap_uint<128>("0x08"), .resp_immmap = ap_uint<128>("0x08"), .resp_prev_ar = true, .resp_max_rcv_psn = 0x000108, .resp_lowest_unsacked_psn = 0x000104, .resp_rcvd_bytes = 0x00001234, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000B00, .req_old_unack = 0x000B00, .req_old_valid = 0x000B00, .retryCounter = 3, .state = ERROR}},
        {.name = "STATE 15 QPI read QP1", .concurrent = false,
         .port = QPI, .qpn = 1, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000105, .resp_old_outstanding = 0x000100, .resp_bitmap = ap_uint<128>("0x0A"), .resp_lastmap = ap_uint<128>("0x08"), .resp_immmap = ap_uint<128>("0x08"), .resp_prev_ar = true, .resp_max_rcv_psn = 0x000108, .resp_lowest_unsacked_psn = 0x000104, .resp_rcvd_bytes = 0x00001234, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000B00, .req_old_unack = 0x000B00, .req_old_valid = 0x000B00, .retryCounter = 3, .state = ERROR}},
        {.name = "STATE 16 setup QP1 again", .concurrent = false,
         .port = QPI, .qpn = 1, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000200, .resp_old_outstanding = 0x000200, .resp_max_rcv_psn = 0x0001FF, .resp_lowest_unsacked_psn = 0x0001FF, .req_next_psn = 0x000C00, .req_old_unack = 0x000C00, .req_old_valid = 0x000C00, .retryCounter = 7, .state = READY_SEND}},
        {.name = "STATE 17 TX read QP1", .concurrent = false,
         .port = TX_IBH, .qpn = 1, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000200, .resp_old_outstanding = 0x000200, .resp_max_rcv_psn = 0x0001FF, .resp_lowest_unsacked_psn = 0x0001FF, .req_next_psn = 0x000C00, .req_old_unack = 0x000C00, .req_old_valid = 0x000C00, .retryCounter = 7, .state = READY_SEND}},

        // first and last QP, every QP has its own entry
        {.name = "STATE 18 setup QP0", .concurrent = false,
         .port = QPI, .qpn = 0, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000010, .resp_old_outstanding = 0x000010, .resp_max_rcv_psn = 0x00000F, .resp_lowest_unsacked_psn = 0x00000F, .req_next_psn = 0x000020, .req_old_unack = 0x000020, .req_old_valid = 0x000020, .retryCounter = 7, .state = READY_SEND}},
        {.name = "STATE 19 setup QP255", .concurrent = false,
         .port = QPI, .qpn = 255, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x0FF000, .resp_old_outstanding = 0x0FF000, .resp_max_rcv_psn = 0x0FEFFF, .resp_lowest_unsacked_psn = 0x0FEFFF, .req_next_psn = 0x0FF100, .req_old_unack = 0x0FF100, .req_old_valid = 0x0FF100, .retryCounter = 7, .state = READY_SEND}},
        {.name = "STATE 20 RX write QP255", .concurrent = false,
         .port = RX_IBH, .qpn = 255, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x0FF002, .resp_old_outstanding = 0x0FF000, .resp_bitmap = ap_uint<128>("0x80000000000000000000000000000002"), .resp_lastmap = ap_uint<128>("0x80000000000000000000000000000000"), .resp_max_rcv_psn = 0x0FF081, .resp_lowest_unsacked_psn = 0x0FF001, .resp_rcvd_bytes = 0x00080000, .resp_sack_trigger_cnt = 0x001000, .req_next_psn = 0x0FF100, .req_old_unack = 0x0FF100, .req_old_valid = 0x0FF100, .retryCounter = 5, .state = READY_SEND}},
        {.name = "STATE 21 TX read QP0", .concurrent = false,
         .port = TX_IBH, .qpn = 0, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000010, .resp_old_outstanding = 0x000010, .resp_max_rcv_psn = 0x00000F, .resp_lowest_unsacked_psn = 0x00000F, .req_next_psn = 0x000020, .req_old_unack = 0x000020, .req_old_valid = 0x000020, .retryCounter = 7, .state = READY_SEND}},
        {.name = "STATE 22 TX read QP255", .concurrent = false,
         .port = TX_IBH, .qpn = 255, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x0FF002, .resp_old_outstanding = 0x0FF000, .resp_bitmap = ap_uint<128>("0x80000000000000000000000000000002"), .resp_lastmap = ap_uint<128>("0x80000000000000000000000000000000"), .resp_max_rcv_psn = 0x0FF081, .resp_lowest_unsacked_psn = 0x0FF001, .resp_rcvd_bytes = 0x00080000, .resp_sack_trigger_cnt = 0x001000, .req_next_psn = 0x0FF100, .req_old_unack = 0x0FF100, .req_old_valid = 0x0FF100, .retryCounter = 5, .state = READY_SEND}},

        // underflow: psn 0 - 1 wraps to 0xFFFFFF (setup with local psn 0, max_psn_sent with next psn 0).
        // overflow: every field at its maximum is stored and returned whole (bitmap bit 127, rcvdBytes bit 31)
        {.name = "STATE 23 setup QP3 psn 0", .concurrent = false,
         .port = QPI, .qpn = 3, .write = true, .isResponse = false, .selective = false, .entry = {.resp_max_rcv_psn = 0xFFFFFF, .resp_lowest_unsacked_psn = 0xFFFFFF, .retryCounter = 7, .state = READY_SEND}},
        {.name = "STATE 24 QPI read QP3", .concurrent = false,
         .port = QPI, .qpn = 3, .write = false, .isResponse = false, .selective = false, .entry = {.resp_max_rcv_psn = 0xFFFFFF, .resp_lowest_unsacked_psn = 0xFFFFFF, .retryCounter = 7, .state = READY_SEND}},
        {.name = "STATE 25 CTRL read next psn 0", .concurrent = false,
         .port = RX_CTRL, .qpn = 3, .write = false, .isResponse = false, .selective = false, .entry = {.resp_max_rcv_psn = 0xFFFFFF, .resp_lowest_unsacked_psn = 0xFFFFFF, .retryCounter = 7, .state = READY_SEND}},
        {.name = "STATE 26 RX write all ones", .concurrent = false,
         .port = RX_IBH, .qpn = 3, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0xFFFFFF, .resp_bitmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_lastmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_immmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_prev_ar = true, .resp_max_rcv_psn = 0xFFFFFF, .resp_lowest_unsacked_psn = 0xFFFFFF, .resp_rcvd_bytes = 0xFFFFFFFF, .resp_sack_trigger_cnt = 0xFFFFFF, .retryCounter = 7, .state = READY_SEND}},
        {.name = "STATE 27 RX read all ones", .concurrent = false,
         .port = RX_IBH, .qpn = 3, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0xFFFFFF, .resp_bitmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_lastmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_immmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_prev_ar = true, .resp_max_rcv_psn = 0xFFFFFF, .resp_lowest_unsacked_psn = 0xFFFFFF, .resp_rcvd_bytes = 0xFFFFFFFF, .resp_sack_trigger_cnt = 0xFFFFFF, .retryCounter = 7, .state = READY_SEND}},
        {.name = "STATE 28 CTRL write all ones", .concurrent = false,
         .port = RX_CTRL, .qpn = 3, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0xFFFFFF, .resp_bitmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_lastmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_immmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_prev_ar = true, .resp_max_rcv_psn = 0xFFFFFF, .resp_lowest_unsacked_psn = 0xFFFFFF, .resp_rcvd_bytes = 0xFFFFFFFF, .resp_sack_trigger_cnt = 0xFFFFFF, .req_old_unack = 0xFFFFFF, .retryCounter = 7, .req_retries = 7, .state = ERROR}, .exp_invalid = true},
        {.name = "STATE 29 TX write next psn max", .concurrent = false,
         .port = TX_IBH, .qpn = 3, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0xFFFFFF, .resp_bitmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_lastmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_immmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_prev_ar = true, .resp_max_rcv_psn = 0xFFFFFF, .resp_lowest_unsacked_psn = 0xFFFFFF, .resp_rcvd_bytes = 0xFFFFFFFF, .resp_sack_trigger_cnt = 0xFFFFFF, .req_next_psn = 0xFFFFFF, .req_old_unack = 0xFFFFFF, .retryCounter = 7, .req_retries = 7, .state = ERROR}, .exp_invalid = true},
        {.name = "STATE 30 CTRL read next psn max", .concurrent = false,
         .port = RX_CTRL, .qpn = 3, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0xFFFFFF, .resp_bitmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_lastmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_immmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_prev_ar = true, .resp_max_rcv_psn = 0xFFFFFF, .resp_lowest_unsacked_psn = 0xFFFFFF, .resp_rcvd_bytes = 0xFFFFFFFF, .resp_sack_trigger_cnt = 0xFFFFFF, .req_next_psn = 0xFFFFFF, .req_old_unack = 0xFFFFFF, .retryCounter = 7, .req_retries = 7, .state = ERROR}, .exp_invalid = true},
        {.name = "STATE 31 TX write next psn wrap", .concurrent = false,
         .port = TX_IBH, .qpn = 3, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0xFFFFFF, .resp_bitmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_lastmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_immmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_prev_ar = true, .resp_max_rcv_psn = 0xFFFFFF, .resp_lowest_unsacked_psn = 0xFFFFFF, .resp_rcvd_bytes = 0xFFFFFFFF, .resp_sack_trigger_cnt = 0xFFFFFF, .req_old_unack = 0xFFFFFF, .retryCounter = 7, .req_retries = 7, .state = ERROR}, .exp_invalid = true},
        {.name = "STATE 32 CTRL read next psn wrap", .concurrent = false,
         .port = RX_CTRL, .qpn = 3, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0xFFFFFF, .resp_bitmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_lastmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_immmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_prev_ar = true, .resp_max_rcv_psn = 0xFFFFFF, .resp_lowest_unsacked_psn = 0xFFFFFF, .resp_rcvd_bytes = 0xFFFFFFFF, .resp_sack_trigger_cnt = 0xFFFFFF, .req_old_unack = 0xFFFFFF, .retryCounter = 7, .req_retries = 7, .state = ERROR}, .exp_invalid = true},
        {.name = "STATE 33 TX read QP3", .concurrent = false,
         .port = TX_IBH, .qpn = 3, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0xFFFFFF, .resp_bitmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_lastmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_immmap = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .resp_prev_ar = true, .resp_max_rcv_psn = 0xFFFFFF, .resp_lowest_unsacked_psn = 0xFFFFFF, .resp_rcvd_bytes = 0xFFFFFFFF, .resp_sack_trigger_cnt = 0xFFFFFF, .req_old_unack = 0xFFFFFF, .retryCounter = 7, .req_retries = 7, .state = ERROR}, .exp_invalid = true},
       
        // QP3 is in ERROR from row 28: its RX and CTRL reads are invalid
        // concurrent requests: one served per call, rx > ctrl > tx > qpi, a read sees the writes served before it
        {.name = "STATE 34 setup QP2", .concurrent = false,
         .port = QPI, .qpn = 2, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000300, .resp_old_outstanding = 0x000300, .resp_max_rcv_psn = 0x0002FF, .resp_lowest_unsacked_psn = 0x0002FF, .req_next_psn = 0x000D00, .req_old_unack = 0x000D00, .req_old_valid = 0x000D00, .retryCounter = 7, .state = READY_SEND}},
        {.name = "STATE 35 all ports: RX write", .concurrent = true,
         .port = RX_IBH, .qpn = 2, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000301, .resp_old_outstanding = 0x000300, .resp_bitmap = ap_uint<128>("0x2"), .resp_lastmap = ap_uint<128>("0x2"), .resp_max_rcv_psn = 0x000302, .resp_lowest_unsacked_psn = 0x000300, .resp_rcvd_bytes = 0x00000840, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000D00, .req_old_unack = 0x000D00, .req_old_valid = 0x000D00, .retryCounter = 6, .state = READY_SEND}},
        {.name = "STATE 36 all ports: CTRL read", .concurrent = true,
         .port = RX_CTRL, .qpn = 2, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000301, .resp_old_outstanding = 0x000300, .resp_bitmap = ap_uint<128>("0x2"), .resp_lastmap = ap_uint<128>("0x2"), .resp_max_rcv_psn = 0x000302, .resp_lowest_unsacked_psn = 0x000300, .resp_rcvd_bytes = 0x00000840, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000D00, .req_old_unack = 0x000D00, .req_old_valid = 0x000D00, .retryCounter = 6, .state = READY_SEND}},
        {.name = "STATE 37 all ports: TX write", .concurrent = true,
         .port = TX_IBH, .qpn = 2, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000301, .resp_old_outstanding = 0x000300, .resp_bitmap = ap_uint<128>("0x2"), .resp_lastmap = ap_uint<128>("0x2"), .resp_max_rcv_psn = 0x000302, .resp_lowest_unsacked_psn = 0x000300, .resp_rcvd_bytes = 0x00000840, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000D08, .req_old_unack = 0x000D00, .req_old_valid = 0x000D00, .retryCounter = 6, .state = READY_SEND}},
        {.name = "STATE 38 all ports: QPI read", .concurrent = false,
         .port = QPI, .qpn = 2, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000301, .resp_old_outstanding = 0x000300, .resp_bitmap = ap_uint<128>("0x2"), .resp_lastmap = ap_uint<128>("0x2"), .resp_max_rcv_psn = 0x000302, .resp_lowest_unsacked_psn = 0x000300, .resp_rcvd_bytes = 0x00000840, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000D08, .req_old_unack = 0x000D00, .req_old_valid = 0x000D00, .retryCounter = 6, .state = READY_SEND}},
        // TX write already pending, the CTRL read is served first: it gets the old max_psn_sent
        {.name = "STATE 39 CTRL read before TX write", .concurrent = true,
         .port = RX_CTRL, .qpn = 2, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000301, .resp_old_outstanding = 0x000300, .resp_bitmap = ap_uint<128>("0x2"), .resp_lastmap = ap_uint<128>("0x2"), .resp_max_rcv_psn = 0x000302, .resp_lowest_unsacked_psn = 0x000300, .resp_rcvd_bytes = 0x00000840, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000D08, .req_old_unack = 0x000D00, .req_old_valid = 0x000D00, .retryCounter = 6, .state = READY_SEND}},
        {.name = "STATE 40 TX write after CTRL read", .concurrent = false,
         .port = TX_IBH, .qpn = 2, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000301, .resp_old_outstanding = 0x000300, .resp_bitmap = ap_uint<128>("0x2"), .resp_lastmap = ap_uint<128>("0x2"), .resp_max_rcv_psn = 0x000302, .resp_lowest_unsacked_psn = 0x000300, .resp_rcvd_bytes = 0x00000840, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000D10, .req_old_unack = 0x000D00, .req_old_valid = 0x000D00, .retryCounter = 6, .state = READY_SEND}},
        {.name = "STATE 41 CTRL read QP2", .concurrent = false,
         .port = RX_CTRL, .qpn = 2, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000301, .resp_old_outstanding = 0x000300, .resp_bitmap = ap_uint<128>("0x2"), .resp_lastmap = ap_uint<128>("0x2"), .resp_max_rcv_psn = 0x000302, .resp_lowest_unsacked_psn = 0x000300, .resp_rcvd_bytes = 0x00000840, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000D10, .req_old_unack = 0x000D00, .req_old_valid = 0x000D00, .retryCounter = 6, .state = READY_SEND}},
        // CTRL write and TX read pending together: the TX read already sees the write
        {.name = "STATE 42 CTRL write before TX read", .concurrent = true,
         .port = RX_CTRL, .qpn = 2, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000301, .resp_old_outstanding = 0x000300, .resp_bitmap = ap_uint<128>("0x2"), .resp_lastmap = ap_uint<128>("0x2"), .resp_max_rcv_psn = 0x000302, .resp_lowest_unsacked_psn = 0x000300, .resp_rcvd_bytes = 0x00000840, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000D10, .req_old_unack = 0x000D04, .req_old_valid = 0x000D00, .retryCounter = 6, .req_retries = 1, .state = READY_SEND}},
        {.name = "STATE 43 TX read after CTRL write", .concurrent = false,
         .port = TX_IBH, .qpn = 2, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000301, .resp_old_outstanding = 0x000300, .resp_bitmap = ap_uint<128>("0x2"), .resp_lastmap = ap_uint<128>("0x2"), .resp_max_rcv_psn = 0x000302, .resp_lowest_unsacked_psn = 0x000300, .resp_rcvd_bytes = 0x00000840, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000D10, .req_old_unack = 0x000D04, .req_old_valid = 0x000D00, .retryCounter = 6, .req_retries = 1, .state = READY_SEND}},
        // requests on the same port are served in order, a read right after a write of the same QP sees it,
        // the QPI read waits for all of them
        {.name = "STATE 44 RX queue: write", .concurrent = true,
         .port = RX_IBH, .qpn = 2, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000303, .resp_old_outstanding = 0x000300, .resp_max_rcv_psn = 0x000302, .resp_lowest_unsacked_psn = 0x000302, .resp_rcvd_bytes = 0x00001080, .resp_sack_trigger_cnt = 0x000800, .req_next_psn = 0x000D10, .req_old_unack = 0x000D04, .req_old_valid = 0x000D00, .retryCounter = 6, .req_retries = 1, .state = READY_SEND}},
        {.name = "STATE 45 RX queue: read", .concurrent = true,
         .port = RX_IBH, .qpn = 2, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000303, .resp_old_outstanding = 0x000300, .resp_max_rcv_psn = 0x000302, .resp_lowest_unsacked_psn = 0x000302, .resp_rcvd_bytes = 0x00001080, .resp_sack_trigger_cnt = 0x000800, .req_next_psn = 0x000D10, .req_old_unack = 0x000D04, .req_old_valid = 0x000D00, .retryCounter = 6, .req_retries = 1, .state = READY_SEND}},
        {.name = "STATE 46 RX queue: write", .concurrent = true,
         .port = RX_IBH, .qpn = 2, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000304, .resp_old_outstanding = 0x000300, .resp_bitmap = ap_uint<128>("0x4"), .resp_lastmap = ap_uint<128>("0x4"), .resp_immmap = ap_uint<128>("0x4"), .resp_prev_ar = true, .resp_max_rcv_psn = 0x000306, .resp_lowest_unsacked_psn = 0x000303, .resp_rcvd_bytes = 0x000018C0, .resp_sack_trigger_cnt = 0x000C00, .req_next_psn = 0x000D10, .req_old_unack = 0x000D04, .req_old_valid = 0x000D00, .retryCounter = 6, .req_retries = 1, .state = READY_SEND}},
        {.name = "STATE 47 RX queue: read", .concurrent = true,
         .port = RX_IBH, .qpn = 2, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000304, .resp_old_outstanding = 0x000300, .resp_bitmap = ap_uint<128>("0x4"), .resp_lastmap = ap_uint<128>("0x4"), .resp_immmap = ap_uint<128>("0x4"), .resp_prev_ar = true, .resp_max_rcv_psn = 0x000306, .resp_lowest_unsacked_psn = 0x000303, .resp_rcvd_bytes = 0x000018C0, .resp_sack_trigger_cnt = 0x000C00, .req_next_psn = 0x000D10, .req_old_unack = 0x000D04, .req_old_valid = 0x000D00, .retryCounter = 6, .req_retries = 1, .state = READY_SEND}},
        {.name = "STATE 48 RX queue: QPI read last", .concurrent = false,
         .port = QPI, .qpn = 2, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000304, .resp_old_outstanding = 0x000300, .resp_bitmap = ap_uint<128>("0x4"), .resp_lastmap = ap_uint<128>("0x4"), .resp_immmap = ap_uint<128>("0x4"), .resp_prev_ar = true, .resp_max_rcv_psn = 0x000306, .resp_lowest_unsacked_psn = 0x000303, .resp_rcvd_bytes = 0x000018C0, .resp_sack_trigger_cnt = 0x000C00, .req_next_psn = 0x000D10, .req_old_unack = 0x000D04, .req_old_valid = 0x000D00, .retryCounter = 6, .req_retries = 1, .state = READY_SEND}},
        // writes to different QPs at the same time, QP2 untouched. Then reads of different QPs at the same time
        {.name = "STATE 49 RX write QP0", .concurrent = true,
         .port = RX_IBH, .qpn = 0, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000011, .resp_old_outstanding = 0x000010, .resp_max_rcv_psn = 0x000010, .resp_lowest_unsacked_psn = 0x000010, .resp_rcvd_bytes = 0x00000040, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000020, .req_old_unack = 0x000020, .req_old_valid = 0x000020, .retryCounter = 4, .state = READY_SEND}},
        {.name = "STATE 50 CTRL write QP255", .concurrent = true,
         .port = RX_CTRL, .qpn = 255, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x0FF002, .resp_old_outstanding = 0x0FF000, .resp_bitmap = ap_uint<128>("0x80000000000000000000000000000002"), .resp_lastmap = ap_uint<128>("0x80000000000000000000000000000000"), .resp_max_rcv_psn = 0x0FF081, .resp_lowest_unsacked_psn = 0x0FF001, .resp_rcvd_bytes = 0x00080000, .resp_sack_trigger_cnt = 0x001000, .req_next_psn = 0x0FF100, .req_old_unack = 0x0FF101, .req_old_valid = 0x0FF100, .retryCounter = 5, .req_retries = 1, .state = READY_SEND}},
        {.name = "STATE 51 TX write QP1", .concurrent = true,
         .port = TX_IBH, .qpn = 1, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000200, .resp_old_outstanding = 0x000200, .resp_max_rcv_psn = 0x0001FF, .resp_lowest_unsacked_psn = 0x0001FF, .req_next_psn = 0x000C01, .req_old_unack = 0x000C00, .req_old_valid = 0x000C00, .retryCounter = 7, .state = READY_SEND}},
        {.name = "STATE 52 QPI read QP2", .concurrent = false,
         .port = QPI, .qpn = 2, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000304, .resp_old_outstanding = 0x000300, .resp_bitmap = ap_uint<128>("0x4"), .resp_lastmap = ap_uint<128>("0x4"), .resp_immmap = ap_uint<128>("0x4"), .resp_prev_ar = true, .resp_max_rcv_psn = 0x000306, .resp_lowest_unsacked_psn = 0x000303, .resp_rcvd_bytes = 0x000018C0, .resp_sack_trigger_cnt = 0x000C00, .req_next_psn = 0x000D10, .req_old_unack = 0x000D04, .req_old_valid = 0x000D00, .retryCounter = 6, .req_retries = 1, .state = READY_SEND}},
        {.name = "STATE 53 RX read QP0", .concurrent = true,
         .port = RX_IBH, .qpn = 0, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000011, .resp_old_outstanding = 0x000010, .resp_max_rcv_psn = 0x000010, .resp_lowest_unsacked_psn = 0x000010, .resp_rcvd_bytes = 0x00000040, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000020, .req_old_unack = 0x000020, .req_old_valid = 0x000020, .retryCounter = 4, .state = READY_SEND}},
        {.name = "STATE 54 CTRL read QP255", .concurrent = true,
         .port = RX_CTRL, .qpn = 255, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x0FF002, .resp_old_outstanding = 0x0FF000, .resp_bitmap = ap_uint<128>("0x80000000000000000000000000000002"), .resp_lastmap = ap_uint<128>("0x80000000000000000000000000000000"), .resp_max_rcv_psn = 0x0FF081, .resp_lowest_unsacked_psn = 0x0FF001, .resp_rcvd_bytes = 0x00080000, .resp_sack_trigger_cnt = 0x001000, .req_next_psn = 0x0FF100, .req_old_unack = 0x0FF101, .req_old_valid = 0x0FF100, .retryCounter = 5, .req_retries = 1, .state = READY_SEND}},
        {.name = "STATE 55 TX read QP1", .concurrent = false,
         .port = TX_IBH, .qpn = 1, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000200, .resp_old_outstanding = 0x000200, .resp_max_rcv_psn = 0x0001FF, .resp_lowest_unsacked_psn = 0x0001FF, .req_next_psn = 0x000C01, .req_old_unack = 0x000C00, .req_old_valid = 0x000C00, .retryCounter = 7, .state = READY_SEND}},

        // malformed: rx request with isResponse (not sent since step B). CURRENT BEHAVIOUR: the read returns the requestor
        // view, the write overwrites req_old_unack, which belongs to rx_req_fsm
        {.name = "STATE 56 RX isResponse read", .concurrent = false,
         .port = RX_IBH, .qpn = 1, .write = false, .isResponse = true, .selective = false,
         .entry = {.resp_epsn = 0x000200, .resp_old_outstanding = 0x000200, .resp_max_rcv_psn = 0x0001FF, .resp_lowest_unsacked_psn = 0x0001FF, .req_next_psn = 0x000C01, .req_old_unack = 0x000C00, .req_old_valid = 0x000C00, .retryCounter = 7, .state = READY_SEND}},
        {.name = "STATE 57 RX isResponse write", .concurrent = false,
         .port = RX_IBH, .qpn = 1, .write = true, .isResponse = true, .selective = false,
         .entry = {.resp_epsn = 0x000200, .resp_old_outstanding = 0x000200, .resp_max_rcv_psn = 0x0001FF, .resp_lowest_unsacked_psn = 0x0001FF, .req_next_psn = 0x000C01, .req_old_unack = 0x000ABC, .req_old_valid = 0x000C00, .retryCounter = 7, .state = READY_SEND}},
        {.name = "STATE 58 CTRL read QP1", .concurrent = false,
         .port = RX_CTRL, .qpn = 1, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000200, .resp_old_outstanding = 0x000200, .resp_max_rcv_psn = 0x0001FF, .resp_lowest_unsacked_psn = 0x0001FF, .req_next_psn = 0x000C01, .req_old_unack = 0x000ABC, .req_old_valid = 0x000C00, .retryCounter = 7, .state = READY_SEND}},
        // malformed: 24-bit dest_qp. CURRENT BEHAVIOUR: the request keeps 16 bits, 0x010003 writes QP3
        {.name = "STATE 59 RX write qpn 0x010003", .concurrent = false,
         .port = RX_IBH, .qpn = 0x010003, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000005, .resp_max_rcv_psn = 0x000004, .resp_lowest_unsacked_psn = 0x000004, .resp_rcvd_bytes = 0x000000C0, .resp_sack_trigger_cnt = 0x000400, .req_old_unack = 0xFFFFFF, .retryCounter = 2, .req_retries = 7, .state = ERROR}, .exp_invalid = true},
        {.name = "STATE 60 RX read QP3", .concurrent = false,
         .port = RX_IBH, .qpn = 3, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000005, .resp_max_rcv_psn = 0x000004, .resp_lowest_unsacked_psn = 0x000004, .resp_rcvd_bytes = 0x000000C0, .resp_sack_trigger_cnt = 0x000400, .req_old_unack = 0xFFFFFF, .retryCounter = 2, .req_retries = 7, .state = ERROR}, .exp_invalid = true},

        // invalid QP (>= MAX_QPS): a write changes nothing, a read is still answered (or the client would wait forever).
        // 256, 0x0101, 0x0103 and 0xFFFF are QP 0, 1, 3 and 255 if only the low 8 bits are used
        {.name = "STATE 61 RX write qpn 256", .concurrent = false,
         .port = RX_IBH, .qpn = 256, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x00ABCD, .resp_bitmap = ap_uint<128>("0xFF"), .resp_lastmap = ap_uint<128>("0xFF"), .resp_immmap = ap_uint<128>("0xFF"), .resp_prev_ar = true, .resp_max_rcv_psn = 0x00ABCD, .resp_lowest_unsacked_psn = 0x00ABCD, .resp_rcvd_bytes = 0x0BADBEEF, .resp_sack_trigger_cnt = 0x00ABCD, .retryCounter = 1}},
        {.name = "STATE 62 CTRL write qpn 0x0101", .concurrent = false,
         .port = RX_CTRL, .qpn = 0x0101, .write = true, .isResponse = false, .selective = false, .entry = {.req_old_unack = 0x00DEAD, .req_retries = 5, .state = ERROR}, .exp_invalid = true},
        {.name = "STATE 63 TX write qpn 0x0103", .concurrent = false,
         .port = TX_IBH, .qpn = 0x0103, .write = true, .isResponse = false, .selective = false, .entry = {.req_next_psn = 0x00BEEF}},
        {.name = "STATE 64 setup qpn 0xFFFF", .concurrent = false,
         .port = QPI, .qpn = 0xFFFF, .write = true, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x0F0F0F, .resp_old_outstanding = 0x0F0F0F, .resp_max_rcv_psn = 0x0F0F0E, .resp_lowest_unsacked_psn = 0x0F0F0E, .req_next_psn = 0x0E0E0E, .req_old_unack = 0x0E0E0E, .req_old_valid = 0x0E0E0E, .retryCounter = 7}},
        {.name = "STATE 65 RX read qpn 256", .concurrent = false,
         .port = RX_IBH, .qpn = 256, .write = false, .isResponse = false, .selective = false, .entry = {}, .exp_invalid = true},
        {.name = "STATE 66 CTRL read qpn 0x0101", .concurrent = false,
         .port = RX_CTRL, .qpn = 0x0101, .write = false, .isResponse = false, .selective = false, .entry = {}, .exp_invalid = true},
        {.name = "STATE 67 TX read qpn 0x0103", .concurrent = false,
         .port = TX_IBH, .qpn = 0x0103, .write = false, .isResponse = false, .selective = false, .entry = {}},
        {.name = "STATE 68 QPI read qpn 0xFFFF", .concurrent = false,
         .port = QPI, .qpn = 0xFFFF, .write = false, .isResponse = false, .selective = false, .entry = {}},
        {.name = "STATE 69 QP0 untouched", .concurrent = false,
         .port = TX_IBH, .qpn = 0, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000011, .resp_old_outstanding = 0x000010, .resp_max_rcv_psn = 0x000010, .resp_lowest_unsacked_psn = 0x000010, .resp_rcvd_bytes = 0x00000040, .resp_sack_trigger_cnt = 0x000400, .req_next_psn = 0x000020, .req_old_unack = 0x000020, .req_old_valid = 0x000020, .retryCounter = 4, .state = READY_SEND}},
        {.name = "STATE 70 QP1 untouched", .concurrent = false,
         .port = TX_IBH, .qpn = 1, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000200, .resp_old_outstanding = 0x000200, .resp_max_rcv_psn = 0x0001FF, .resp_lowest_unsacked_psn = 0x0001FF, .req_next_psn = 0x000C01, .req_old_unack = 0x000ABC, .req_old_valid = 0x000C00, .retryCounter = 7, .state = READY_SEND}},
        {.name = "STATE 71 QP3 untouched", .concurrent = false,
         .port = TX_IBH, .qpn = 3, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x000005, .resp_max_rcv_psn = 0x000004, .resp_lowest_unsacked_psn = 0x000004, .resp_rcvd_bytes = 0x000000C0, .resp_sack_trigger_cnt = 0x000400, .req_old_unack = 0xFFFFFF, .retryCounter = 2, .req_retries = 7, .state = ERROR}, .exp_invalid = true},
        {.name = "STATE 72 QP255 untouched", .concurrent = false,
         .port = TX_IBH, .qpn = 255, .write = false, .isResponse = false, .selective = false,
         .entry = {.resp_epsn = 0x0FF002, .resp_old_outstanding = 0x0FF000, .resp_bitmap = ap_uint<128>("0x80000000000000000000000000000002"), .resp_lastmap = ap_uint<128>("0x80000000000000000000000000000000"), .resp_max_rcv_psn = 0x0FF081, .resp_lowest_unsacked_psn = 0x0FF001, .resp_rcvd_bytes = 0x00080000, .resp_sack_trigger_cnt = 0x001000, .req_next_psn = 0x0FF100, .req_old_unack = 0x0FF101, .req_old_valid = 0x0FF100, .retryCounter = 5, .req_retries = 1, .state = READY_SEND}},
        // a QP in range that was never set up is invalid too (QP 4: no setup row)
        {.name = "STATE 73 RX read QP4 never set up", .concurrent = false,
         .port = RX_IBH, .qpn = 4, .write = false, .isResponse = false, .selective = false, .entry = {}, .exp_invalid = true},
        {.name = "STATE 74 CTRL read QP4 never set up", .concurrent = false,
         .port = RX_CTRL, .qpn = 4, .write = false, .isResponse = false, .selective = false, .entry = {}, .exp_invalid = true},
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
                case RX_CTRL: tb.rxCtrl2stateTable_upd_req.write(request.write ? reqStateReq(request.qpn, request.entry.req_old_unack, request.entry.req_retries, request.entry.state == ERROR) : reqStateReq(request.qpn)); break;
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