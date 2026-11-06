#pragma once

#include <vector>
#include "util.hpp"
#include "mrc_transport_protocol/mrc_headers.hpp"

namespace 
{
    // requestor
    #define REMOTE_QPN 0x0042
    #define REMOTE_IP 0x0A000002 // 10.0.0.2
    #define REMOTE_UDP_PORT 0xC000

    #define RKEY 0x1234
    #define FIRST_RX_PSN_0 0x0A0B00 // first rx psn for the first QP tested
    #define FIRST_RX_PSN_3 0x200000

    #define PMTU 1024

    #define VERY_BIG_NUMBER 1000

    // local QPs acting as responder (send writes)
    #define RSP_QP_0 0x08   // write 1
    #define RSP_QP_1 0x09   // write 2
    #define RSP_QP_2 0x0A   // write 3
    #define RSP_QP_3 0x10   // write with immediate, LAST corner cases, zero length (fresh QP)
    // local QPs acting as requestor (send ACK/SACK/NACK)
    #define REQ_QP_0 0x0B   // ACK/SACK/NACK
    #define REQ_QP_1 0x0C   // NACK retry limit (qp should go to error state)
    #define REQ_QP_2 0x0D   // AETH NAK (qp should go to error state)
    #define REQ_QP_3 0x0E   // NACK UNEXP_EVENT (qp should go to error state)
    #define REQ_QP_4 0x0F   // 24-bit PSN wrap

    #define REQ_PSN 0x000200   // first PSN sent by RSP_QP_2..D
    #define REQ_PSN_F 0xFFFFFE // first PSN sent by REQ_QP_3
    #define REQ_WRITES 6

    // how many cycles before feeding next packet
    // 100 may be not random enough 
    enum : int32_t { DRAIN = -1, BACK_TO_BACK = 0, RETRANSMIT = 100 }; 
    
    struct WriteTestCase
    {
        // misc
        const char* name;

        // BTH
        uint8_t opcode;
        uint16_t p_key;
        uint32_t dqp;   // 24 bits
        uint8_t a;      // 1 bit
        uint8_t R;      // 1 bit
        uint8_t rtx;    // 1 bit
        uint8_t ts;     // 1 bit
        uint32_t psn;   // 24 bits

        // METH
        uint16_t rqmsn;
        uint16_t msn;

        // [TSETH]
        uint16_t tx_timestamp;
        uint8_t tsr;    // 1 bit
        uint8_t ftype;  // 4 bits

        // RETH
        uint64_t va;
        uint32_t r_key;
        uint32_t dmalen;
        
        // ImmDt
        uint64_t immDt;

        // payload
        std::vector<uint8_t> payload; 

        // expectations
        bool exp_accepted;
        bool exp_ack;
        uint32_t exp_cack_psn; 
        uint32_t exp_ack_msn;
        uint8_t exp_syndrome;
        
        // how many cycles before feeding next packet
        int32_t after_cycles;
    };

    struct AckTestCase 
    { 
        // misc
        const char* name;

        // BTH
        uint8_t opcode;
        uint16_t p_key;
        uint32_t dqp;   // 24 bits
        uint8_t a;      // 1 bit
        uint8_t R;      // 1 bit
        uint8_t rtx;    // 1 bit
        uint8_t ts;     // 1 bit
        uint32_t psn;   // 24 bits

        // AETH
        uint8_t syndrome;
        uint32_t msn;   // 24 bits

        // expectations
        uint32_t exp_completions; // ackMeta on rx_ack_meta, one per released PSN

        // how many cycles before feeding next packet
        int32_t after_cycles;
    };

    struct SackTestCase 
    {
        // misc
        const char* name;

        // BTH
        uint8_t opcode;
        uint16_t p_key;
        uint32_t dqp;   // 24 bits
        uint8_t a;      // 1 bit
        uint8_t R;      // 1 bit
        uint8_t rtx;    // 1 bit
        uint8_t ts;     // 1 bit
        uint32_t psn;   // 24 bits

        // SACK
        // type=cm
        // res (nxt=sack)
        uint8_t m;      // 2 bits
        uint8_t pr;     // 1 bit
        int16_t ack_psn_offset;
        uint32_t entropy;
        uint16_t spdcid;
        uint16_t dpdcid;
        uint32_t cack_psn;  // 24 bits
        uint8_t cc_type;    // 4 bits
        uint8_t cc_fl;      // 4 bits
        uint8_t mpr;
        int16_t sack_offset;
        uint64_t sack_bitmap;

        // CC
        uint16_t tx_timestamp;
        uint16_t ooo_count;     // 15 bits
        uint8_t r_c;            // 1 bit
        uint8_t rcv_cwnd_pen;   // 7 bits
        uint32_t rcvd_bytes;    // 24 bits

        // how many cycles before feeding next packet
        int32_t after_cycles;
    };

    struct NackTestCase 
    {
        // misc
        const char* name;

        // BTH
        uint8_t opcode;
        uint16_t p_key;
        uint32_t dqp;   // 24 bits
        uint8_t a;      // 1 bit
        uint8_t R;      // 1 bit
        uint8_t rtx;    // 1 bit
        uint8_t ts;     // 1 bit
        uint32_t psn;   // 24 bits

        // NETH
        uint8_t nack_reason;
        uint8_t vendor_info;
        uint32_t entropy;
        uint16_t spdcid;
        uint16_t dpdcid;
        uint32_t nack_psn;  // 24 bits
        uint8_t cc_type;    // 4 bits
        uint8_t cc_fl;      // 4 bits
        uint16_t tx_timestamp;

        // expectations
        bool exp_retransmit; // nack_psn is sent again

        // how many cycles before feeding next packet
        int32_t after_cycles;
    };

    struct ProbeTestCase 
    {
        // misc
        const char* name;

        // BTH
        uint8_t opcode;
        uint16_t p_key;
        uint32_t dqp;   // 24 bits
        uint8_t a;      // 1 bit
        uint8_t R;      // 1 bit
        uint8_t rtx;    // 1 bit
        uint8_t ts;     // 1 bit
        uint32_t psn;   // 24 bits

        // PETH
        uint8_t vendor_info;
        uint16_t probe_id;
        uint16_t spdcid;
        uint16_t dpdcid;
        uint16_t tx_timestamp;
        uint8_t tsr;    // 1 bit
        uint8_t ftype;  // 4 bits

        // how many cycles before feeding next packet
        int32_t after_cycles;
    };

    struct EndpointReqTestCase 
    {
        // misc
        const char* name;

        // BTH
        uint8_t opcode;
        uint16_t p_key;
        uint32_t dqp;   // 24 bits
        uint8_t a;      // 1 bit
        uint8_t R;      // 1 bit
        uint8_t rtx;    // 1 bit
        uint8_t ts;     // 1 bit
        uint32_t psn;   // 24 bits

