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
        
        // ImmDt of the WriteIMMs completed by this packet (cack moved past their LAST_IMM / ONLY_IMM), in RQMSN order.
        // Last, so the rows completing none leave it empty
        std::vector<uint32_t> exp_imm;

        // set the QP up again just before this packet (same context as at start-up, expected PSN = this psn)
        bool reconnect;
        // update only the r_key of the QP just before this packet (virtual_address = 0xDEADBEEF): MSN and RQMSN carry on
        bool rkey_update;

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
        SACK_TC({.name = "MRC SACK B01", .opcode = MRC_SACK, .p_key = 0xFFFF, .dqp = REQ_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = 0x00ABCD,
                 .m = SACK_M_ALWAYS_SKIP, .pr = 0, .ack_psn_offset = -1, .entropy = 0xDEADBEEF, .spdcid = REQ_QP_0, .dpdcid = REMOTE_QPN+3, .cack_psn = REQ_PSN+1, .cc_type = MRC_CC_TYPE_DEFAULT, .cc_fl = 0xF, .mpr = 0x04, .sack_offset = -3, .sack_bitmap = 0xF0F0F0F0F0F0F0F0ULL,
                 .tx_timestamp = 0xBEEF, .ooo_count = 0x7FFF, .r_c = 1, .rcv_cwnd_pen = 0x7F, .rcvd_bytes = 0xABCDEF, .after_cycles = BACK_TO_BACK}),
        NACK_TC({.name = "MRC NACK B02", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+1,
                 .nack_reason = NACK_TRIMMED, .vendor_info = 0, .entropy = 0x1234, .spdcid = REQ_QP_0, .dpdcid = REMOTE_QPN+3, .nack_psn = REQ_PSN+1, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = false, .after_cycles = BACK_TO_BACK}),   // SACKed
        NACK_TC({.name = "MRC NACK B03", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+0,
                 .nack_reason = NACK_NO_BITMAP, .vendor_info = 0, .entropy = 0x1234, .spdcid = REQ_QP_0, .dpdcid = REMOTE_QPN+3, .nack_psn = REQ_PSN+2, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x1111,
                 .exp_retransmit = true, .after_cycles = RETRANSMIT}),     // NETH.nack_psn wins over BTH.psn
        ACK_TC ({.name = "MRC ACK B04", .opcode = MRC_ACK, .p_key = 0xFFFF, .dqp = REQ_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+2,
                 .syndrome = 0x1F, .msn = 0x777, .exp_completions = 3, .after_cycles = BACK_TO_BACK}),   // +0 and +1 were only SACKed
        ACK_TC ({.name = "MRC ACK B05", .opcode = MRC_ACK, .p_key = 0xFFFF, .dqp = REQ_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+1,
                 .syndrome = 0x1F, .msn = 0, .exp_completions = 0, .after_cycles = BACK_TO_BACK}),   // stale
        ACK_TC ({.name = "MRC ACK B06", .opcode = MRC_ACK, .p_key = 0xFFFF, .dqp = REQ_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+5,
                 .syndrome = 0x21, .msn = 0, .exp_completions = 0, .after_cycles = BACK_TO_BACK}),   // RNR NAK
        ACK_TC ({.name = "MRC ACK B07", .opcode = MRC_ACK, .p_key = 0xFFFF, .dqp = REQ_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+5,
                 .syndrome = 0x40, .msn = 0, .exp_completions = 0, .after_cycles = BACK_TO_BACK}),   // reserved type
        SACK_TC({.name = "MRC SACK B08", .opcode = MRC_SACK, .p_key = 0xFFFF, .dqp = REQ_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+9,
                 .m = SACK_M_NONE, .pr = 0, .ack_psn_offset = 0, .entropy = 0, .spdcid = REQ_QP_0, .dpdcid = REMOTE_QPN+3, .cack_psn = REQ_PSN+9, .cc_type = MRC_CC_TYPE_DEFAULT, .cc_fl = 0, .mpr = 0, .sack_offset = 0, .sack_bitmap = 0,
                 .tx_timestamp = 0, .ooo_count = 0, .r_c = 0, .rcv_cwnd_pen = 0, .rcvd_bytes = 0, .after_cycles = BACK_TO_BACK}), // never sent
        NACK_TC({.name = "MRC NACK B09", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+3,
                 .nack_reason = NACK_NO_PKT_BUFFER, .vendor_info = 0, .entropy = 0x1234, .spdcid = REQ_QP_0, .dpdcid = REMOTE_QPN+3, .nack_psn = REQ_PSN+3, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = true, .after_cycles = RETRANSMIT}),     // B08 did not move unack_psn
        NACK_TC({.name = "MRC NACK B10", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+4,
                 .nack_reason = 0x03, .vendor_info = 0, .entropy = 0x1234, .spdcid = REQ_QP_0, .dpdcid = REMOTE_QPN+3, .nack_psn = REQ_PSN+4, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = false, .after_cycles = BACK_TO_BACK}),   // reserved reason
        NACK_TC({.name = "MRC NACK B11", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+6,
                 .nack_reason = NACK_TRIMMED, .vendor_info = 0, .entropy = 0x1234, .spdcid = REQ_QP_0, .dpdcid = REMOTE_QPN+3, .nack_psn = REQ_PSN+6, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = false, .after_cycles = BACK_TO_BACK}),   // never sent
        ACK_TC ({.name = "MRC ACK B12", .opcode = MRC_ACK, .p_key = 0xFFFF, .dqp = REQ_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+6,
                 .syndrome = 0x1F, .msn = 0, .exp_completions = 0, .after_cycles = BACK_TO_BACK}),   // never sent

        // QP D. AETH NAK (Class B): QP to ERROR, no retransmission; in ERROR every ACK / NACK is ignored
        ACK_TC ({.name = "MRC ACK D01", .opcode = MRC_ACK, .p_key = 0xFFFF, .dqp = REQ_QP_2, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+0,
                 .syndrome = 0x21, .msn = 0, .exp_completions = 0, .after_cycles = BACK_TO_BACK}),       // RNR NAK
        ACK_TC ({.name = "MRC ACK D02", .opcode = MRC_ACK, .p_key = 0xFFFF, .dqp = REQ_QP_2, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+0,
                 .syndrome = 0x1F, .msn = 0, .exp_completions = 1, .after_cycles = BACK_TO_BACK}),
        NACK_TC({.name = "MRC NACK D03", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_2, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+1,
                 .nack_reason = NACK_TRIMMED_LASTHOP, .vendor_info = 0, .entropy = 0x5678, .spdcid = REQ_QP_2, .dpdcid = REMOTE_QPN+5, .nack_psn = REQ_PSN+1, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = true, .after_cycles = RETRANSMIT}),
        ACK_TC ({.name = "MRC NAK D04", .opcode = MRC_ACK, .p_key = 0xFFFF, .dqp = REQ_QP_2, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+1,
                 .syndrome = 0x60 | AETH_NAK_REMOTE_ACCESS, .msn = 0, .exp_completions = 0, .after_cycles = BACK_TO_BACK}),   // -> ERROR
        NACK_TC({.name = "MRC NACK D05", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_2, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+1,
                 .nack_reason = NACK_TRIMMED, .vendor_info = 0, .entropy = 0x5678, .spdcid = REQ_QP_2, .dpdcid = REMOTE_QPN+5, .nack_psn = REQ_PSN+1, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = false, .after_cycles = BACK_TO_BACK}),
        ACK_TC ({.name = "MRC ACK D06", .opcode = MRC_ACK, .p_key = 0xFFFF, .dqp = REQ_QP_2, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+5,
                 .syndrome = 0x1F, .msn = 0, .exp_completions = 0, .after_cycles = BACK_TO_BACK}),

        // QP E. NACK UNEXP_EVENT: QP to ERROR. CURRENT BEHAVIOUR: on a psn never sent it is ignored and the QP stays usable.
        // E02 SACKs a psn that is never ACKed (the QP ends in ERROR): a completion caused by the SACK would be left over
        NACK_TC({.name = "MRC NACK E01", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_3, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+7,
                 .nack_reason = NACK_UNEXP_EVENT, .vendor_info = 0, .entropy = 0x9ABC, .spdcid = REQ_QP_3, .dpdcid = REMOTE_QPN+6, .nack_psn = REQ_PSN+7, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = false, .after_cycles = BACK_TO_BACK}),
        SACK_TC({.name = "MRC SACK E02", .opcode = MRC_SACK, .p_key = 0xFFFF, .dqp = REQ_QP_3, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+0,
                 .m = SACK_M_NONE, .pr = 0, .ack_psn_offset = 0, .entropy = 0x9ABC, .spdcid = REQ_QP_3, .dpdcid = REMOTE_QPN+6, .cack_psn = REQ_PSN+0, .cc_type = MRC_CC_TYPE_DEFAULT, .cc_fl = 0, .mpr = 0, .sack_offset = 0, .sack_bitmap = 0,
                 .tx_timestamp = 0, .ooo_count = 0, .r_c = 0, .rcv_cwnd_pen = 0, .rcvd_bytes = 0, .after_cycles = BACK_TO_BACK}),
        NACK_TC({.name = "MRC NACK E03", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_3, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+1,
                 .nack_reason = NACK_NO_RESOURCE, .vendor_info = 0, .entropy = 0x9ABC, .spdcid = REQ_QP_3, .dpdcid = REMOTE_QPN+6, .nack_psn = REQ_PSN+1, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = true, .after_cycles = RETRANSMIT}),
        NACK_TC({.name = "MRC NACK E04", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_3, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+2,
                 .nack_reason = NACK_UNEXP_EVENT, .vendor_info = 0, .entropy = 0x9ABC, .spdcid = REQ_QP_3, .dpdcid = REMOTE_QPN+6, .nack_psn = REQ_PSN+2, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = false, .after_cycles = BACK_TO_BACK}),   // -> ERROR
        NACK_TC({.name = "MRC NACK E05", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_3, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+2,
                 .nack_reason = NACK_TRIMMED, .vendor_info = 0, .entropy = 0x9ABC, .spdcid = REQ_QP_3, .dpdcid = REMOTE_QPN+6, .nack_psn = REQ_PSN+2, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = false, .after_cycles = BACK_TO_BACK}),
        ACK_TC ({.name = "MRC ACK E06", .opcode = MRC_ACK, .p_key = 0xFFFF, .dqp = REQ_QP_3, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+5,
                 .syndrome = 0x1F, .msn = 0, .exp_completions = 0, .after_cycles = BACK_TO_BACK}),

        // QP F. Outstanding check, SACK and release across the 24-bit wrap
        NACK_TC({.name = "MRC NACK F01", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_4, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = 0x000000,
                 .nack_reason = NACK_PSN_OOR_WINDOW, .vendor_info = 0, .entropy = 0xDEF0, .spdcid = REQ_QP_4, .dpdcid = REMOTE_QPN+7, .nack_psn = 0x000000, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = true, .after_cycles = RETRANSMIT}),
        SACK_TC({.name = "MRC SACK F02", .opcode = MRC_SACK, .p_key = 0xFFFF, .dqp = REQ_QP_4, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = 0xFFFFFF,
                 .m = SACK_M_SKIP, .pr = 0, .ack_psn_offset = 0, .entropy = 0xDEF0, .spdcid = REQ_QP_4, .dpdcid = REMOTE_QPN+7, .cack_psn = 0xFFFFFF, .cc_type = MRC_CC_TYPE_DEFAULT, .cc_fl = 0, .mpr = 0, .sack_offset = 0, .sack_bitmap = 0,
                 .tx_timestamp = 0, .ooo_count = 0, .r_c = 0, .rcv_cwnd_pen = 0, .rcvd_bytes = 0, .after_cycles = BACK_TO_BACK}),
        NACK_TC({.name = "MRC NACK F03", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_4, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = 0xFFFFFF,
                 .nack_reason = NACK_TRIMMED, .vendor_info = 0, .entropy = 0xDEF0, .spdcid = REQ_QP_4, .dpdcid = REMOTE_QPN+7, .nack_psn = 0xFFFFFF, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = false, .after_cycles = BACK_TO_BACK}),   // SACKed
        NACK_TC({.name = "MRC NACK F04", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_4, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = 0x000001,
                 .nack_reason = NACK_TRIMMED, .vendor_info = 0, .entropy = 0xDEF0, .spdcid = REQ_QP_4, .dpdcid = REMOTE_QPN+7, .nack_psn = 0x000001, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = true, .after_cycles = RETRANSMIT}),
        ACK_TC ({.name = "MRC ACK F05", .opcode = MRC_ACK, .p_key = 0xFFFF, .dqp = REQ_QP_4, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = 0x000000,
                 .syndrome = 0x1F, .msn = 0, .exp_completions = 3, .after_cycles = BACK_TO_BACK}),       // 0xFFFFFE, 0xFFFFFF, 0x000000

        // QP C. NACK retry limit (MRC_REQ_MAX_RETRIES = 7): every retriable reason retransmits, the 8th NACK puts the QP in ERROR
        NACK_TC({.name = "MRC NACK C01", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_1, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+0,
                 .nack_reason = NACK_TRIMMED, .vendor_info = 0, .entropy = 0x4321, .spdcid = REQ_QP_1, .dpdcid = REMOTE_QPN+4, .nack_psn = REQ_PSN+0, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = true, .after_cycles = RETRANSMIT}),
        NACK_TC({.name = "MRC NACK C02", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_1, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+0,
                 .nack_reason = NACK_TRIMMED_LASTHOP, .vendor_info = 0, .entropy = 0x4321, .spdcid = REQ_QP_1, .dpdcid = REMOTE_QPN+4, .nack_psn = REQ_PSN+0, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = true, .after_cycles = RETRANSMIT}),
        NACK_TC({.name = "MRC NACK C03", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_1, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+0,
                 .nack_reason = NACK_NO_BITMAP, .vendor_info = 0, .entropy = 0x4321, .spdcid = REQ_QP_1, .dpdcid = REMOTE_QPN+4, .nack_psn = REQ_PSN+0, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = true, .after_cycles = RETRANSMIT}),
        NACK_TC({.name = "MRC NACK C04", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_1, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+0,
                 .nack_reason = NACK_NO_PKT_BUFFER, .vendor_info = 0, .entropy = 0x4321, .spdcid = REQ_QP_1, .dpdcid = REMOTE_QPN+4, .nack_psn = REQ_PSN+0, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = true, .after_cycles = RETRANSMIT}),
        NACK_TC({.name = "MRC NACK C05", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_1, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+0,
                 .nack_reason = NACK_NO_RESOURCE, .vendor_info = 0, .entropy = 0x4321, .spdcid = REQ_QP_1, .dpdcid = REMOTE_QPN+4, .nack_psn = REQ_PSN+0, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = true, .after_cycles = RETRANSMIT}),
        NACK_TC({.name = "MRC NACK C06", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_1, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+0,
                 .nack_reason = NACK_PSN_OOR_WINDOW, .vendor_info = 0, .entropy = 0x4321, .spdcid = REQ_QP_1, .dpdcid = REMOTE_QPN+4, .nack_psn = REQ_PSN+0, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = true, .after_cycles = RETRANSMIT}),
        NACK_TC({.name = "MRC NACK C07", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_1, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+0,
                 .nack_reason = NACK_TRIMMED, .vendor_info = 0, .entropy = 0x4321, .spdcid = REQ_QP_1, .dpdcid = REMOTE_QPN+4, .nack_psn = REQ_PSN+0, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = true, .after_cycles = RETRANSMIT}),     // retry 7
        NACK_TC({.name = "MRC NACK C08", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_1, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+0,
                 .nack_reason = NACK_TRIMMED, .vendor_info = 0, .entropy = 0x4321, .spdcid = REQ_QP_1, .dpdcid = REMOTE_QPN+4, .nack_psn = REQ_PSN+0, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = false, .after_cycles = BACK_TO_BACK}),   // -> ERROR
        NACK_TC({.name = "MRC NACK C09", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = REQ_QP_1, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+1,
                 .nack_reason = NACK_NO_RESOURCE, .vendor_info = 0, .entropy = 0x4321, .spdcid = REQ_QP_1, .dpdcid = REMOTE_QPN+4, .nack_psn = REQ_PSN+1, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = false, .after_cycles = BACK_TO_BACK}),
        ACK_TC ({.name = "MRC ACK C10", .opcode = MRC_ACK, .p_key = 0xFFFF, .dqp = REQ_QP_1, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+5,
                 .syndrome = 0x1F, .msn = 0, .exp_completions = 0, .after_cycles = BACK_TO_BACK}),

        // everything left is acknowledged: the timers of QP B and QP F stop
        ACK_TC ({.name = "MRC ACK B13", .opcode = MRC_ACK, .p_key = 0xFFFF, .dqp = REQ_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = REQ_PSN+5,
                 .syndrome = 0x1F, .msn = 0, .exp_completions = 3, .after_cycles = BACK_TO_BACK}),
        ACK_TC ({.name = "MRC ACK F06", .opcode = MRC_ACK, .p_key = 0xFFFF, .dqp = REQ_QP_4, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = 0x000003,
                 .syndrome = 0x1F, .msn = 0, .exp_completions = 3, .after_cycles = DRAIN}),

        // PSN far ahead 
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 01a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+140,
                  .rqmsn = 0, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x3000, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0xEE),
                  .exp_accepted = false, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        
        // in-order ONLY, same va, distinct fills 
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 02a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+0,
                  .rqmsn = 0, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x1000, .r_key = RKEY, .dmalen = 1000, .immDt = 0x00000000, .payload = buildWritePayload(1000,0x01),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+0, .exp_ack_msn = 1, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 02b", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+1,
                  .rqmsn = 0, .msn = 1, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x1000, .r_key = RKEY, .dmalen = 1000, .immDt = 0x00000000, .payload = buildWritePayload(1000,0x02),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+1, .exp_ack_msn = 2, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 02c", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+2,
                  .rqmsn = 0, .msn = 2, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x1000, .r_key = RKEY, .dmalen = 1000, .immDt = 0x00000000, .payload = buildWritePayload(1000,0x03),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+2, .exp_ack_msn = 3, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 02d", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+3,
                  .rqmsn = 0, .msn = 3, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x1000, .r_key = RKEY, .dmalen = 1000, .immDt = 0x00000000, .payload = buildWritePayload(1000,0x04),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+3, .exp_ack_msn = 4, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 02e", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+4,
                  .rqmsn = 0, .msn = 4, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x1000, .r_key = RKEY, .dmalen = 1000, .immDt = 0x00000000, .payload = buildWritePayload(1000,0x05),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+4, .exp_ack_msn = 5, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 02f", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+5,
                  .rqmsn = 0, .msn = 5, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x1000, .r_key = RKEY, .dmalen = 1000, .immDt = 0x00000000, .payload = buildWritePayload(1000,0x06),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+5, .exp_ack_msn = 6, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 02g", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+6,
                  .rqmsn = 0, .msn = 6, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x1000, .r_key = RKEY, .dmalen = 1000, .immDt = 0x00000000, .payload = buildWritePayload(1000,0x07),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+6, .exp_ack_msn = 7, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 02h", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+7,
                  .rqmsn = 0, .msn = 7, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x1000, .r_key = RKEY, .dmalen = 1000, .immDt = 0x00000000, .payload = buildWritePayload(1000,0x08),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+7, .exp_ack_msn = 8, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),

        // exp_accepted, then its duplicate (not rewritten, re-ACKed)
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 03a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+8,
                  .rqmsn = 0, .msn = 8, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x4000, .r_key = RKEY, .dmalen = 1000, .immDt = 0x00000000, .payload = buildWritePayload(1000,0x09),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+8, .exp_ack_msn = 9, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 03b", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+8,
                  .rqmsn = 0, .msn = 8, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x4000, .r_key = RKEY, .dmalen = 1000, .immDt = 0x00000000, .payload = buildWritePayload(1000,0x09),
                  .exp_accepted = false, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+8, .exp_ack_msn = 9, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),

        // older PSNs, not rewritten, re-ACKed with cack, not their own PSN
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 04a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+4,
                  .rqmsn = 0, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x5000, .r_key = RKEY, .dmalen = 1024, .immDt = 0x00000000, .payload = buildWritePayload(1024,0xEE),
                  .exp_accepted = false, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+8, .exp_ack_msn = 9, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 04b", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0-21,
                  .rqmsn = 0, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x5000, .r_key = RKEY, .dmalen = 1024, .immDt = 0x00000000, .payload = buildWritePayload(1024,0xEE),
                  .exp_accepted = false, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+8, .exp_ack_msn = 9, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 04c", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+8,
                  .rqmsn = 0, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x5000, .r_key = RKEY, .dmalen = 1024, .immDt = 0x00000000, .payload = buildWritePayload(1024,0xEE),
                  .exp_accepted = false, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+8, .exp_ack_msn = 9, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),

        // multi-packet, RETH on every packet 
        WRITE_TC({.name = "MRC_RDMA_WRITE_FIRST 05a", .opcode = MRC_RDMA_WRITE_FIRST, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+9,
                  .rqmsn = 0, .msn = 9, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x10000, .r_key = RKEY, .dmalen = 2*PMTU+100, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0xA1),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+9, .exp_ack_msn = 9, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        PROBE_TC({.name = "MRC PROBE 05a+", .opcode = MRC_PROBE_REQ, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = 0,
                  .vendor_info = 0x5A, .probe_id = 0x3001, .spdcid = RSP_QP_0, .dpdcid = REMOTE_QPN, .tx_timestamp = 0x1234, .tsr = 0, .ftype = MRC_FTYPE_TS, .after_cycles = BACK_TO_BACK}),
        ENDPOINT_REQ_TC({.name = "MRC EP REQ 05b+", .opcode = MRC_EP_REQ, .p_key = 0xFFFF, .dqp = MRC_EV_PROBE_DQP, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = 0x3002,
                         .op = EP_OP_PORT_STATUS, .vendor_info = 0x5A, .port_status_mask = 0xA5A5A5A5, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_MIDDLE 05b", .opcode = MRC_RDMA_WRITE_MIDDLE, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+10,
                  .rqmsn = 0, .msn = 9, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x10000+PMTU, .r_key = RKEY, .dmalen = 2*PMTU+100, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0xA2),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+10, .exp_ack_msn = 9, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        PROBE_TC({.name = "MRC PROBE 05a+", .opcode = MRC_PROBE_REQ, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = 0,
                  .vendor_info = 0x5A, .probe_id = 0x3001, .spdcid = RSP_QP_0, .dpdcid = REMOTE_QPN, .tx_timestamp = 0x1234, .tsr = 0, .ftype = MRC_FTYPE_TS, .after_cycles = BACK_TO_BACK}),
        ENDPOINT_REQ_TC({.name = "MRC EP REQ 05b+", .opcode = MRC_EP_REQ, .p_key = 0xFFFF, .dqp = MRC_EV_PROBE_DQP, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = 0x3002,
                         .op = EP_OP_PORT_STATUS, .vendor_info = 0x5A, .port_status_mask = 0xA5A5A5A5, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_LAST 05c", .opcode = MRC_RDMA_WRITE_LAST, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+11,
                  .rqmsn = 0, .msn = 9, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x10000+2*PMTU, .r_key = RKEY, .dmalen = 2*PMTU+100, .immDt = 0x00000000, .payload = buildWritePayload(100,0xA3),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+11, .exp_ack_msn = 10, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),

        // TSETH on/off 
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 06a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 1, .psn = FIRST_RX_PSN_0+12,
                  .rqmsn = 0, .msn = 10, .tx_timestamp = 0x1234, .tsr = 0, .ftype = 1, .va = 0x20000, .r_key = RKEY, .dmalen = 700, .immDt = 0x00000000, .payload = buildWritePayload(700,0xB0),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+12, .exp_ack_msn = 11, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_FIRST 06b", .opcode = MRC_RDMA_WRITE_FIRST, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 1, .psn = FIRST_RX_PSN_0+13,
                  .rqmsn = 0, .msn = 11, .tx_timestamp = 0x1234, .tsr = 0, .ftype = 1, .va = 0x21000, .r_key = RKEY, .dmalen = 2*PMTU+50, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0xB1),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+13, .exp_ack_msn = 11, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_MIDDLE 06c", .opcode = MRC_RDMA_WRITE_MIDDLE, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 1, .psn = FIRST_RX_PSN_0+14,
                  .rqmsn = 0, .msn = 11, .tx_timestamp = 0x1234, .tsr = 0, .ftype = 1, .va = 0x21000+PMTU, .r_key = RKEY, .dmalen = 2*PMTU+50, .immDt = 0x00000000,
                  .payload = buildWritePayload(PMTU,0xB2), .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+14, .exp_ack_msn = 11, .exp_syndrome = 0x00, .exp_imm = {},
                  .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_LAST 06d", .opcode = MRC_RDMA_WRITE_LAST, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 1, .psn = FIRST_RX_PSN_0+15,
                  .rqmsn = 0, .msn = 11, .tx_timestamp = 0x1234, .tsr = 0, .ftype = 1, .va = 0x21000+2*PMTU, .r_key = RKEY, .dmalen = 2*PMTU+50, .immDt = 0x00000000,
                  .payload = buildWritePayload(50,0xB3), .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+15, .exp_ack_msn = 12, .exp_syndrome = 0x00, .exp_imm = {},
                  .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_FIRST 06e", .opcode = MRC_RDMA_WRITE_FIRST, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+16,
                  .rqmsn = 0, .msn = 12, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x24000, .r_key = RKEY, .dmalen = 2*PMTU+50, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0xC1),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+16, .exp_ack_msn = 12, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_MIDDLE 06f", .opcode = MRC_RDMA_WRITE_MIDDLE, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 1, .psn = FIRST_RX_PSN_0+17,
                  .rqmsn = 0, .msn = 12, .tx_timestamp = 0x1234, .tsr = 0, .ftype = 1, .va = 0x24000+PMTU, .r_key = RKEY, .dmalen = 2*PMTU+50, .immDt = 0x00000000,
                  .payload = buildWritePayload(PMTU,0xC2), .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+17, .exp_ack_msn = 12, .exp_syndrome = 0x00, .exp_imm = {},
                  .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_LAST 06g", .opcode = MRC_RDMA_WRITE_LAST, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+18,
                  .rqmsn = 0, .msn = 12, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x24000+2*PMTU, .r_key = RKEY, .dmalen = 2*PMTU+50, .immDt = 0x00000000, .payload = buildWritePayload(50,0xC3),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+18, .exp_ack_msn = 13, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),

        // rtx bit 
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 07a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 1, .ts = 0, .psn = FIRST_RX_PSN_0+19,
                  .rqmsn = 0, .msn = 13, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x30000, .r_key = RKEY, .dmalen = 200, .immDt = 0x00000000, .payload = buildWritePayload(200,0xD0),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+19, .exp_ack_msn = 14, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),

        // sizes around the 64-byte beat boundary 
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 08a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+20,
                  .rqmsn = 0, .msn = 14, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x40000, .r_key = RKEY, .dmalen = 1, .immDt = 0x00000000, .payload = buildWritePayload(1, 0x10),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+20, .exp_ack_msn = 15, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 08b", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+21,
                  .rqmsn = 0, .msn = 15, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x40100, .r_key = RKEY, .dmalen = 44, .immDt = 0x00000000, .payload = buildWritePayload(44,0x20),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+21, .exp_ack_msn = 16, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 08c", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+22,
                  .rqmsn = 0, .msn = 16, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x40200, .r_key = RKEY, .dmalen = 45, .immDt = 0x00000000, .payload = buildWritePayload(45,0x30),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+22, .exp_ack_msn = 17, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 08d", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+23,
                  .rqmsn = 0, .msn = 17, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x40300, .r_key = RKEY, .dmalen = 48, .immDt = 0x00000000, .payload = buildWritePayload(48,0x40),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+23, .exp_ack_msn = 18, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 08e", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+24,
                  .rqmsn = 0, .msn = 18, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x40400, .r_key = RKEY, .dmalen = 49, .immDt = 0x00000000, .payload = buildWritePayload(49,0x50),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+24, .exp_ack_msn = 19, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 08f", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 1, .psn = FIRST_RX_PSN_0+25,
                  .rqmsn = 0, .msn = 19, .tx_timestamp = 0x1234, .tsr = 0, .ftype = 1, .va = 0x40500, .r_key = RKEY, .dmalen = 40, .immDt = 0x00000000, .payload = buildWritePayload(40,0x60),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+25, .exp_ack_msn = 20, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 08g", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 1, .psn = FIRST_RX_PSN_0+26,
                  .rqmsn = 0, .msn = 20, .tx_timestamp = 0x1234, .tsr = 0, .ftype = 1, .va = 0x40600, .r_key = RKEY, .dmalen = 48, .immDt = 0x00000000, .payload = buildWritePayload(48,0x70),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+26, .exp_ack_msn = 21, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 08h", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 1, .psn = FIRST_RX_PSN_0+27,
                  .rqmsn = 0, .msn = 21, .tx_timestamp = 0x1234, .tsr = 0, .ftype = 1, .va = 0x40700, .r_key = RKEY, .dmalen = 49, .immDt = 0x00000000, .payload = buildWritePayload(49,0x80),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+27, .exp_ack_msn = 22, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),

        // ONLY todo: add control packets in the middle
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 09a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+28,
                  .rqmsn = 0, .msn = 22, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x50000, .r_key = RKEY, .dmalen = 256, .immDt = 0x00000000, .payload = buildWritePayload(256,0x90),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+28, .exp_ack_msn = 23, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        ACK_TC ({.name = "MRC ACK 09b", .opcode = MRC_ACK, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = 0x0000FF,
                 .syndrome = 0x1F, .msn = 0, .exp_completions = 0, .after_cycles = BACK_TO_BACK}),
        SACK_TC({.name = "MRC SACK 09c", .opcode = MRC_SACK, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = 0x0000FF,
                 .m = SACK_M_NONE, .pr = 0, .ack_psn_offset = 0, .entropy = 0, .spdcid = RSP_QP_0, .dpdcid = REMOTE_QPN, .cack_psn = 0x0000FF, .cc_type = MRC_CC_TYPE_DEFAULT, .cc_fl = 0, .mpr = 0, .sack_offset = 0, .sack_bitmap = 0,
                 .tx_timestamp = 0, .ooo_count = 0, .r_c = 0, .rcv_cwnd_pen = 0, .rcvd_bytes = 0, .after_cycles = BACK_TO_BACK}),
        NACK_TC({.name = "MRC NACK 09d", .opcode = MRC_NACK, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = 0x000100,
                 .nack_reason = NACK_TRIMMED, .vendor_info = 0, .entropy = 0, .spdcid = RSP_QP_0, .dpdcid = REMOTE_QPN, .nack_psn = 0x000100, .cc_type = 0x2, .cc_fl = 0, .tx_timestamp = 0x0000,
                 .exp_retransmit = false, .after_cycles = BACK_TO_BACK}),
        PROBE_TC({.name = "MRC PROBE 09e", .opcode = MRC_PROBE_REQ, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = 0,
                  .vendor_info = 0, .probe_id = 0x3003, .spdcid = RSP_QP_0, .dpdcid = REMOTE_QPN, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .after_cycles = BACK_TO_BACK}),
        ENDPOINT_REQ_TC({.name = "MRC EP REQ 09f", .opcode = MRC_EP_REQ, .p_key = 0xFFFF, .dqp = MRC_EV_PROBE_DQP, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = 0x3004,
                         .op = EP_OP_EV_PROBE, .vendor_info = 0, .port_status_mask = 0, .tx_timestamp = 0x2222, .tsr = 0, .ftype = MRC_FTYPE_TS, .after_cycles = BACK_TO_BACK}),
        ENDPOINT_RES_TC({.name = "MRC EP RES 09g", .opcode = MRC_EP_RSP, .p_key = 0xFFFF, .dqp = MRC_EV_PROBE_DQP, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = 0x3004,
                         .op = EP_OP_EV_PROBE, .tx_timestamp = 0x3333, .after_cycles = BACK_TO_BACK}),

        // Write with Immediate 
        WRITE_TC({.name = "MRC_RDMA_WRITE_FIRST 10a", .opcode = MRC_RDMA_WRITE_FIRST, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+29,
                  .rqmsn = 1, .msn = 23, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x51000, .r_key = RKEY, .dmalen = PMTU+100, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0xE1),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+29, .exp_ack_msn = 23, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_LAST_IMM 10b", .opcode = MRC_RDMA_WRITE_LAST_IMM, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+30,
                  .rqmsn = 1, .msn = 23, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x51000+PMTU, .r_key = RKEY, .dmalen = PMTU+100, .immDt = 0x10B10B10, .payload = buildWritePayload(100,0xE2),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+30, .exp_ack_msn = 24, .exp_syndrome = 0x00, .exp_imm = {0x10B10B10}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY_IMM 10c", .opcode = MRC_RDMA_WRITE_ONLY_IMM, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+31,
                  .rqmsn = 2, .msn = 24, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x52000, .r_key = RKEY, .dmalen = 300, .immDt = 0x10C10C10, .payload = buildWritePayload(300,0xE3),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+31, .exp_ack_msn = 25, .exp_syndrome = 0x00, .exp_imm = {0x10C10C10}, .after_cycles = BACK_TO_BACK}),

        // PSN ahead (repo: NAK once, then silent), in order resets it 
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 11a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+182,
                  .rqmsn = 0, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x2000, .r_key = RKEY, .dmalen = 1024, .immDt = 0x00000000, .payload = buildWritePayload(1024,0xEE),
                  .exp_accepted = false, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}), 
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 11b", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+182,
                  .rqmsn = 0, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x2000, .r_key = RKEY, .dmalen = 1024, .immDt = 0x00000000, .payload = buildWritePayload(1024,0xEE),
                  .exp_accepted = false, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}), 
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 11c", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+182,
                  .rqmsn = 0, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x2000, .r_key = RKEY, .dmalen = 1024, .immDt = 0x00000000, .payload = buildWritePayload(1024,0xEE),
                  .exp_accepted = false, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}), 
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 11d", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+32,
                  .rqmsn = 0, .msn = 25, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x2000, .r_key = RKEY, .dmalen = 1024, .immDt = 0x00000000, .payload = buildWritePayload(1024,0x11),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+32, .exp_ack_msn = 26, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 11e", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+183,
                  .rqmsn = 0, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x2000, .r_key = RKEY, .dmalen = 1024, .immDt = 0x00000000, .payload = buildWritePayload(1024,0xEE),
                  .exp_accepted = false, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}), 
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 11f", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+183,
                  .rqmsn = 0, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x2000, .r_key = RKEY, .dmalen = 1024, .immDt = 0x00000000, .payload = buildWritePayload(1024,0xEE),
                  .exp_accepted = false, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),

        // burst
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 12a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+33,
                  .rqmsn = 0, .msn = 26, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x60000, .r_key = RKEY, .dmalen = 500, .immDt = 0x00000000, .payload = buildWritePayload(500,0xF1),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+33, .exp_ack_msn = 27, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 12b", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 1, .psn = FIRST_RX_PSN_0+34,
                  .rqmsn = 0, .msn = 27, .tx_timestamp = 0x1234, .tsr = 0, .ftype = 1, .va = 0x61000, .r_key = RKEY, .dmalen = 45, .immDt = 0x00000000, .payload = buildWritePayload(45,0xF2),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+34, .exp_ack_msn = 28, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_FIRST 12c", .opcode = MRC_RDMA_WRITE_FIRST, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+35,
                  .rqmsn = 0, .msn = 28, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x62000, .r_key = RKEY, .dmalen = 2*PMTU+10, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0xF3),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+35, .exp_ack_msn = 28, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_MIDDLE 12d", .opcode = MRC_RDMA_WRITE_MIDDLE, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+36,
                  .rqmsn = 0, .msn = 28, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x62000+PMTU, .r_key = RKEY, .dmalen = 2*PMTU+10, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0xF4),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+36, .exp_ack_msn = 28, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_LAST 12e", .opcode = MRC_RDMA_WRITE_LAST, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+37,
                  .rqmsn = 0, .msn = 28, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x62000+2*PMTU, .r_key = RKEY, .dmalen = 2*PMTU+10, .immDt = 0x00000000, .payload = buildWritePayload(10,0xF5),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+37, .exp_ack_msn = 29, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        
        // same as before but waiting random amount of cycles in between
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 13a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+38,
                  .rqmsn = 0, .msn = 29, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x60000, .r_key = RKEY, .dmalen = 500, .immDt = 0x00000000, .payload = buildWritePayload(500,0xF1),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+38, .exp_ack_msn = 30, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 13b", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 1, .psn = FIRST_RX_PSN_0+39,
                  .rqmsn = 0, .msn = 30, .tx_timestamp = 0x1234, .tsr = 0, .ftype = 1, .va = 0x61000, .r_key = RKEY, .dmalen = 45, .immDt = 0x00000000, .payload = buildWritePayload(45,0xF2),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+39, .exp_ack_msn = 31, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_FIRST 13c", .opcode = MRC_RDMA_WRITE_FIRST, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+40,
                  .rqmsn = 0, .msn = 31, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x62000, .r_key = RKEY, .dmalen = 2*PMTU+10, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0xF3),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+40, .exp_ack_msn = 31, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = 2}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_MIDDLE 13d", .opcode = MRC_RDMA_WRITE_MIDDLE, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+41,
                  .rqmsn = 0, .msn = 31, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x62000+PMTU, .r_key = RKEY, .dmalen = 2*PMTU+10, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0xF4),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+41, .exp_ack_msn = 31, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = 200}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_LAST 13e", .opcode = MRC_RDMA_WRITE_LAST, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+42,
                  .rqmsn = 0, .msn = 31, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x62000+2*PMTU, .r_key = RKEY, .dmalen = 2*PMTU+10, .immDt = 0x00000000, .payload = buildWritePayload(10,0xF5),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+42, .exp_ack_msn = 32, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = 1000}),

        // BTH.a = 0 -> written, MSN still advances, but no ACK. Each a=0 row is followed by an a=1 row
        // in the same group, so a stray ACK would be caught by the next ACK check
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 14a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+43,
                  .rqmsn = 0, .msn = 32, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x80000, .r_key = RKEY, .dmalen = 128, .immDt = 0x00000000, .payload = buildWritePayload(128,0x14),
                  .exp_accepted = true, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 14b", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+44,
                  .rqmsn = 0, .msn = 33, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x80100, .r_key = RKEY, .dmalen = 128, .immDt = 0x00000000, .payload = buildWritePayload(128,0x15),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+44, .exp_ack_msn = 34, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_FIRST 14c", .opcode = MRC_RDMA_WRITE_FIRST, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+45,
                  .rqmsn = 0, .msn = 34, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x81000, .r_key = RKEY, .dmalen = 2*PMTU+30, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0x16),
                  .exp_accepted = true, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_MIDDLE 14d", .opcode = MRC_RDMA_WRITE_MIDDLE, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+46,
                  .rqmsn = 0, .msn = 34, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x81000+PMTU, .r_key = RKEY, .dmalen = 2*PMTU+30, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0x17),
                  .exp_accepted = true, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_LAST 14e", .opcode = MRC_RDMA_WRITE_LAST, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+47,
                  .rqmsn = 0, .msn = 34, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x81000+2*PMTU, .r_key = RKEY, .dmalen = 2*PMTU+30, .immDt = 0x00000000, .payload = buildWritePayload(30,0x18),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+47, .exp_ack_msn = 35, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),

        // two multi-packet messages out of order: A = +48..+51, B = +52..+54. Packets beyond the hole are written
        // but not ACKed, a duplicate inside the window is ACKed with the current cack_psn. 15h closes the hole:
        // cack_psn jumps to +54 and the pending ACK requests of 15c and 15e produce one ACK, MSN +2, although 15h has a = 0
        WRITE_TC({.name = "MRC_RDMA_WRITE_MIDDLE 15a", .opcode = MRC_RDMA_WRITE_MIDDLE, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+53,
                  .rqmsn = 0, .msn = 36, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x86000+PMTU, .r_key = RKEY, .dmalen = 2*PMTU+50, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0x40),
                  .exp_accepted = true, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_MIDDLE 15b", .opcode = MRC_RDMA_WRITE_MIDDLE, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+50,
                  .rqmsn = 0, .msn = 35, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x82000+2*PMTU, .r_key = RKEY, .dmalen = 3*PMTU+100, .immDt = 0x00000000,
                  .payload = buildWritePayload(PMTU,0x41), .exp_accepted = true, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {},
                  .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_LAST 15c", .opcode = MRC_RDMA_WRITE_LAST, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+54,
                  .rqmsn = 0, .msn = 36, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x86000+2*PMTU, .r_key = RKEY, .dmalen = 2*PMTU+50, .immDt = 0x00000000, .payload = buildWritePayload(50,0x42),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+47, .exp_ack_msn = 35, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        // for ackmap
        //WRITE_TC({.name = "MRC_RDMA_WRITE_FIRST 15d", .opcode = MRC_RDMA_WRITE_FIRST, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+48,
        //          .rqmsn = 0, .msn = 35, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x82000, .r_key = RKEY, .dmalen = 3*PMTU+100, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0x43),
        //          .exp_accepted = true, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        // for prevAr
        WRITE_TC({.name = "MRC_RDMA_WRITE_FIRST 15d", .opcode = MRC_RDMA_WRITE_FIRST, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+48,
                  .rqmsn = 0, .msn = 35, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x82000, .r_key = RKEY, .dmalen = 3*PMTU+100, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0x43),
                  .exp_accepted = true, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_LAST 15e", .opcode = MRC_RDMA_WRITE_LAST, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+51,
                  .rqmsn = 0, .msn = 35, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x82000+3*PMTU, .r_key = RKEY, .dmalen = 3*PMTU+100, .immDt = 0x00000000, .payload = buildWritePayload(100,0x44),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+48, .exp_ack_msn = 35, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_FIRST 15f", .opcode = MRC_RDMA_WRITE_FIRST, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+52,
                  .rqmsn = 0, .msn = 36, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x86000, .r_key = RKEY, .dmalen = 2*PMTU+50, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0x45),
                  .exp_accepted = true, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_MIDDLE 15g", .opcode = MRC_RDMA_WRITE_MIDDLE, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+53,
                  .rqmsn = 0, .msn = 36, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x86000+PMTU, .r_key = RKEY, .dmalen = 2*PMTU+50, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0xEE),
                  .exp_accepted = false, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+48, .exp_ack_msn = 35, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_MIDDLE 15h", .opcode = MRC_RDMA_WRITE_MIDDLE, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+49,
                  .rqmsn = 0, .msn = 35, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x82000+PMTU, .r_key = RKEY, .dmalen = 3*PMTU+100, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0x46),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+54, .exp_ack_msn = 37, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),

        // window edge (epsn = +55): epsn+128 is dropped silently, epsn+127 is written but not ACKed (hole below it),
        // epsn+128 is still dropped because accepting a packet beyond a hole does not move the window
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 16a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+183,
                  .rqmsn = 0, .msn = 37, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x83000, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0xEE),
                  .exp_accepted = false, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        //WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 16b", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+182,
        //          .rqmsn = 0, .msn = 37, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x83100, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0x1B),
        //          .exp_accepted = true, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 16b", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+182,
                  .rqmsn = 0, .msn = 37, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x83100, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0x1B),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+54, .exp_ack_msn = 37, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 16c", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+183,
                  .rqmsn = 0, .msn = 38, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x83200, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0xEE),
                  .exp_accepted = false, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),

        // out-of-window packets are dropped silently whatever BTH.a; the duplicate and in-order packets around them are unaffected
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 17a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+300,
                  .rqmsn = 0, .msn = 39, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x84000, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0xEE),
                  .exp_accepted = false, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 17b", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+54,
                  .rqmsn = 0, .msn = 38, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x84000, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0xEE),
                  .exp_accepted = false, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+54, .exp_ack_msn = 37, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 17c", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+300,
                  .rqmsn = 0, .msn = 39, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x84000, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0xEE),
                  .exp_accepted = false, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 17d", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+55,
                  .rqmsn = 0, .msn = 39, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x84100, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0x1D),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+55, .exp_ack_msn = 38, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 17e", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+301,
                  .rqmsn = 0, .msn = 40, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x84000, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0xEE),
                  .exp_accepted = false, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 17f", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+56,
                  .rqmsn = 0, .msn = 40, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x84200, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0x1E),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+56, .exp_ack_msn = 39, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),

        // duplicates are ACKed with the current cack_psn whatever BTH.a and opcode, and never rewritten
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 18a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+56,
                  .rqmsn = 0, .msn = 40, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x84200, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0xEE),
                  .exp_accepted = false, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+56, .exp_ack_msn = 39, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_MIDDLE 18b", .opcode = MRC_RDMA_WRITE_MIDDLE, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+46,
                  .rqmsn = 0, .msn = 34, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x81000+PMTU, .r_key = RKEY, .dmalen = 2*PMTU+30, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0xEE),
                  .exp_accepted = false, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+56, .exp_ack_msn = 39, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 18c", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_0+57,
                  .rqmsn = 0, .msn = 41, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x85000, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0x1F),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_0+57, .exp_ack_msn = 40, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),

        // QP 0x06 (epsn 0xFFFFFE): multi-packet write in order across the 24-bit wrap, then a duplicate behind the wrap
        WRITE_TC({.name = "MRC_RDMA_WRITE_FIRST 19a", .opcode = MRC_RDMA_WRITE_FIRST, .p_key = 0xFFFF, .dqp = RSP_QP_1, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = 0xFFFFFE,
                  .rqmsn = 0, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x70000, .r_key = RKEY, .dmalen = 2*PMTU+20, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0x21),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = 0xFFFFFE, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_MIDDLE 19b", .opcode = MRC_RDMA_WRITE_MIDDLE, .p_key = 0xFFFF, .dqp = RSP_QP_1, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = 0xFFFFFF,
                  .rqmsn = 0, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x70000+PMTU, .r_key = RKEY, .dmalen = 2*PMTU+20, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0x22),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = 0xFFFFFF, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_LAST 19c", .opcode = MRC_RDMA_WRITE_LAST, .p_key = 0xFFFF, .dqp = RSP_QP_1, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = 0x000000,
                  .rqmsn = 0, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x70000+2*PMTU, .r_key = RKEY, .dmalen = 2*PMTU+20, .immDt = 0x00000000, .payload = buildWritePayload(20,0x23),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = 0x000000, .exp_ack_msn = 1, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 19d", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_1, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = 0xFFFFFF,
                  .rqmsn = 0, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x71000, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0xEE),
                  .exp_accepted = false, .exp_ack = true, .exp_cack_psn = 0x000000, .exp_ack_msn = 1, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 19e", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_1, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = 0x000001,
                  .rqmsn = 0, .msn = 1, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x71100, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0x24),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = 0x000001, .exp_ack_msn = 2, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),

        // QP 0x07 (epsn 0xFFFFFB): ahead by 7 ACROSS the wrap. Expected = same behaviour as 15a (accepted).
        // QP 0x0A (epsn 0xFFFFFB): ahead by 7 ACROSS the wrap: written, ACK request pending until 0xFFFFFB..0x000001 arrive
        // QP 0x0A (epsn 0xFFFFFB): ahead by 7 across the wrap. Written and ACKed at once with cack 0xFFFFFA because BTH.a = 1
        // (like 16b), the ACK request also stays pending (prevAr) until 0xFFFFFB..0x000001 arrive
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 20a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_2, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = 0x000002,
                  .rqmsn = 0, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x78000, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0x25),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = 0xFFFFFA, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),                
        // QP 0x10, fresh (epsn FIRST_RX_PSN_3). Write with Immediate: TSETH + ImmDt layout, non-zero immDt / RQMSN,
        // LAST_IMM ahead of a hole (stale ACK), the hole closed by an a = 0 FIRST (prevAr ACK), then a duplicate LAST_IMM
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY_IMM 21a", .opcode = MRC_RDMA_WRITE_ONLY_IMM, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 1, .psn = FIRST_RX_PSN_3+0,
                  .rqmsn = 1, .msn = 0, .tx_timestamp = 0x1234, .tsr = 0, .ftype = 1, .va = 0x90000, .r_key = RKEY, .dmalen = 64, .immDt = 0xCAFEBABE, .payload = buildWritePayload(64,0x31),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+0, .exp_ack_msn = 1, .exp_syndrome = 0x00, .exp_imm = {0xCAFEBABE}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_FIRST 21b", .opcode = MRC_RDMA_WRITE_FIRST, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 1, .psn = FIRST_RX_PSN_3+1,
                  .rqmsn = 2, .msn = 1, .tx_timestamp = 0x1234, .tsr = 0, .ftype = 1, .va = 0x91000, .r_key = RKEY, .dmalen = PMTU+64, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0x32),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+1, .exp_ack_msn = 1, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_LAST_IMM 21c", .opcode = MRC_RDMA_WRITE_LAST_IMM, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 1, .psn = FIRST_RX_PSN_3+2,
                  .rqmsn = 2, .msn = 1, .tx_timestamp = 0x1234, .tsr = 0, .ftype = 1, .va = 0x91000+PMTU, .r_key = RKEY, .dmalen = PMTU+64, .immDt = 0x12345678, .payload = buildWritePayload(64,0x33),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+2, .exp_ack_msn = 2, .exp_syndrome = 0x00, .exp_imm = {0x12345678}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_LAST_IMM 21d", .opcode = MRC_RDMA_WRITE_LAST_IMM, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+4,
                  .rqmsn = 3, .msn = 2, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x92000+PMTU, .r_key = RKEY, .dmalen = PMTU+64, .immDt = 0x0BADF00D, .payload = buildWritePayload(64,0x34),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+2, .exp_ack_msn = 2, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_FIRST 21e", .opcode = MRC_RDMA_WRITE_FIRST, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+3,
                  .rqmsn = 3, .msn = 2, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x92000, .r_key = RKEY, .dmalen = PMTU+64, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0x35),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+4, .exp_ack_msn = 3, .exp_syndrome = 0x00, .exp_imm = {0x0BADF00D}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_LAST_IMM 21f", .opcode = MRC_RDMA_WRITE_LAST_IMM, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+4,
                  .rqmsn = 3, .msn = 2, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x92000+PMTU, .r_key = RKEY, .dmalen = PMTU+64, .immDt = 0x0BADF00D, .payload = buildWritePayload(64,0xEE),
                  .exp_accepted = false, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+4, .exp_ack_msn = 3, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),

        // LAST corner cases: a full-PMTU LAST with a = 0 completes its message without an ACK (seen through the MSN of the
        // following duplicate, which also has rtx = 1); a LAST ahead of a hole gets a stale ACK; the hole is closed by a
        // FIRST with a = 0 but rtx = 1; a LAST beyond the window is dropped silently (the next ONLY would catch a stray ACK)
        WRITE_TC({.name = "MRC_RDMA_WRITE_FIRST 22a", .opcode = MRC_RDMA_WRITE_FIRST, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+5,
                  .rqmsn = 0, .msn = 3, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x93000, .r_key = RKEY, .dmalen = 2*PMTU, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0x41),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+5, .exp_ack_msn = 3, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_LAST 22b", .opcode = MRC_RDMA_WRITE_LAST, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+6,
                  .rqmsn = 0, .msn = 3, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x93000+PMTU, .r_key = RKEY, .dmalen = 2*PMTU, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0x42),
                  .exp_accepted = true, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_LAST 22c", .opcode = MRC_RDMA_WRITE_LAST, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 1, .ts = 0, .psn = FIRST_RX_PSN_3+6,
                  .rqmsn = 0, .msn = 3, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x93000+PMTU, .r_key = RKEY, .dmalen = 2*PMTU, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0xEE),
                  .exp_accepted = false, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+6, .exp_ack_msn = 4, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_LAST 22d", .opcode = MRC_RDMA_WRITE_LAST, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+8,
                  .rqmsn = 0, .msn = 4, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x94000+PMTU, .r_key = RKEY, .dmalen = PMTU+32, .immDt = 0x00000000, .payload = buildWritePayload(32,0x43),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+6, .exp_ack_msn = 4, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_FIRST 22e", .opcode = MRC_RDMA_WRITE_FIRST, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 0, .R = 0, .rtx = 1, .ts = 0, .psn = FIRST_RX_PSN_3+7,
                  .rqmsn = 0, .msn = 4, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x94000, .r_key = RKEY, .dmalen = PMTU+32, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0x44),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+8, .exp_ack_msn = 5, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_LAST 22f", .opcode = MRC_RDMA_WRITE_LAST, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+137,
                  .rqmsn = 0, .msn = 5, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x95000, .r_key = RKEY, .dmalen = PMTU+32, .immDt = 0x00000000, .payload = buildWritePayload(32,0xEE),
                  .exp_accepted = false, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 22g", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+9,
                  .rqmsn = 0, .msn = 5, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x95100, .r_key = RKEY, .dmalen = 32, .immDt = 0x00000000, .payload = buildWritePayload(32,0x45),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+9, .exp_ack_msn = 6, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),

        // zero-length WRITE ONLY (0 <= only <= PMTU): ACKed and completed with no data, the next write is unaffected
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 23a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+10,
                  .rqmsn = 0, .msn = 6, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x96000, .r_key = RKEY, .dmalen = 0, .immDt = 0x00000000, .payload = buildWritePayload(0,0x00),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+10, .exp_ack_msn = 7, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 23b", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+11,
                  .rqmsn = 0, .msn = 7, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x96100, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0x51),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+11, .exp_ack_msn = 8, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),
        
        // padding (BTH.PadCnt): payloads of 4n+3 bytes (pad 1) and 4n+2 bytes (pad 2) on ONLY, ONLY_IMM with TSETH and LAST_IMM.
        // The pad bytes are counted in the UDP length but must not reach the memory
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY_IMM 24a", .opcode = MRC_RDMA_WRITE_ONLY_IMM, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 1, .psn = FIRST_RX_PSN_3+12,
                  .rqmsn = 4, .msn = 8, .tx_timestamp = 0x1234, .tsr = 0, .ftype = 1, .va = 0x97000, .r_key = RKEY, .dmalen = 63, .immDt = 0xA1B2C3D4, .payload = buildWritePayload(63,0x61),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+12, .exp_ack_msn = 9, .exp_syndrome = 0x00, .exp_imm = {0xA1B2C3D4}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_FIRST 24b", .opcode = MRC_RDMA_WRITE_FIRST, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+13,
                  .rqmsn = 5, .msn = 9, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x98000, .r_key = RKEY, .dmalen = PMTU+2, .immDt = 0x00000000, .payload = buildWritePayload(PMTU,0x62),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+13, .exp_ack_msn = 9, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_LAST_IMM 24c", .opcode = MRC_RDMA_WRITE_LAST_IMM, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+14,
                  .rqmsn = 5, .msn = 9, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x98000+PMTU, .r_key = RKEY, .dmalen = PMTU+2, .immDt = 0x5A5A5A5A, .payload = buildWritePayload(2,0x63),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+14, .exp_ack_msn = 10, .exp_syndrome = 0x00, .exp_imm = {0x5A5A5A5A}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 24d", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+15,
                  .rqmsn = 0, .msn = 10, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x99000, .r_key = RKEY, .dmalen = 3, .immDt = 0x00000000, .payload = buildWritePayload(3,0x64),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+15, .exp_ack_msn = 11, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),
        

        // two WriteIMMs (RQMSN 6, 7) ahead of a hole: both ImmDt are held. The plain ONLY that closes the hole completes
        // three messages and releases both ImmDt from the same packet, in RQMSN order
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY_IMM 25a", .opcode = MRC_RDMA_WRITE_ONLY_IMM, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+17,
                  .rqmsn = 6, .msn = 12, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x9A000, .r_key = RKEY, .dmalen = 64, .immDt = 0x66666666, .payload = buildWritePayload(64,0x71),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+15, .exp_ack_msn = 11, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY_IMM 25b", .opcode = MRC_RDMA_WRITE_ONLY_IMM, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+18,
                  .rqmsn = 7, .msn = 13, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x9B000, .r_key = RKEY, .dmalen = 64, .immDt = 0x77777777, .payload = buildWritePayload(64,0x72),
                  .exp_accepted = true, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 25c", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+16,
                  .rqmsn = 0, .msn = 11, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x9C000, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0x73),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+18, .exp_ack_msn = 14, .exp_syndrome = 0x00, .exp_imm = {0x66666666, 0x77777777}, .after_cycles = DRAIN}),
        
        // reconnect: a packet held ahead of a hole (26a) is left in the old connection. After the QP is set up again (26b)
        // epsn, bitmap, MSN and RQMSN restart: the first WriteIMM completes alone, with MSN 1 and its ImmDt read at RQMSN 1.
        // The hole of the old connection arriving late (26c) is a duplicate: never written
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 26a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 0, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+20,
                  .rqmsn = 0, .msn = 15, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x9D000, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0x81),
                  .exp_accepted = true, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY_IMM 26b", .opcode = MRC_RDMA_WRITE_ONLY_IMM, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+0x100,
                  .rqmsn = 1, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x9E000, .r_key = RKEY, .dmalen = 64, .immDt = 0xC0FFEE01, .payload = buildWritePayload(64,0x82),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+0x100, .exp_ack_msn = 1, .exp_syndrome = 0x00, .exp_imm = {0xC0FFEE01}, .reconnect = true, .after_cycles = BACK_TO_BACK}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 26c", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+19,
                  .rqmsn = 0, .msn = 14, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0x9F000, .r_key = RKEY, .dmalen = 64, .immDt = 0x00000000, .payload = buildWritePayload(64,0x83),
                  .exp_accepted = false, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+0x100, .exp_ack_msn = 1, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),

        // rkey update in the middle of the connection: the second WriteIMM still gets MSN 2 and its own ImmDt (RQMSN 2).
        // A reset would give MSN 1 and the ImmDt of 26b (slot 1)
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY_IMM 27a", .opcode = MRC_RDMA_WRITE_ONLY_IMM, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+0x101,
                  .rqmsn = 2, .msn = 1, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0xA0000, .r_key = RKEY, .dmalen = 64, .immDt = 0xC0FFEE02, .payload = buildWritePayload(64,0x84),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+0x101, .exp_ack_msn = 2, .exp_syndrome = 0x00, .exp_imm = {0xC0FFEE02}, .rkey_update = true, .after_cycles = DRAIN}),

        // an unsupported opcode (0xC4: not a Write, not a control packet) is dropped without touching the QP state.
        // The write that follows is still processed: one PSN ahead, so it is out of order
        WRITE_TC({.name = "UNSUPPORTED 0xC4 28a", .opcode = 0xC4, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+0x102,
                  .rqmsn = 0, .msn = 2, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0xA1000, .r_key = RKEY, .dmalen = 64, .immDt = 0, .payload = buildWritePayload(64,0x91),
                  .exp_accepted = false, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 28b", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = RSP_QP_3, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+0x103,
                  .rqmsn = 0, .msn = 2, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0xA2000, .r_key = RKEY, .dmalen = 64, .immDt = 0, .payload = buildWritePayload(64,0x92),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = FIRST_RX_PSN_3+0x101, .exp_ack_msn = 2, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),
        
        // invalid QPs are silently dropped: 0x110 is beyond MAX_QPS (it aliases QP 0x10, PSN inside its window),
        // 0x20 was never set up (zero state: epsn 0, PSN 0 would be accepted)
        WRITE_TC({.name = "BAD QP 0x110 29a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = 0x110, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+0x102,
                  .rqmsn = 0, .msn = 2, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0xB1000, .r_key = RKEY, .dmalen = 64, .immDt = 0, .payload = buildWritePayload(64,0xA1),
                  .exp_accepted = false, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),
        WRITE_TC({.name = "UNUSED QP 0x20 29b", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = 0x20, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = 0,
                  .rqmsn = 0, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0xB2000, .r_key = RKEY, .dmalen = 64, .immDt = 0, .payload = buildWritePayload(64,0xA2),
                  .exp_accepted = false, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),
        // 24-bit dqp: 0x010010 is QP 0x10 if only 16 bits are kept (PSN inside QP 0x10's window)
        WRITE_TC({.name = "BAD QP 0x010010 29c", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = 0x010010, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = FIRST_RX_PSN_3+0x102,
                  .rqmsn = 0, .msn = 2, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0xB3000, .r_key = RKEY, .dmalen = 64, .immDt = 0, .payload = buildWritePayload(64,0xA3),
                  .exp_accepted = false, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),
        
        // QP D went to ERROR on the AETH NAK of row D04: a Write to it is dropped, although its PSN (0, the local psn of the
        // setup) is the expected one
        WRITE_TC({.name = "QP D IN ERROR 30a", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = REQ_QP_2, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = 0,
                  .rqmsn = 0, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0xB4000, .r_key = RKEY, .dmalen = 64, .immDt = 0, .payload = buildWritePayload(64,0xA4),
                  .exp_accepted = false, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),
        // QP B never went to ERROR (same setup, psn 0): the same Write is accepted
        WRITE_TC({.name = "QP B IN RTS 30b", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = REQ_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = 0,
                  .rqmsn = 0, .msn = 0, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0xB5000, .r_key = RKEY, .dmalen = 64, .immDt = 0, .payload = buildWritePayload(64,0xA5),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = 0, .exp_ack_msn = 1, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),
        
        // max_wimm_inflight = 32 on QP B: after 31a the RQMSN head is 2, so RQMSN 33 (31b, ahead of the hole at PSN 2) still has a
        // slot and RQMSN 34 (31c) does not: NAK Invalid Request (0x61) with its PSN, not written, QP to ERROR (Table 6-15).
        // The hole arriving late (31d) is dropped: the QP is in ERROR, and the ImmDt of 31b is never released
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY_IMM 31a", .opcode = MRC_RDMA_WRITE_ONLY_IMM, .p_key = 0xFFFF, .dqp = REQ_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = 1,
                  .rqmsn = 1, .msn = 1, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0xB6000, .r_key = RKEY, .dmalen = 64, .immDt = 0xB0000001, .payload = buildWritePayload(64,0xA6),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = 1, .exp_ack_msn = 2, .exp_syndrome = 0x00, .exp_imm = {0xB0000001}, .after_cycles = DRAIN}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY_IMM 31b", .opcode = MRC_RDMA_WRITE_ONLY_IMM, .p_key = 0xFFFF, .dqp = REQ_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = 3,
                  .rqmsn = 33, .msn = 3, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0xB7000, .r_key = RKEY, .dmalen = 64, .immDt = 0xB0000021, .payload = buildWritePayload(64,0xA7),
                  .exp_accepted = true, .exp_ack = true, .exp_cack_psn = 1, .exp_ack_msn = 2, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY_IMM 31c", .opcode = MRC_RDMA_WRITE_ONLY_IMM, .p_key = 0xFFFF, .dqp = REQ_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = 4,
                  .rqmsn = 34, .msn = 4, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0xB8000, .r_key = RKEY, .dmalen = 64, .immDt = 0xB0000022, .payload = buildWritePayload(64,0xA8),
                  .exp_accepted = false, .exp_ack = true, .exp_cack_psn = 4, .exp_ack_msn = 2, .exp_syndrome = 0x61, .exp_imm = {}, .after_cycles = DRAIN}),
        WRITE_TC({.name = "MRC_RDMA_WRITE_ONLY 31d", .opcode = MRC_RDMA_WRITE_ONLY, .p_key = 0xFFFF, .dqp = REQ_QP_0, .a = 1, .R = 0, .rtx = 0, .ts = 0, .psn = 2,
                  .rqmsn = 0, .msn = 2, .tx_timestamp = 0, .tsr = 0, .ftype = 0, .va = 0xB9000, .r_key = RKEY, .dmalen = 64, .immDt = 0, .payload = buildWritePayload(64,0xA9),
                  .exp_accepted = false, .exp_ack = false, .exp_cack_psn = 0, .exp_ack_msn = 0, .exp_syndrome = 0x00, .exp_imm = {}, .after_cycles = DRAIN}),
        // RESPONDER SIDE
    };
}