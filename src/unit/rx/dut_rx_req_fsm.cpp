#include <cstdint>
#include <iostream>
#include <string>

#include "rocev2_config.hpp"
#include "tests.hpp"

#include "mrc_transport_protocol/rx/rx_req_fsm.hpp"

// it receives:
// 1. metaIn            requestor-side control packets (ACK / NAK, SACK, NACK) from rx_ctrl_fsm
// 2. stateTable_rsp    requestor state of dest_qp: unack_psn, max_psn_sent, retries, error, valid
// it outputs
// 1. stateTable_req    the state read of dest_qp, then at most one write back
// 2. rxClearTimer_req  Local ACK timer stop / restart
// 3. rx2retrans_upd    release up to cack_psn (transport ACK only)
// 4. rx2retrans_req    retransmission of a NACKed PSN
// 5. errOut            QP in ERROR
// a packet for a QP whose state says valid = false produces only the read

namespace
{
    #define RX_REQ_FSM_INSTID 2 // own static state, not shared with the integration test (instances 0 and 1)

    struct ReqFsmTestCase
    {
        const char* name;

        // input: the packet
        uint8_t syndrome; // AETH: 0x1F ACK, 0x61 NAK Invalid Request
        uint32_t qpn;
        uint32_t psn;     // cack_psn of an ACK

        // input: the state of qpn
        uint32_t unack_psn;
        uint32_t max_psn_sent;
        bool valid;

        // expected outputs
        bool exp_state_write;
        bool exp_release;
        bool exp_timer;
        bool exp_error;
    };

    struct ReqFsmTestBench
    {
        hls::stream<mrcCtrlMeta<DATA_WIDTH> > metaIn;
        hls::stream<reqStateRsp>              stateTable_rsp;
        hls::stream<reqStateReq>              stateTable_req;
        hls::stream<rxTimerUpdate>            rxClearTimer_req;
        hls::stream<retransUpdate>            rx2retrans_upd;
        hls::stream<retransmission>           rx2retrans_req;
        hls::stream<reqErrEvent>              errOut;

        void tickOneCycle()
        {
            rx_req_fsm<DATA_WIDTH, RX_REQ_FSM_INSTID>(metaIn, stateTable_rsp, stateTable_req,
                                                      rxClearTimer_req, rx2retrans_upd, rx2retrans_req, errOut);
        }
    };

    bool runAndCheck(ReqFsmTestBench& tb, const ReqFsmTestCase& tc)
    {
        bool returnValue = true;
        auto fail = [&](const std::string& msg) {
            std::cerr << "[FAIL " << tc.name << "] " << msg << std::endl;
            returnValue = false;
        };
        auto expect = [&](const std::string& field, size_t got, size_t exp) {
            if (got != exp) fail(field + " " + std::to_string(got) + " != " + std::to_string(exp));
        };

        mrcCtrlMeta<DATA_WIDTH> m;
        m.op_code  = MRC_ACK;
        m.dest_qp  = tc.qpn;
        m.psn      = tc.psn;
        m.lengthOk = true;
        m.raw      = tc.syndrome;
        tb.metaIn.write(m);

        // LOAD: the read of dest_qp, answered like the state table does
        tb.tickOneCycle();
        expect("state reads", tb.stateTable_req.size(), 1);
        if (!tb.stateTable_req.empty()) {
            const reqStateReq read = tb.stateTable_req.read();
        if (read.write || read.qpn != (tc.qpn & 0xFFFF)) fail("first request is not the read of qpn");        }
        tb.stateTable_rsp.write(reqStateRsp(tc.unack_psn, tc.max_psn_sent, 0, false, tc.valid));

        // PROCESS
        tb.tickOneCycle();
        expect("state writes",    tb.stateTable_req.size(),   tc.exp_state_write);
        expect("releases",        tb.rx2retrans_upd.size(),   tc.exp_release);
        expect("timer updates",   tb.rxClearTimer_req.size(), tc.exp_timer);
        expect("error events",    tb.errOut.size(),           tc.exp_error);
        expect("retransmissions", tb.rx2retrans_req.size(),   0);
        if (!tb.stateTable_rsp.empty()) fail("state response not consumed");

        while (!tb.stateTable_req.empty())   tb.stateTable_req.read();
        while (!tb.rx2retrans_upd.empty())   tb.rx2retrans_upd.read();
        while (!tb.rxClearTimer_req.empty()) tb.rxClearTimer_req.read();
        while (!tb.errOut.empty())           tb.errOut.read();
        while (!tb.stateTable_rsp.empty())   tb.stateTable_rsp.read();

        return returnValue;
    }

    // each ACK / NAK is sent twice: once to a valid QP (the reference), once with the same state but valid = false
    const ReqFsmTestCase reqFsmTestCases[] = {
        // ACK of 0x102 with 0x100..0x105 outstanding: unack moves to 0x103, 0x100..0x102 released, timer restarted
        {.name = "REQ 01 ACK valid QP",     .syndrome = 0x1F, .qpn = 5,     .psn = 0x102, .unack_psn = 0x100, .max_psn_sent = 0x105, .valid = true,
         .exp_state_write = true,  .exp_release = true,  .exp_timer = true,  .exp_error = false},
        {.name = "REQ 02 ACK invalid QP",   .syndrome = 0x1F, .qpn = 6,     .psn = 0x102, .unack_psn = 0x100, .max_psn_sent = 0x105, .valid = false,
         .exp_state_write = false, .exp_release = false, .exp_timer = false, .exp_error = false},
        {.name = "REQ 03 ACK qpn 0x105",    .syndrome = 0x1F, .qpn = 0x105, .psn = 0x102, .unack_psn = 0x100, .max_psn_sent = 0x105, .valid = false,
         .exp_state_write = false, .exp_release = false, .exp_timer = false, .exp_error = false},

        // NAK Invalid Request: QP in ERROR, state written, timer stopped
        {.name = "REQ 04 NAK valid QP",     .syndrome = 0x61, .qpn = 5,     .psn = 0x103, .unack_psn = 0x100, .max_psn_sent = 0x105, .valid = true,
         .exp_state_write = true,  .exp_release = false, .exp_timer = true,  .exp_error = true},
        {.name = "REQ 05 NAK invalid QP",   .syndrome = 0x61, .qpn = 6,     .psn = 0x103, .unack_psn = 0x100, .max_psn_sent = 0x105, .valid = false,
         .exp_state_write = false, .exp_release = false, .exp_timer = false, .exp_error = false},
        // dest_qp 0x010005: the state read keeps 16 bits and gets QP 5's state (valid), but the QP does not exist
        {.name = "REQ 06 ACK qpn 0x010005", .syndrome = 0x1F, .qpn = 0x010005, .psn = 0x102, .unack_psn = 0x100, .max_psn_sent = 0x105, .valid = true,
         .exp_state_write = false, .exp_release = false, .exp_timer = false, .exp_error = false},
    };
} // namespace

int runRxReqFsmTest()
{
    ReqFsmTestBench tb;

    int failures = 0;
    const size_t testCaseCount = sizeof(reqFsmTestCases) / sizeof(reqFsmTestCases[0]);
    for (size_t tcIndex = 0; tcIndex < testCaseCount; ++tcIndex) {
        const ReqFsmTestCase& tc = reqFsmTestCases[tcIndex];
        std::cout << "\n" << tc.name << "\n" << std::endl;
        failures += !runAndCheck(tb, tc);
    }

    return failures;
}