        // ERTH
        uint8_t op;     // 2 bits
        uint8_t vendor_info;
        uint32_t port_status_mask;
        uint16_t tx_timestamp;
        uint8_t tsr;    // 1 bit
        uint8_t ftype;  // 4 bits

        // how many cycles before feeding next packet
        int32_t after_cycles;
    };

    struct EndpointResTestCase 
    {
        // misc
        const char* name;

        // BTH
        uint8_t opcode;
        uint16_t p_key;
        uint32_t dqp;   // 24 bits
        uint8_t a;      // 1 bit
        uint8_t R;      // 1 bit
        uint8_t rtx;    // 1 bit
        uint8_t ts;     // 1 bit
        uint32_t psn;   // 24 bits

        // EETH
        uint8_t op;     // 2 bits
        uint16_t tx_timestamp;

        // how many cycles before feeding next packet
        int32_t after_cycles;
    };

    enum class TestCaseKind : uint8_t { WRITE, ACK, SACK, NACK, PROBE, ENDPOINT_REQ, ENDPOINT_RES };
    struct TestCase 
    {
        TestCaseKind kind;

        WriteTestCase write;
        AckTestCase ack;
        SackTestCase sack;
        NackTestCase nack;
        ProbeTestCase probe;
        EndpointReqTestCase endpoint_req;
        EndpointResTestCase endpoint_res;
    };

    TestCase WRITE_TC(const WriteTestCase& t) { TestCase c = TestCase(); c.kind = TestCaseKind::WRITE; c.write = t; return c; }
    TestCase ACK_TC(const AckTestCase& t) { TestCase c = TestCase(); c.kind = TestCaseKind::ACK; c.ack = t; return c; }
    TestCase SACK_TC(const SackTestCase& t) { TestCase c = TestCase(); c.kind = TestCaseKind::SACK; c.sack = t; return c; }
    TestCase NACK_TC(const NackTestCase& t) { TestCase c = TestCase(); c.kind = TestCaseKind::NACK; c.nack = t; return c; }
    TestCase PROBE_TC(const ProbeTestCase& t) { TestCase c = TestCase(); c.kind = TestCaseKind::PROBE; c.probe = t; return c; }
    TestCase ENDPOINT_REQ_TC(const EndpointReqTestCase& t) { TestCase c = TestCase(); c.kind = TestCaseKind::ENDPOINT_REQ; c.endpoint_req = t; return c; }
    TestCase ENDPOINT_RES_TC(const EndpointResTestCase& t) { TestCase c = TestCase(); c.kind = TestCaseKind::ENDPOINT_RES; c.endpoint_res = t; return c; }

    const TestCase testCases[] = {
        // REQUESTOR SIDE. Each REQ_QP_x posted REQ_WRITES WRITE ONLY (lst = 1) at start-up, so PSNs REQ_PSN..REQ_PSN+5
        // are outstanding (0xFFFFFE..0x000003 for REQ_QP_4). One single group: the Local ACK timer of every QP is
        // stopped at its end (all acknowledged or QP in ERROR), so it can never retransmit during the write tests.

        // QP B. SACK: moves unack_psn (SETH.cack_psn, not BTH.psn), never completes. ACK: completes every psn <= BTH.psn.
        // RNR NAK / reserved AETH types are ignored, a cack_psn never sent is ignored, a stale one completes nothing.
        SACK_TC({"MRC SACK B01", MRC_SACK, 0xFFFF, REQ_QP_0, 0,0,0,0, 0x00ABCD,  SACK_M_ALWAYS_SKIP, 0, -1, 0xDEADBEEF, REQ_QP_0, REMOTE_QPN+3, REQ_PSN+1, MRC_CC_TYPE_DEFAULT, 0xF, 0x04, -3, 0xF0F0F0F0F0F0F0F0ULL, 0xBEEF, 0x7FFF, 1, 0x7F, 0xABCDEF, BACK_TO_BACK}),
        NACK_TC({"MRC NACK B02", MRC_NACK, 0xFFFF, REQ_QP_0, 0,0,0,0, REQ_PSN+1, NACK_TRIMMED,         0, 0x1234, REQ_QP_0, REMOTE_QPN+3, REQ_PSN+1, 0x2, 0, 0x0000, false, BACK_TO_BACK}),   // SACKed
        NACK_TC({"MRC NACK B03", MRC_NACK, 0xFFFF, REQ_QP_0, 0,0,0,0, REQ_PSN+0, NACK_NO_BITMAP,       0, 0x1234, REQ_QP_0, REMOTE_QPN+3, REQ_PSN+2, 0x2, 0, 0x1111, true,  RETRANSMIT}),     // NETH.nack_psn wins over BTH.psn
        ACK_TC ({"MRC ACK B04",  MRC_ACK,  0xFFFF, REQ_QP_0, 0,0,0,0, REQ_PSN+2, 0x1F, 0x777, 3, BACK_TO_BACK}),   // +0 and +1 were only SACKed
        ACK_TC ({"MRC ACK B05",  MRC_ACK,  0xFFFF, REQ_QP_0, 0,0,0,0, REQ_PSN+1, 0x1F, 0,     0, BACK_TO_BACK}),   // stale
        ACK_TC ({"MRC ACK B06",  MRC_ACK,  0xFFFF, REQ_QP_0, 0,0,0,0, REQ_PSN+5, 0x21, 0,     0, BACK_TO_BACK}),   // RNR NAK
        ACK_TC ({"MRC ACK B07",  MRC_ACK,  0xFFFF, REQ_QP_0, 0,0,0,0, REQ_PSN+5, 0x40, 0,     0, BACK_TO_BACK}),   // reserved type
        SACK_TC({"MRC SACK B08", MRC_SACK, 0xFFFF, REQ_QP_0, 0,0,0,0, REQ_PSN+9,  SACK_M_NONE, 0, 0, 0, REQ_QP_0, REMOTE_QPN+3, REQ_PSN+9, MRC_CC_TYPE_DEFAULT, 0, 0, 0, 0, 0, 0, 0, 0, 0, BACK_TO_BACK}), // never sent
        NACK_TC({"MRC NACK B09", MRC_NACK, 0xFFFF, REQ_QP_0, 0,0,0,0, REQ_PSN+3, NACK_NO_PKT_BUFFER,   0, 0x1234, REQ_QP_0, REMOTE_QPN+3, REQ_PSN+3, 0x2, 0, 0x0000, true,  RETRANSMIT}),     // B08 did not move unack_psn
        NACK_TC({"MRC NACK B10", MRC_NACK, 0xFFFF, REQ_QP_0, 0,0,0,0, REQ_PSN+4, 0x03,                 0, 0x1234, REQ_QP_0, REMOTE_QPN+3, REQ_PSN+4, 0x2, 0, 0x0000, false, BACK_TO_BACK}),   // reserved reason
        NACK_TC({"MRC NACK B11", MRC_NACK, 0xFFFF, REQ_QP_0, 0,0,0,0, REQ_PSN+6, NACK_TRIMMED,         0, 0x1234, REQ_QP_0, REMOTE_QPN+3, REQ_PSN+6, 0x2, 0, 0x0000, false, BACK_TO_BACK}),   // never sent
        ACK_TC ({"MRC ACK B12",  MRC_ACK,  0xFFFF, REQ_QP_0, 0,0,0,0, REQ_PSN+6, 0x1F, 0,     0, BACK_TO_BACK}),   // never sent

        // QP D. AETH NAK (Class B): QP to ERROR, no retransmission; in ERROR every ACK / NACK is ignored
        ACK_TC ({"MRC ACK D01",  MRC_ACK,  0xFFFF, REQ_QP_2, 0,0,0,0, REQ_PSN+0, 0x21, 0, 0, BACK_TO_BACK}),       // RNR NAK
        ACK_TC ({"MRC ACK D02",  MRC_ACK,  0xFFFF, REQ_QP_2, 0,0,0,0, REQ_PSN+0, 0x1F, 0, 1, BACK_TO_BACK}),
        NACK_TC({"MRC NACK D03", MRC_NACK, 0xFFFF, REQ_QP_2, 0,0,0,0, REQ_PSN+1, NACK_TRIMMED_LASTHOP, 0, 0x5678, REQ_QP_2, REMOTE_QPN+5, REQ_PSN+1, 0x2, 0, 0x0000, true,  RETRANSMIT}),
        ACK_TC ({"MRC NAK D04",  MRC_ACK,  0xFFFF, REQ_QP_2, 0,0,0,0, REQ_PSN+1, 0x60 | AETH_NAK_REMOTE_ACCESS, 0, 0, BACK_TO_BACK}),   // -> ERROR
        NACK_TC({"MRC NACK D05", MRC_NACK, 0xFFFF, REQ_QP_2, 0,0,0,0, REQ_PSN+1, NACK_TRIMMED,         0, 0x5678, REQ_QP_2, REMOTE_QPN+5, REQ_PSN+1, 0x2, 0, 0x0000, false, BACK_TO_BACK}),
        ACK_TC ({"MRC ACK D06",  MRC_ACK,  0xFFFF, REQ_QP_2, 0,0,0,0, REQ_PSN+5, 0x1F, 0, 0, BACK_TO_BACK}),

        // QP E. NACK UNEXP_EVENT: QP to ERROR. CURRENT BEHAVIOUR: on a psn never sent it is ignored and the QP stays usable.
        // E02 SACKs a psn that is never ACKed (the QP ends in ERROR): a completion caused by the SACK would be left over
        NACK_TC({"MRC NACK E01", MRC_NACK, 0xFFFF, REQ_QP_3, 0,0,0,0, REQ_PSN+7, NACK_UNEXP_EVENT,     0, 0x9ABC, REQ_QP_3, REMOTE_QPN+6, REQ_PSN+7, 0x2, 0, 0x0000, false, BACK_TO_BACK}),
        SACK_TC({"MRC SACK E02", MRC_SACK, 0xFFFF, REQ_QP_3, 0,0,0,0, REQ_PSN+0, SACK_M_NONE, 0, 0, 0x9ABC, REQ_QP_3, REMOTE_QPN+6, REQ_PSN+0, MRC_CC_TYPE_DEFAULT, 0, 0, 0, 0, 0, 0, 0, 0, 0, BACK_TO_BACK}),
        NACK_TC({"MRC NACK E03", MRC_NACK, 0xFFFF, REQ_QP_3, 0,0,0,0, REQ_PSN+1, NACK_NO_RESOURCE,     0, 0x9ABC, REQ_QP_3, REMOTE_QPN+6, REQ_PSN+1, 0x2, 0, 0x0000, true,  RETRANSMIT}),
        NACK_TC({"MRC NACK E04", MRC_NACK, 0xFFFF, REQ_QP_3, 0,0,0,0, REQ_PSN+2, NACK_UNEXP_EVENT,     0, 0x9ABC, REQ_QP_3, REMOTE_QPN+6, REQ_PSN+2, 0x2, 0, 0x0000, false, BACK_TO_BACK}),   // -> ERROR
        NACK_TC({"MRC NACK E05", MRC_NACK, 0xFFFF, REQ_QP_3, 0,0,0,0, REQ_PSN+2, NACK_TRIMMED,         0, 0x9ABC, REQ_QP_3, REMOTE_QPN+6, REQ_PSN+2, 0x2, 0, 0x0000, false, BACK_TO_BACK}),
        ACK_TC ({"MRC ACK E06",  MRC_ACK,  0xFFFF, REQ_QP_3, 0,0,0,0, REQ_PSN+5, 0x1F, 0, 0, BACK_TO_BACK}),

        // QP F. Outstanding check, SACK and release across the 24-bit wrap
        NACK_TC({"MRC NACK F01", MRC_NACK, 0xFFFF, REQ_QP_4, 0,0,0,0, 0x000000, NACK_PSN_OOR_WINDOW,   0, 0xDEF0, REQ_QP_4, REMOTE_QPN+7, 0x000000, 0x2, 0, 0x0000, true,  RETRANSMIT}),
        SACK_TC({"MRC SACK F02", MRC_SACK, 0xFFFF, REQ_QP_4, 0,0,0,0, 0xFFFFFF, SACK_M_SKIP, 0, 0, 0xDEF0, REQ_QP_4, REMOTE_QPN+7, 0xFFFFFF, MRC_CC_TYPE_DEFAULT, 0, 0, 0, 0, 0, 0, 0, 0, 0, BACK_TO_BACK}),
        NACK_TC({"MRC NACK F03", MRC_NACK, 0xFFFF, REQ_QP_4, 0,0,0,0, 0xFFFFFF, NACK_TRIMMED,          0, 0xDEF0, REQ_QP_4, REMOTE_QPN+7, 0xFFFFFF, 0x2, 0, 0x0000, false, BACK_TO_BACK}),   // SACKed
        NACK_TC({"MRC NACK F04", MRC_NACK, 0xFFFF, REQ_QP_4, 0,0,0,0, 0x000001, NACK_TRIMMED,          0, 0xDEF0, REQ_QP_4, REMOTE_QPN+7, 0x000001, 0x2, 0, 0x0000, true,  RETRANSMIT}),
        ACK_TC ({"MRC ACK F05",  MRC_ACK,  0xFFFF, REQ_QP_4, 0,0,0,0, 0x000000, 0x1F, 0, 3, BACK_TO_BACK}),       // 0xFFFFFE, 0xFFFFFF, 0x000000

        // QP C. NACK retry limit (MRC_REQ_MAX_RETRIES = 7): every retriable reason retransmits, the 8th NACK puts the QP in ERROR
        NACK_TC({"MRC NACK C01", MRC_NACK, 0xFFFF, REQ_QP_1, 0,0,0,0, REQ_PSN+0, NACK_TRIMMED,         0, 0x4321, REQ_QP_1, REMOTE_QPN+4, REQ_PSN+0, 0x2, 0, 0x0000, true,  RETRANSMIT}),
        NACK_TC({"MRC NACK C02", MRC_NACK, 0xFFFF, REQ_QP_1, 0,0,0,0, REQ_PSN+0, NACK_TRIMMED_LASTHOP, 0, 0x4321, REQ_QP_1, REMOTE_QPN+4, REQ_PSN+0, 0x2, 0, 0x0000, true,  RETRANSMIT}),
        NACK_TC({"MRC NACK C03", MRC_NACK, 0xFFFF, REQ_QP_1, 0,0,0,0, REQ_PSN+0, NACK_NO_BITMAP,       0, 0x4321, REQ_QP_1, REMOTE_QPN+4, REQ_PSN+0, 0x2, 0, 0x0000, true,  RETRANSMIT}),
        NACK_TC({"MRC NACK C04", MRC_NACK, 0xFFFF, REQ_QP_1, 0,0,0,0, REQ_PSN+0, NACK_NO_PKT_BUFFER,   0, 0x4321, REQ_QP_1, REMOTE_QPN+4, REQ_PSN+0, 0x2, 0, 0x0000, true,  RETRANSMIT}),
        NACK_TC({"MRC NACK C05", MRC_NACK, 0xFFFF, REQ_QP_1, 0,0,0,0, REQ_PSN+0, NACK_NO_RESOURCE,     0, 0x4321, REQ_QP_1, REMOTE_QPN+4, REQ_PSN+0, 0x2, 0, 0x0000, true,  RETRANSMIT}),
        NACK_TC({"MRC NACK C06", MRC_NACK, 0xFFFF, REQ_QP_1, 0,0,0,0, REQ_PSN+0, NACK_PSN_OOR_WINDOW,  0, 0x4321, REQ_QP_1, REMOTE_QPN+4, REQ_PSN+0, 0x2, 0, 0x0000, true,  RETRANSMIT}),
        NACK_TC({"MRC NACK C07", MRC_NACK, 0xFFFF, REQ_QP_1, 0,0,0,0, REQ_PSN+0, NACK_TRIMMED,         0, 0x4321, REQ_QP_1, REMOTE_QPN+4, REQ_PSN+0, 0x2, 0, 0x0000, true,  RETRANSMIT}),     // retry 7
        NACK_TC({"MRC NACK C08", MRC_NACK, 0xFFFF, REQ_QP_1, 0,0,0,0, REQ_PSN+0, NACK_TRIMMED,         0, 0x4321, REQ_QP_1, REMOTE_QPN+4, REQ_PSN+0, 0x2, 0, 0x0000, false, BACK_TO_BACK}),   // -> ERROR
        NACK_TC({"MRC NACK C09", MRC_NACK, 0xFFFF, REQ_QP_1, 0,0,0,0, REQ_PSN+1, NACK_NO_RESOURCE,     0, 0x4321, REQ_QP_1, REMOTE_QPN+4, REQ_PSN+1, 0x2, 0, 0x0000, false, BACK_TO_BACK}),
        ACK_TC ({"MRC ACK C10",  MRC_ACK,  0xFFFF, REQ_QP_1, 0,0,0,0, REQ_PSN+5, 0x1F, 0, 0, BACK_TO_BACK}),

        // everything left is acknowledged: the timers of QP B and QP F stop
        ACK_TC ({"MRC ACK B13",  MRC_ACK,  0xFFFF, REQ_QP_0, 0,0,0,0, REQ_PSN+5, 0x1F, 0, 3, BACK_TO_BACK}),
        ACK_TC ({"MRC ACK F06",  MRC_ACK,  0xFFFF, REQ_QP_4, 0,0,0,0, 0x000003, 0x1F, 0, 3, DRAIN}),

        // PSN far ahead 
        WRITE_TC({"MRC WRITE ONLY 01a", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+140, 0, 0, 0,0,0, 0x3000, RKEY, 64, 0x00000000, buildWritePayload(64,0xEE), false, false, 0, 0, 0x00, BACK_TO_BACK}),
        
        // in-order ONLY, same va, distinct fills 
        WRITE_TC({"MRC WRITE ONLY 02a", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+0, 0, 0, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x01), true, true, FIRST_RX_PSN_0+0, 1, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02b", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+1, 0, 1, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x02), true, true, FIRST_RX_PSN_0+1, 2, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02c", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+2, 0, 2, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x03), true, true, FIRST_RX_PSN_0+2, 3, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02d", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+3, 0, 3, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x04), true, true, FIRST_RX_PSN_0+3, 4, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02e", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+4, 0, 4, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x05), true, true, FIRST_RX_PSN_0+4, 5, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02f", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+5, 0, 5, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x06), true, true, FIRST_RX_PSN_0+5, 6, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02g", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+6, 0, 6, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x07), true, true, FIRST_RX_PSN_0+6, 7, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02h", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+7, 0, 7, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x08), true, true, FIRST_RX_PSN_0+7, 8, 0x00, BACK_TO_BACK}),

        // exp_accepted, then its duplicate (not rewritten, re-ACKed)
        WRITE_TC({"MRC WRITE ONLY 03a", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+8, 0, 8, 0,0,0, 0x4000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x09), true,  true, FIRST_RX_PSN_0+8, 9, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 03b", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+8, 0, 8, 0,0,0, 0x4000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x09), false, true, FIRST_RX_PSN_0+8, 9, 0x00, BACK_TO_BACK}),

        // older PSNs, not rewritten, re-ACKed with cack, not their own PSN
        WRITE_TC({"MRC WRITE ONLY 04a", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+4,  0, 0, 0,0,0, 0x5000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, true, FIRST_RX_PSN_0+8,  9, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 04b", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0-21, 0, 0, 0,0,0, 0x5000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, true, FIRST_RX_PSN_0+8, 9, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 04c", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+8,  0, 0, 0,0,0, 0x5000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, true, FIRST_RX_PSN_0+8,  9, 0x00, BACK_TO_BACK}),

        // multi-packet, RETH on every packet 
        WRITE_TC({"MRC WRITE ONLY 05a", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+9,  0, 9, 0,0,0, 0x10000,         RKEY, 2*PMTU+100, 0x00000000, buildWritePayload(PMTU,0xA1), true, true, FIRST_RX_PSN_0+9,  9 , 0x00, BACK_TO_BACK}),
        PROBE_TC({"MRC PROBE 05a+", MRC_PROBE_REQ, 0xFFFF, RSP_QP_0, 0,0,0,0, 0, 0x5A, 0x3001, RSP_QP_0, REMOTE_QPN, 0x1234, 0, MRC_FTYPE_TS, BACK_TO_BACK}),
        ENDPOINT_REQ_TC({"MRC EP REQ 05b+", MRC_EP_REQ, 0xFFFF, MRC_EV_PROBE_DQP, 0,0,0,0, 0x3002, EP_OP_PORT_STATUS, 0x5A, 0xA5A5A5A5, 0, 0, 0, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 05b", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+10, 0, 9, 0,0,0, 0x10000+PMTU,     RKEY, 2*PMTU+100, 0x00000000, buildWritePayload(PMTU,0xA2), true, true, FIRST_RX_PSN_0+10, 9 , 0x00, BACK_TO_BACK}),
        PROBE_TC({"MRC PROBE 05a+", MRC_PROBE_REQ, 0xFFFF, RSP_QP_0, 0,0,0,0, 0, 0x5A, 0x3001, RSP_QP_0, REMOTE_QPN, 0x1234, 0, MRC_FTYPE_TS, BACK_TO_BACK}),
        ENDPOINT_REQ_TC({"MRC EP REQ 05b+", MRC_EP_REQ, 0xFFFF, MRC_EV_PROBE_DQP, 0,0,0,0, 0x3002, EP_OP_PORT_STATUS, 0x5A, 0xA5A5A5A5, 0, 0, 0, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 05c", MRC_RDMA_WRITE_LAST,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+11, 0, 9, 0,0,0, 0x10000+2*PMTU,   RKEY, 2*PMTU+100, 0x00000000, buildWritePayload(100,0xA3), true, true, FIRST_RX_PSN_0+11, 10, 0x00, BACK_TO_BACK}),

        // TSETH on/off 
        WRITE_TC({"MRC WRITE ONLY 06a", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN_0+12, 0, 10, 0x1234,0,1, 0x20000,       RKEY, 700,       0x00000000, buildWritePayload(700,0xB0), true, true, FIRST_RX_PSN_0+12, 11, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 06b", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN_0+13, 0, 11, 0x1234,0,1, 0x21000,       RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(PMTU,0xB1), true, true, FIRST_RX_PSN_0+13, 11, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 06c", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN_0+14, 0, 11, 0x1234,0,1, 0x21000+PMTU,   RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(PMTU,0xB2), true, true, FIRST_RX_PSN_0+14, 11, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 06d", MRC_RDMA_WRITE_LAST,   0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN_0+15, 0, 11, 0x1234,0,1, 0x21000+2*PMTU, RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(50,0xB3),  true, true, FIRST_RX_PSN_0+15, 12, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 06e", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+16, 0, 12, 0,0,0,      0x24000,       RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(PMTU,0xC1), true, true, FIRST_RX_PSN_0+16, 12, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 06f", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN_0+17, 0, 12, 0x1234,0,1, 0x24000+PMTU,   RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(PMTU,0xC2), true, true, FIRST_RX_PSN_0+17, 12, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 06g", MRC_RDMA_WRITE_LAST,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+18, 0, 12, 0,0,0,      0x24000+2*PMTU, RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(50,0xC3),  true, true, FIRST_RX_PSN_0+18, 13, 0x00, BACK_TO_BACK}),

        // rtx bit 
        WRITE_TC({"MRC WRITE ONLY 07a", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,1,0, FIRST_RX_PSN_0+19, 0, 13, 0,0,0, 0x30000, RKEY, 200, 0x00000000, buildWritePayload(200,0xD0), true, true, FIRST_RX_PSN_0+19, 14, 0x00, BACK_TO_BACK}),

        // sizes around the 64-byte beat boundary 
        WRITE_TC({"MRC WRITE ONLY 08a", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+20, 0, 14, 0,0,0,      0x40000, RKEY, 1,  0x00000000, buildWritePayload(1, 0x10), true, true, FIRST_RX_PSN_0+20, 15, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08b", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+21, 0, 15, 0,0,0,      0x40100, RKEY, 44, 0x00000000, buildWritePayload(44,0x20), true, true, FIRST_RX_PSN_0+21, 16, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08c", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+22, 0, 16, 0,0,0,      0x40200, RKEY, 45, 0x00000000, buildWritePayload(45,0x30), true, true, FIRST_RX_PSN_0+22, 17, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08d", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+23, 0, 17, 0,0,0,      0x40300, RKEY, 48, 0x00000000, buildWritePayload(48,0x40), true, true, FIRST_RX_PSN_0+23, 18, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08e", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+24, 0, 18, 0,0,0,      0x40400, RKEY, 49, 0x00000000, buildWritePayload(49,0x50), true, true, FIRST_RX_PSN_0+24, 19, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08f", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN_0+25, 0, 19, 0x1234,0,1, 0x40500, RKEY, 40, 0x00000000, buildWritePayload(40,0x60), true, true, FIRST_RX_PSN_0+25, 20, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08g", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN_0+26, 0, 20, 0x1234,0,1, 0x40600, RKEY, 48, 0x00000000, buildWritePayload(48,0x70), true, true, FIRST_RX_PSN_0+26, 21, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08h", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN_0+27, 0, 21, 0x1234,0,1, 0x40700, RKEY, 49, 0x00000000, buildWritePayload(49,0x80), true, true, FIRST_RX_PSN_0+27, 22, 0x00, BACK_TO_BACK}),

        // ONLY todo: add control packets in the middle
        WRITE_TC({"MRC WRITE ONLY 09a", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+28, 0, 22, 0,0,0, 0x50000, RKEY, 256, 0x00000000, buildWritePayload(256,0x90), true, true, FIRST_RX_PSN_0+28, 23, 0x00, BACK_TO_BACK}),
        ACK_TC ({"MRC ACK 09b",  MRC_ACK,  0xFFFF, RSP_QP_0, 0,0,0,0, 0x0000FF, 0x1F, 0, 0, BACK_TO_BACK}),
        SACK_TC({"MRC SACK 09c", MRC_SACK, 0xFFFF, RSP_QP_0, 0,0,0,0, 0x0000FF, SACK_M_NONE, 0, 0, 0, RSP_QP_0, REMOTE_QPN, 0x0000FF, MRC_CC_TYPE_DEFAULT, 0, 0, 0, 0, 0, 0, 0, 0, 0, BACK_TO_BACK}),
        NACK_TC({"MRC NACK 09d", MRC_NACK, 0xFFFF, RSP_QP_0, 0,0,0,0, 0x000100, NACK_TRIMMED, 0, 0, RSP_QP_0, REMOTE_QPN, 0x000100, 0x2, 0, 0x0000, false, BACK_TO_BACK}),
        PROBE_TC({"MRC PROBE 09e", MRC_PROBE_REQ, 0xFFFF, RSP_QP_0, 0,0,0,0, 0, 0, 0x3003, RSP_QP_0, REMOTE_QPN, 0, 0, 0, BACK_TO_BACK}),
        ENDPOINT_REQ_TC({"MRC EP REQ 09f", MRC_EP_REQ, 0xFFFF, MRC_EV_PROBE_DQP, 0,0,0,0, 0x3004, EP_OP_EV_PROBE, 0, 0, 0x2222, 0, MRC_FTYPE_TS, BACK_TO_BACK}),
        ENDPOINT_RES_TC({"MRC EP RES 09g", MRC_EP_RSP, 0xFFFF, MRC_EV_PROBE_DQP, 0,0,0,0, 0x3004, EP_OP_EV_PROBE, 0x3333, BACK_TO_BACK}),

        // Write with Immediate 
        WRITE_TC({"MRC WRITE ONLY 10a", MRC_RDMA_WRITE_FIRST,     0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+29, 0, 23, 0,0,0, 0x51000,     RKEY, PMTU+100, 0x00000000, buildWritePayload(PMTU,0xE1), true, true, FIRST_RX_PSN_0+29, 23, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 10b", MRC_RDMA_WRITE_LAST_IMM,  0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+30, 0, 23, 0,0,0, 0x51000+PMTU, RKEY, PMTU+100, 0x00000000, buildWritePayload(100,0xE2), true, true, FIRST_RX_PSN_0+30, 24, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 10c", MRC_RDMA_WRITE_ONLY_IMM,  0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+31, 0, 24, 0,0,0, 0x52000,     RKEY, 300,     0x00000000, buildWritePayload(300,0xE3), true, true, FIRST_RX_PSN_0+31, 25, 0x00, BACK_TO_BACK}),

        // PSN ahead (repo: NAK once, then silent), in order resets it 
        WRITE_TC({"MRC WRITE ONLY 11a", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+182, 0, 0,  0,0,0, 0x2000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, false, 0, 0, 0x00, BACK_TO_BACK}), 
        WRITE_TC({"MRC WRITE ONLY 11b", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+182, 0, 0,  0,0,0, 0x2000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, false, 0, 0, 0x00, BACK_TO_BACK}), 
        WRITE_TC({"MRC WRITE ONLY 11c", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+182, 0, 0,  0,0,0, 0x2000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, false, 0, 0, 0x00, BACK_TO_BACK}), 
        WRITE_TC({"MRC WRITE ONLY 11d", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+32,  0, 25, 0,0,0, 0x2000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0x11), true,  true,  FIRST_RX_PSN_0+32, 26, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 11e", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+183, 0, 0,  0,0,0, 0x2000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, false, 0, 0, 0x00, BACK_TO_BACK}), 
        WRITE_TC({"MRC WRITE ONLY 11f", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+183, 0, 0,  0,0,0, 0x2000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, false, 0, 0, 0x00, BACK_TO_BACK}),

        // burst
        WRITE_TC({"MRC WRITE ONLY 12a", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+33, 0, 26, 0,0,0,      0x60000,       RKEY, 500,       0x00000000, buildWritePayload(500,0xF1), true, true, FIRST_RX_PSN_0+33, 27, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 12b", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN_0+34, 0, 27, 0x1234,0,1, 0x61000,       RKEY, 45,        0x00000000, buildWritePayload(45,0xF2),  true, true, FIRST_RX_PSN_0+34, 28, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 12c", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+35, 0, 28, 0,0,0,      0x62000,       RKEY, 2*PMTU+10,  0x00000000, buildWritePayload(PMTU,0xF3), true, true, FIRST_RX_PSN_0+35, 28, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 12d", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+36, 0, 28, 0,0,0,      0x62000+PMTU,   RKEY, 2*PMTU+10,  0x00000000, buildWritePayload(PMTU,0xF4), true, true, FIRST_RX_PSN_0+36, 28, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 12e", MRC_RDMA_WRITE_LAST,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+37, 0, 28, 0,0,0,      0x62000+2*PMTU, RKEY, 2*PMTU+10,  0x00000000, buildWritePayload(10,0xF5),  true, true, FIRST_RX_PSN_0+37, 29, 0x00, BACK_TO_BACK}),
        
        // same as before but waiting random amount of cycles in between
        WRITE_TC({"MRC WRITE ONLY 13a", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+38, 0, 29, 0,0,0,      0x60000,        RKEY, 500,       0x00000000, buildWritePayload(500,0xF1), true, true, FIRST_RX_PSN_0+38, 30, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 13b", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN_0+39, 0, 30, 0x1234,0,1, 0x61000,        RKEY, 45,        0x00000000, buildWritePayload(45,0xF2),  true, true, FIRST_RX_PSN_0+39, 31, 0x00, DRAIN}),
        WRITE_TC({"MRC WRITE ONLY 13c", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+40, 0, 31, 0,0,0,      0x62000,        RKEY, 2*PMTU+10,  0x00000000, buildWritePayload(PMTU,0xF3), true, true, FIRST_RX_PSN_0+40, 31, 0x00, 2}),
        WRITE_TC({"MRC WRITE ONLY 13d", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+41, 0, 31, 0,0,0,      0x62000+PMTU,   RKEY, 2*PMTU+10,  0x00000000, buildWritePayload(PMTU,0xF4), true, true, FIRST_RX_PSN_0+41, 31, 0x00, 200}),
        WRITE_TC({"MRC WRITE ONLY 13e", MRC_RDMA_WRITE_LAST,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+42, 0, 31, 0,0,0,      0x62000+2*PMTU, RKEY, 2*PMTU+10,  0x00000000, buildWritePayload(10,0xF5),  true, true, FIRST_RX_PSN_0+42, 32, 0x00, 1000}),

        // BTH.a = 0 -> written, MSN still advances, but no ACK. Each a=0 row is followed by an a=1 row
        // in the same group, so a stray ACK would be caught by the next ACK check
        WRITE_TC({"MRC WRITE ONLY 14a", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN_0+43, 0, 32, 0,0,0, 0x80000,        RKEY, 128,       0x00000000, buildWritePayload(128,0x14),  true, false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 14b", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+44, 0, 33, 0,0,0, 0x80100,        RKEY, 128,       0x00000000, buildWritePayload(128,0x15),  true, true,  FIRST_RX_PSN_0+44, 34, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 14c", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN_0+45, 0, 34, 0,0,0, 0x81000,        RKEY, 2*PMTU+30, 0x00000000, buildWritePayload(PMTU,0x16), true, false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 14d", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN_0+46, 0, 34, 0,0,0, 0x81000+PMTU,   RKEY, 2*PMTU+30, 0x00000000, buildWritePayload(PMTU,0x17), true, false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 14e", MRC_RDMA_WRITE_LAST,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+47, 0, 34, 0,0,0, 0x81000+2*PMTU, RKEY, 2*PMTU+30, 0x00000000, buildWritePayload(30,0x18),   true, true,  FIRST_RX_PSN_0+47, 35, 0x00, DRAIN}),

        // two multi-packet messages out of order: A = +48..+51, B = +52..+54. Packets beyond the hole are written
        // but not ACKed, a duplicate inside the window is ACKed with the current cack_psn. 15h closes the hole:
        // cack_psn jumps to +54 and the pending ACK requests of 15c and 15e produce one ACK, MSN +2, although 15h has a = 0
        WRITE_TC({"MRC WRITE ONLY 15a", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN_0+53, 0, 36, 0,0,0, 0x86000+PMTU,   RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(PMTU,0x40), true,  false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 15b", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN_0+50, 0, 35, 0,0,0, 0x82000+2*PMTU, RKEY, 3*PMTU+100, 0x00000000, buildWritePayload(PMTU,0x41), true,  false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 15c", MRC_RDMA_WRITE_LAST,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+54, 0, 36, 0,0,0, 0x86000+2*PMTU, RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(50,0x42),   true,  true,  FIRST_RX_PSN_0+47, 35, 0x00, BACK_TO_BACK}),
        // for ackmap
        //WRITE_TC({"MRC WRITE ONLY 15d", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN_0+48, 0, 35, 0,0,0, 0x82000,        RKEY, 3*PMTU+100, 0x00000000, buildWritePayload(PMTU,0x43), true,  false, 0,               0,  0x00, BACK_TO_BACK}),
        // for prevAr
        WRITE_TC({"MRC WRITE ONLY 15d", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN_0+48, 0, 35, 0,0,0, 0x82000,        RKEY, 3*PMTU+100, 0x00000000, buildWritePayload(PMTU,0x43), true,  false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 15e", MRC_RDMA_WRITE_LAST,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+51, 0, 35, 0,0,0, 0x82000+3*PMTU, RKEY, 3*PMTU+100, 0x00000000, buildWritePayload(100,0x44),  true,  true,  FIRST_RX_PSN_0+48, 35, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 15f", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN_0+52, 0, 36, 0,0,0, 0x86000,        RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(PMTU,0x45), true,  false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 15g", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+53, 0, 36, 0,0,0, 0x86000+PMTU,   RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(PMTU,0xEE), false, true,  FIRST_RX_PSN_0+48, 35, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 15h", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN_0+49, 0, 35, 0,0,0, 0x82000+PMTU,   RKEY, 3*PMTU+100, 0x00000000, buildWritePayload(PMTU,0x46), true,  true,  FIRST_RX_PSN_0+54, 37, 0x00, DRAIN}),

        // window edge (epsn = +55): epsn+128 is dropped silently, epsn+127 is written but not ACKed (hole below it),
        // epsn+128 is still dropped because accepting a packet beyond a hole does not move the window
        WRITE_TC({"MRC WRITE ONLY 16a", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+183, 0, 37, 0,0,0, 0x83000,        RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, false, 0,               0,  0x00, BACK_TO_BACK}),
        //WRITE_TC({"MRC WRITE ONLY 16b", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+182, 0, 37, 0,0,0, 0x83100,        RKEY, 64,        0x00000000, buildWritePayload(64,0x1B),   true,  false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 16b", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+182, 0, 37, 0,0,0, 0x83100,        RKEY, 64,        0x00000000, buildWritePayload(64,0x1B),   true,  true,  FIRST_RX_PSN_0+54, 37, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 16c", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+183, 0, 38, 0,0,0, 0x83200,        RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, false, 0,               0,  0x00, DRAIN}),

        // out-of-window packets are dropped silently whatever BTH.a; the duplicate and in-order packets around them are unaffected
        WRITE_TC({"MRC WRITE ONLY 17a", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+300, 0, 39, 0,0,0, 0x84000,       RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 17b", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+54,  0, 38, 0,0,0, 0x84000,       RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, true,  FIRST_RX_PSN_0+54, 37, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 17c", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+300, 0, 39, 0,0,0, 0x84000,       RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 17d", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+55,  0, 39, 0,0,0, 0x84100,       RKEY, 64,        0x00000000, buildWritePayload(64,0x1D),   true,  true,  FIRST_RX_PSN_0+55, 38, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 17e", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN_0+301, 0, 40, 0,0,0, 0x84000,       RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 17f", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+56,  0, 40, 0,0,0, 0x84200,       RKEY, 64,        0x00000000, buildWritePayload(64,0x1E),   true,  true,  FIRST_RX_PSN_0+56, 39, 0x00, DRAIN}),

        // duplicates are ACKed with the current cack_psn whatever BTH.a and opcode, and never rewritten
        WRITE_TC({"MRC WRITE ONLY 18a", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN_0+56, 0, 40, 0,0,0, 0x84200,        RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, true, FIRST_RX_PSN_0+56, 39, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 18b", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+46, 0, 34, 0,0,0, 0x81000+PMTU,   RKEY, 2*PMTU+30, 0x00000000, buildWritePayload(PMTU,0xEE), false, true, FIRST_RX_PSN_0+56, 39, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 18c", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN_0+57, 0, 41, 0,0,0, 0x85000,        RKEY, 64,        0x00000000, buildWritePayload(64,0x1F),   true,  true, FIRST_RX_PSN_0+57, 40, 0x00, DRAIN}),

        // QP 0x06 (epsn 0xFFFFFE): multi-packet write in order across the 24-bit wrap, then a duplicate behind the wrap
        WRITE_TC({"MRC WRITE ONLY 19a", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_1, 1,0,0,0, 0xFFFFFE, 0, 0, 0,0,0, 0x70000,        RKEY, 2*PMTU+20, 0x00000000, buildWritePayload(PMTU,0x21), true,  true, 0xFFFFFE, 0, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 19b", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_1, 1,0,0,0, 0xFFFFFF, 0, 0, 0,0,0, 0x70000+PMTU,   RKEY, 2*PMTU+20, 0x00000000, buildWritePayload(PMTU,0x22), true,  true, 0xFFFFFF, 0, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 19c", MRC_RDMA_WRITE_LAST,   0xFFFF, RSP_QP_1, 1,0,0,0, 0x000000, 0, 0, 0,0,0, 0x70000+2*PMTU, RKEY, 2*PMTU+20, 0x00000000, buildWritePayload(20,0x23),   true,  true, 0x000000, 1, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 19d", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_1, 1,0,0,0, 0xFFFFFF, 0, 0, 0,0,0, 0x71000,        RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, true, 0x000000, 1, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 19e", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_1, 1,0,0,0, 0x000001, 0, 1, 0,0,0, 0x71100,        RKEY, 64,        0x00000000, buildWritePayload(64,0x24),   true,  true, 0x000001, 2, 0x00, DRAIN}),

        // QP 0x07 (epsn 0xFFFFFB): ahead by 7 ACROSS the wrap. Expected = same behaviour as 15a (accepted).
        // QP 0x0A (epsn 0xFFFFFB): ahead by 7 ACROSS the wrap: written, ACK request pending until 0xFFFFFB..0x000001 arrive
        // QP 0x0A (epsn 0xFFFFFB): ahead by 7 across the wrap. Written and ACKed at once with cack 0xFFFFFA because BTH.a = 1
        // (like 16b), the ACK request also stays pending (prevAr) until 0xFFFFFB..0x000001 arrive
        WRITE_TC({"MRC WRITE ONLY 20a", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_2, 1,0,0,0, 0x000002, 0, 0, 0,0,0, 0x78000,        RKEY, 64,        0x00000000, buildWritePayload(64,0x25),   true,  true,  0xFFFFFA, 0, 0x00, DRAIN}),                
        // QP 0x10, fresh (epsn FIRST_RX_PSN_3). Write with Immediate: TSETH + ImmDt layout, non-zero immDt / RQMSN,
        // LAST_IMM ahead of a hole (stale ACK), the hole closed by an a = 0 FIRST (prevAr ACK), then a duplicate LAST_IMM
        WRITE_TC({"MRC WRITE IMM 21a", MRC_RDMA_WRITE_ONLY_IMM, 0xFFFF, RSP_QP_3, 1,0,0,1, FIRST_RX_PSN_3+0, 1, 0, 0x1234,0,1, 0x90000,      RKEY, 64,      0xCAFEBABE, buildWritePayload(64,0x31),   true,  true,  FIRST_RX_PSN_3+0, 1, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE IMM 21b", MRC_RDMA_WRITE_FIRST,    0xFFFF, RSP_QP_3, 1,0,0,1, FIRST_RX_PSN_3+1, 2, 1, 0x1234,0,1, 0x91000,      RKEY, PMTU+64, 0x00000000, buildWritePayload(PMTU,0x32), true,  true,  FIRST_RX_PSN_3+1, 1, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE IMM 21c", MRC_RDMA_WRITE_LAST_IMM, 0xFFFF, RSP_QP_3, 1,0,0,1, FIRST_RX_PSN_3+2, 2, 1, 0x1234,0,1, 0x91000+PMTU, RKEY, PMTU+64, 0x12345678, buildWritePayload(64,0x33),   true,  true,  FIRST_RX_PSN_3+2, 2, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE IMM 21d", MRC_RDMA_WRITE_LAST_IMM, 0xFFFF, RSP_QP_3, 1,0,0,0, FIRST_RX_PSN_3+4, 3, 2, 0,0,0,      0x92000+PMTU, RKEY, PMTU+64, 0x0BADF00D, buildWritePayload(64,0x34),   true,  true,  FIRST_RX_PSN_3+2, 2, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE IMM 21e", MRC_RDMA_WRITE_FIRST,    0xFFFF, RSP_QP_3, 0,0,0,0, FIRST_RX_PSN_3+3, 3, 2, 0,0,0,      0x92000,      RKEY, PMTU+64, 0x00000000, buildWritePayload(PMTU,0x35), true,  true,  FIRST_RX_PSN_3+4, 3, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE IMM 21f", MRC_RDMA_WRITE_LAST_IMM, 0xFFFF, RSP_QP_3, 0,0,0,0, FIRST_RX_PSN_3+4, 3, 2, 0,0,0,      0x92000+PMTU, RKEY, PMTU+64, 0x0BADF00D, buildWritePayload(64,0xEE),   false, true,  FIRST_RX_PSN_3+4, 3, 0x00, DRAIN}),

        // LAST corner cases: a full-PMTU LAST with a = 0 completes its message without an ACK (seen through the MSN of the
        // following duplicate, which also has rtx = 1); a LAST ahead of a hole gets a stale ACK; the hole is closed by a
        // FIRST with a = 0 but rtx = 1; a LAST beyond the window is dropped silently (the next ONLY would catch a stray ACK)
        WRITE_TC({"MRC WRITE LAST 22a", MRC_RDMA_WRITE_FIRST,   0xFFFF, RSP_QP_3, 1,0,0,0, FIRST_RX_PSN_3+5,   0, 3, 0,0,0, 0x93000,      RKEY, 2*PMTU,  0x00000000, buildWritePayload(PMTU,0x41), true,  true,  FIRST_RX_PSN_3+5, 3, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE LAST 22b", MRC_RDMA_WRITE_LAST,    0xFFFF, RSP_QP_3, 0,0,0,0, FIRST_RX_PSN_3+6,   0, 3, 0,0,0, 0x93000+PMTU, RKEY, 2*PMTU,  0x00000000, buildWritePayload(PMTU,0x42), true,  false, 0,                0, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE LAST 22c", MRC_RDMA_WRITE_LAST,    0xFFFF, RSP_QP_3, 1,0,1,0, FIRST_RX_PSN_3+6,   0, 3, 0,0,0, 0x93000+PMTU, RKEY, 2*PMTU,  0x00000000, buildWritePayload(PMTU,0xEE), false, true,  FIRST_RX_PSN_3+6, 4, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE LAST 22d", MRC_RDMA_WRITE_LAST,    0xFFFF, RSP_QP_3, 1,0,0,0, FIRST_RX_PSN_3+8,   0, 4, 0,0,0, 0x94000+PMTU, RKEY, PMTU+32, 0x00000000, buildWritePayload(32,0x43),   true,  true,  FIRST_RX_PSN_3+6, 4, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE LAST 22e", MRC_RDMA_WRITE_FIRST,   0xFFFF, RSP_QP_3, 0,0,1,0, FIRST_RX_PSN_3+7,   0, 4, 0,0,0, 0x94000,      RKEY, PMTU+32, 0x00000000, buildWritePayload(PMTU,0x44), true,  true,  FIRST_RX_PSN_3+8, 5, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE LAST 22f", MRC_RDMA_WRITE_LAST,    0xFFFF, RSP_QP_3, 1,0,0,0, FIRST_RX_PSN_3+137, 0, 5, 0,0,0, 0x95000,      RKEY, PMTU+32, 0x00000000, buildWritePayload(32,0xEE),   false, false, 0,                0, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE LAST 22g", MRC_RDMA_WRITE_ONLY,    0xFFFF, RSP_QP_3, 1,0,0,0, FIRST_RX_PSN_3+9,   0, 5, 0,0,0, 0x95100,      RKEY, 32,      0x00000000, buildWritePayload(32,0x45),   true,  true,  FIRST_RX_PSN_3+9, 6, 0x00, DRAIN}),

        // zero-length WRITE ONLY (0 <= only <= PMTU): ACKed and completed with no data, the next write is unaffected
        WRITE_TC({"MRC WRITE ZERO 23a", MRC_RDMA_WRITE_ONLY,    0xFFFF, RSP_QP_3, 1,0,0,0, FIRST_RX_PSN_3+10,  0, 6, 0,0,0, 0x96000,      RKEY, 0,       0x00000000, buildWritePayload(0,0x00),    true,  true,  FIRST_RX_PSN_3+10, 7, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ZERO 23b", MRC_RDMA_WRITE_ONLY,    0xFFFF, RSP_QP_3, 1,0,0,0, FIRST_RX_PSN_3+11,  0, 7, 0,0,0, 0x96100,      RKEY, 64,      0x00000000, buildWritePayload(64,0x51),   true,  true,  FIRST_RX_PSN_3+11, 8, 0x00, DRAIN}),
        
        // RESPONDER SIDE
    };
}