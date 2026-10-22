#include <cstdint>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "rocev2_config.hpp"
#include "util.hpp"
#include "tests.hpp"
#include "mrc_transport_protocol_top.hpp"

#include "axi_utils.hpp"
#include "mrc_transport_protocol/mrc_transport_protocol.hpp"
#include "mrc_transport_protocol/mrc_headers.hpp"
// todo: also add send queues and receive queues

namespace
{
    // requestor
    #define REMOTE_QPN 0x0042
    #define REMOTE_IP 0x0A000002 // 10.0.0.2
    #define REMOTE_UDP_PORT 0xC000

    #define RKEY 0x1234
    #define FIRST_RX_PSN 0x0A0B00 // first rx psn for the first QP tested

    #define PMTU 1024

    #define VERY_BIG_NUMBER 1000

    // local QPs acting as responder (send writes)
    #define RSP_QP_0 0x08   // write 1
    #define RSP_QP_1 0x09   // write 2
    #define RSP_QP_2 0x0A   // write 3
    // local QPs acting as requestor (send ACK/SACK/NACK)
    #define REQ_QP_0 0x0B   // ACK/SACK/NACK
    #define REQ_QP_1 0x0C   // NACK retry limit (qp should go to error state)
    #define REQ_QP_2 0x0D   // AETH NAK (qp should go to error state)
    #define REQ_QP_3 0x0E   // NACK UNEXP_EVENT (qp should go to error state)
    #define REQ_QP_4 0x0F   // 24-bit PSN wrap

    // remove those
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

    struct MrcTransportProtocolTestBench
    {
        hls::stream<ipUdpMeta> rx_meta;
        hls::stream<net_axis<DATA_WIDTH> > rx_data;
        hls::stream<ipUdpMeta> tx_meta;
        hls::stream<net_axis<DATA_WIDTH> > tx_data;
        hls::stream<txMeta> sq_meta;
        hls::stream<ackMeta> rx_ack_meta;
        hls::stream<memCmd> mem_write_cmd;
        hls::stream<memCmd> mem_read_cmd;
        hls::stream<net_axis<DATA_WIDTH> > mem_write_data;
        hls::stream<net_axis<DATA_WIDTH> > mem_read_data;
        hls::stream<qpContext> qp_interface;
        hls::stream<ifConnReq> qp_connection_interface;
#ifdef DBG_IBV
        hls::stream<psnPkg> debug_0;
        hls::stream<psnPkg> debug_1;
        hls::stream<psnPkg> debug_2;
#endif
        ap_uint<32> invalid_psn_drop_count = 0;
        ap_uint<32> retransmit_count = 0;
        ap_uint<32> ibv_rx_count = 0;
        ap_uint<32> ibv_tx_count = 0;

        void tickOneCycle();
    };

    void MrcTransportProtocolTestBench::tickOneCycle()
    {
        // local memory for the requestor: every read command is answered right away with cmd.len zero bytes
        while (!mem_read_cmd.empty()) {
            const memCmd cmd = mem_read_cmd.read();
            for (uint32_t byteIndex = 0; byteIndex < cmd.len; byteIndex += DATA_WIDTH / 8) {
                net_axis<DATA_WIDTH> word;
                word.data = 0;
                word.keep = 0;
                for (uint32_t bit = 0; bit < std::min<uint32_t>(DATA_WIDTH / 8, cmd.len - byteIndex); ++bit) word.keep[bit] = 1;
                word.last = byteIndex + DATA_WIDTH / 8 >= cmd.len;
                mem_read_data.write(word);
            }
        }

        mrc_transport_protocol_top(
            rx_meta, rx_data,
            tx_meta, tx_data,
            sq_meta,
            rx_ack_meta,
            mem_write_cmd, mem_read_cmd,
            mem_write_data, mem_read_data,
            qp_interface, qp_connection_interface,
#ifdef DBG_IBV
            debug_0, debug_1, debug_2,
#endif
            invalid_psn_drop_count, retransmit_count, ibv_rx_count, ibv_tx_count
        );
#ifdef DBG_IBV
        psnPkg debug_packet;
        while (!debug_0.empty()) debug_0.read(debug_packet);
        while (!debug_1.empty()) debug_1.read(debug_packet);
        while (!debug_2.empty()) debug_2.read(debug_packet);
#endif
    }

    std::vector<uint8_t> buildWritePacket(const WriteTestCase& testCase)
    {
        std::vector<uint8_t> packet;

        // BTH
        appendNetworkOrder8(packet, testCase.opcode);
        appendNetworkOrder8(packet, 0); // pad and tver=0
        appendNetworkOrder16(packet, testCase.p_key);
        appendNetworkOrder8(packet, 0); // var-res
        appendNetworkOrder24(packet, testCase.dqp);
        appendNetworkOrder8(packet, (testCase.a << 7) | (testCase.R << 6) | (testCase.rtx << 5) | (testCase.ts << 4)); // a, R, rtx, ts
        appendNetworkOrder24(packet, testCase.psn);

        // METH
        appendNetworkOrder16(packet, testCase.rqmsn);    // RQMSN
        appendNetworkOrder16(packet, testCase.msn);

        // [TSETH]
        if (testCase.ts) {
            appendNetworkOrder16(packet, testCase.tx_timestamp);
            appendNetworkOrder16(packet, (testCase.tsr << 15) | (testCase.ftype & 0xF));
        }

        // RETH
        appendNetworkOrder64(packet, testCase.va);
        appendNetworkOrder32(packet, testCase.r_key);
        appendNetworkOrder32(packet, testCase.dmalen);

        // ImmDt
        if (testCase.opcode == MRC_RDMA_WRITE_LAST_IMM || testCase.opcode == MRC_RDMA_WRITE_ONLY_IMM)
            appendNetworkOrder32(packet, testCase.immDt);

        // payload
        packet.insert(packet.end(), testCase.payload.begin(), testCase.payload.end());
        return packet;
    }

    std::vector<uint8_t> buildAckPacket(const AckTestCase& testCase)
    {
        std::vector<uint8_t> packet;

        // BTH
        appendNetworkOrder8(packet, testCase.opcode);
        appendNetworkOrder8(packet, 0); // pad and tver=0
        appendNetworkOrder16(packet, testCase.p_key);
        appendNetworkOrder8(packet, 0); // var-res
        appendNetworkOrder24(packet, testCase.dqp);
        appendNetworkOrder8(packet, (testCase.a << 7) | (testCase.R << 6) | (testCase.rtx << 5) | (testCase.ts << 4)); // a, R, rtx, ts
        appendNetworkOrder24(packet, testCase.psn);

        // AETH
        appendNetworkOrder8(packet, testCase.syndrome);
        appendNetworkOrder24(packet, testCase.msn);
        return packet;
    }

    std::vector<uint8_t> buildSackPacket(const SackTestCase& testCase)
    {
        std::vector<uint8_t> packet;

        // BTH
        appendNetworkOrder8(packet, testCase.opcode);
        appendNetworkOrder8(packet, 0); // pad and tver=0
        appendNetworkOrder16(packet, testCase.p_key);
        appendNetworkOrder8(packet, 0); // var-res
        appendNetworkOrder24(packet, testCase.dqp);
        appendNetworkOrder8(packet, (testCase.a << 7) | (testCase.R << 6) | (testCase.rtx << 5) | (testCase.ts << 4)); // a, R, rtx, ts
        appendNetworkOrder24(packet, testCase.psn);

        // SETH
        appendNetworkOrder16(packet, ((testCase.m & 0x3) << 5) | ((testCase.pr & 0x1) << 1)); // type, nh, m, r, r, r, pr, r
        appendNetworkOrder16(packet, static_cast<uint16_t>(testCase.ack_psn_offset));
        appendNetworkOrder32(packet, testCase.entropy);
        appendNetworkOrder16(packet, testCase.spdcid);
        appendNetworkOrder16(packet, testCase.dpdcid);
        appendNetworkOrder8(packet, 0); // res
        appendNetworkOrder24(packet, testCase.cack_psn);
        appendNetworkOrder8(packet, (testCase.cc_type << 4) | (testCase.cc_fl & 0xF));
        appendNetworkOrder8(packet, testCase.mpr);
        appendNetworkOrder16(packet, static_cast<uint16_t>(testCase.sack_offset));
        appendNetworkOrder64(packet, testCase.sack_bitmap);

        // CC_STATE (cc_type 0)
        appendNetworkOrder16(packet, testCase.tx_timestamp);
        appendNetworkOrder16(packet, testCase.ooo_count & 0x7FFF); // r, ooo_count
        appendNetworkOrder8(packet, (testCase.r_c << 7) | (testCase.rcv_cwnd_pen & 0x7F));
        appendNetworkOrder24(packet, testCase.rcvd_bytes);
        return packet;
    }

    std::vector<uint8_t> buildNackPacket(const NackTestCase& testCase)
    {
        std::vector<uint8_t> packet;

        // BTH
        appendNetworkOrder8(packet, testCase.opcode);
        appendNetworkOrder8(packet, 0); // pad and tver=0
        appendNetworkOrder16(packet, testCase.p_key);
        appendNetworkOrder8(packet, 0); // var-res
        appendNetworkOrder24(packet, testCase.dqp);
        appendNetworkOrder8(packet, (testCase.a << 7) | (testCase.R << 6) | (testCase.rtx << 5) | (testCase.ts << 4)); // a, R, rtx, ts
        appendNetworkOrder24(packet, testCase.psn);

        // NETH
        appendNetworkOrder16(packet, 0); // type, nh, reserved
        appendNetworkOrder8(packet, testCase.nack_reason);
        appendNetworkOrder8(packet, testCase.vendor_info);
        appendNetworkOrder32(packet, testCase.entropy);
        appendNetworkOrder16(packet, testCase.spdcid);
        appendNetworkOrder16(packet, testCase.dpdcid);
        appendNetworkOrder8(packet, 0); // res
        appendNetworkOrder24(packet, testCase.nack_psn);
        appendNetworkOrder8(packet, (testCase.cc_type << 4) | (testCase.cc_fl & 0xF));
        appendNetworkOrder8(packet, 0); // reserved
        appendNetworkOrder16(packet, testCase.tx_timestamp);
        return packet;
    }

    std::vector<uint8_t> buildProbePacket(const ProbeTestCase& testCase)
    {
        std::vector<uint8_t> packet;

        // BTH
        appendNetworkOrder8(packet, testCase.opcode);
        appendNetworkOrder8(packet, 0); // pad and tver=0
        appendNetworkOrder16(packet, testCase.p_key);
        appendNetworkOrder8(packet, 0); // var-res
        appendNetworkOrder24(packet, testCase.dqp);
        appendNetworkOrder8(packet, (testCase.a << 7) | (testCase.R << 6) | (testCase.rtx << 5) | (testCase.ts << 4)); // a, R, rtx, ts
        appendNetworkOrder24(packet, testCase.psn);

        // PETH
        appendNetworkOrder24(packet, 0); // type, nh, reserved, reserved
        appendNetworkOrder8(packet, testCase.vendor_info);
        appendNetworkOrder16(packet, testCase.probe_id);
        appendNetworkOrder16(packet, 0); // reserved
        appendNetworkOrder16(packet, testCase.spdcid);
        appendNetworkOrder16(packet, testCase.dpdcid);
        appendNetworkOrder16(packet, testCase.tx_timestamp);
        appendNetworkOrder16(packet, (testCase.tsr << 15) | (testCase.ftype & 0xF));
        return packet;
    }

    std::vector<uint8_t> buildEndpointReqPacket(const EndpointReqTestCase& testCase)
    {
        std::vector<uint8_t> packet;

        // BTH
        appendNetworkOrder8(packet, testCase.opcode);
        appendNetworkOrder8(packet, 0); // pad and tver=0
        appendNetworkOrder16(packet, testCase.p_key);
        appendNetworkOrder8(packet, 0); // var-res
        appendNetworkOrder24(packet, testCase.dqp);
        appendNetworkOrder8(packet, (testCase.a << 7) | (testCase.R << 6) | (testCase.rtx << 5) | (testCase.ts << 4)); // a, R, rtx, ts
        appendNetworkOrder24(packet, testCase.psn);

        // ERTH
        appendNetworkOrder16(packet, testCase.op & 0x3); // type, nh, reserved, op
        appendNetworkOrder8(packet, 0); // reserved
        appendNetworkOrder8(packet, testCase.vendor_info);
        appendNetworkOrder32(packet, testCase.port_status_mask);
        appendNetworkOrder32(packet, 0); // reserved
        appendNetworkOrder16(packet, testCase.tx_timestamp);
        appendNetworkOrder16(packet, (testCase.tsr << 15) | (testCase.ftype & 0xF));
        return packet;
    }

    std::vector<uint8_t> buildEndpointResPacket(const EndpointResTestCase& testCase)
    {
        std::vector<uint8_t> packet;

        // BTH
        appendNetworkOrder8(packet, testCase.opcode);
        appendNetworkOrder8(packet, 0); // pad and tver=0
        appendNetworkOrder16(packet, testCase.p_key);
        appendNetworkOrder8(packet, 0); // var-res
        appendNetworkOrder24(packet, testCase.dqp);
        appendNetworkOrder8(packet, (testCase.a << 7) | (testCase.R << 6) | (testCase.rtx << 5) | (testCase.ts << 4)); // a, R, rtx, ts
        appendNetworkOrder24(packet, testCase.psn);

        // EETH
        appendNetworkOrder16(packet, (testCase.op & 0x3) << 2); // type, nh, r, op, r
        appendNetworkOrder16(packet, 0); // reserved
        for (int i = 0; i < 6; ++i) appendNetworkOrder32(packet, 0); // reserved
        appendNetworkOrder16(packet, testCase.tx_timestamp);
        appendNetworkOrder16(packet, 0); // reserved
        appendNetworkOrder32(packet, 0); // reserved
        return packet;
    }

    bool checkWriteProcessed(MrcTransportProtocolTestBench& tb, const WriteTestCase& tc)
    {
        bool returnValue = true;
        auto fail = [&](const std::string& msg) {
            std::cerr << "[FAIL " << tc.name << " psn=" << std::hex << tc.psn << "] " << msg << std::endl;            returnValue = false;
        };

        // populate memory commands: an accepted row owns exactly one, a rejected row owns none
        std::vector<memCmd> memoryCommands;
        const size_t maxCmds = tc.exp_accepted ? 1 : 0;
        while (!tb.mem_write_cmd.empty() && memoryCommands.size() < maxCmds)
            memoryCommands.push_back(tb.mem_write_cmd.read());

        // memory data: an accepted row owns the words up to and including the first last
        std::vector<uint8_t> memBytes;
        bool isLastData = false,
            memLastNotFinal = false;
        while (tc.exp_accepted && !tb.mem_write_data.empty()) {
            net_axis<DATA_WIDTH> word = tb.mem_write_data.read();

            if (isLastData) memLastNotFinal = true;      // data after last word, it should be empty!

            for (size_t bit = 0; bit < DATA_WIDTH / 8; ++bit)
                if (word.keep[bit]) memBytes.push_back(static_cast<uint8_t>(word.data(bit * 8 + 7, bit * 8).to_uint()));

            isLastData |= (word.last == 1);
            if (word.last == 1) break; // rest belongs to the next packet
        }

        // tx packets: a row expecting an ACK owns everything up to and including that ACK (e.g. SACKs before it)
        std::vector<std::vector<uint8_t>> txPkts;
        std::vector<uint8_t> cur;
        while (tc.exp_ack && !tb.tx_data.empty()) {
            net_axis<DATA_WIDTH> word = tb.tx_data.read();
            for (size_t bit = 0; bit < DATA_WIDTH / 8; ++bit)
                if (word.keep[bit]) cur.push_back(static_cast<uint8_t>(word.data(bit * 8 + 7, bit * 8).to_uint()));
            if (word.last) {
                const bool isAck = !cur.empty() && cur[0] == MRC_ACK;
                txPkts.push_back(cur);
                cur.clear();
                if (isAck) break;
            }
        }

        // keep tx_meta aligned with tx_data: one meta per packet consumed
        size_t txMetaCount = 0;
        while (!tb.tx_meta.empty() && txMetaCount < txPkts.size()) { tb.tx_meta.read(); ++txMetaCount; }

        if (tc.exp_accepted) {
            // memory command
            const bool isLast = tc.opcode == MRC_RDMA_WRITE_LAST     || tc.opcode == MRC_RDMA_WRITE_ONLY ||
                                tc.opcode == MRC_RDMA_WRITE_LAST_IMM || tc.opcode == MRC_RDMA_WRITE_ONLY_IMM;
            if (memoryCommands.size() != 1) {
                fail("expected exactly 1 memCmd, got " + std::to_string(memoryCommands.size()));
            } else {
                const memCmd& c = memoryCommands[0];
                if (c.op_code != tc.opcode)                    fail("memCmd opcode " + std::to_string((unsigned)c.op_code) + " != " + std::to_string(tc.opcode));
                if (c.qpn != (tc.dqp & 0xFFFF))                fail("memCmd qpn " + std::to_string(c.qpn.to_uint()) + " != " + std::to_string(tc.dqp & 0xFFFF));
                if (c.addr != (tc.va & 0xFFFFFFFFFFFFULL))     fail("memCmd addr 0x" + [&]{ std::ostringstream s; s << std::hex << c.addr.to_uint64() << " != 0x" << (tc.va & 0xFFFFFFFFFFFFULL); return s.str(); }());
                if (c.len != tc.payload.size())                fail("memCmd len " + std::to_string(c.len.to_uint()) + " != " + std::to_string(tc.payload.size()));
                if ((c.lst == 1) != isLast)                    fail(std::string("memCmd lst=") + std::to_string(c.lst.to_uint()) + " but opcode is " + (isLast ? "last" : "not last"));
            }

            // payload on the memory data stream
            if (memBytes != tc.payload) {
                fail("memory payload mismatch: got " + std::to_string(memBytes.size()) + " bytes, expected " + std::to_string(tc.payload.size()));
                for (size_t i = 0; i < std::min(memBytes.size(), tc.payload.size()); ++i)
                    if (memBytes[i] != tc.payload[i]) {
                        fail("first differing byte at offset " + std::to_string(i) + ": got " + std::to_string(memBytes[i]) + ", expected " + std::to_string(tc.payload[i]));
                        break;
                    }
            }
            if (!tc.payload.empty() && !isLastData) fail("mem_write_data never asserted last");
            if (memLastNotFinal)                    fail("mem_write_data has words after the last word");
        }

        // ACK on the network TX side (checked for accepted and rejected rows, e.g. re-ACKed duplicates)
        size_t acks = 0;
        for (size_t i = 0; i < txPkts.size(); ++i) {
            const std::vector<uint8_t>& p = txPkts[i];
            if (p.size() < 12) { fail("short tx packet"); continue; }
            if (p[0] == MRC_ACK) {
                ++acks;
                if (p.size() == 16) {
                    const uint32_t psn = (p[9] << 16) | (p[10] << 8) | p[11];
                    const uint32_t msn = (p[13] << 16) | (p[14] << 8) | p[15];
                    if (psn != tc.exp_cack_psn) fail("ACK psn " + std::to_string(psn) + " != exp_cack_psn " + std::to_string(tc.exp_cack_psn));

                    const uint8_t expSyndrome = tc.exp_syndrome ? tc.exp_syndrome : 0x1f;
                    if (p[12] != expSyndrome) fail("ACK syndrome 0x" + [&]{ std::ostringstream s; s << std::hex << (unsigned)p[12] << " != 0x" << (unsigned)expSyndrome; return s.str(); }());                    if (msn != tc.exp_ack_msn)  fail("ACK msn " + std::to_string(msn) + " != " + std::to_string(tc.exp_ack_msn));
                } else fail("ACK is not 16 bytes");
            } else if (p[0] != MRC_SACK) {
                fail("unexpected tx opcode " + std::to_string(p[0]));   // a SACK (0xDC) is allowed in MRC
            }
        }
        if (acks != (tc.exp_ack ? 1u : 0u)) fail("expected " + std::to_string(tc.exp_ack ? 1 : 0) + " ACK(s), got " + std::to_string(acks));

        return returnValue;
    }

    bool checkAckProcessed(MrcTransportProtocolTestBench& tb, const AckTestCase& tc) 
    {
        bool returnValue = true;
        auto fail = [&](const std::string& msg) {
            std::cerr << "[FAIL " << tc.name << " psn=" << std::hex << tc.psn << "] " << msg << std::endl;
            returnValue = false;
        };

        // the row owns exp_completions completions, in posted order, the last one for BTH.psn (= cack_psn).
        // Every posted write carries psn[3:0] in laddr[51:48], which comes back as ackMeta.dst
        uint32_t completions = 0;
        while (!tb.rx_ack_meta.empty() && completions < tc.exp_completions) {
            const ackMeta m = tb.rx_ack_meta.read();
            const uint32_t expPsn = (tc.psn - tc.exp_completions + 1 + completions) & 0xFFFFFF;
            ++completions;
            if (m.op_code != MRC_ACK)       fail("completion opcode " + std::to_string((unsigned)m.op_code) + " != MRC_ACK");
            if (m.qpn != (tc.dqp & 0xFFFF)) fail("completion qpn " + std::to_string(m.qpn.to_uint()) + " != " + std::to_string(tc.dqp & 0xFFFF));
            if (m.dst != (expPsn & 0xF))    fail("completion of psn[3:0] " + std::to_string(m.dst.to_uint()) + ", expected psn " + std::to_string(expPsn));
            if (m.lst != 1)                 fail("completion lst is 0, every posted write has lst = 1");
        }
        if (completions != tc.exp_completions) fail("expected " + std::to_string(tc.exp_completions) + " completion(s), got " + std::to_string(completions));

        return returnValue;
    }

    bool checkSackProcessed(MrcTransportProtocolTestBench& tb, const SackTestCase& tc) 
    {
        // a SACK never produces output (no completion, no packet, no memory access): it only moves unack_psn,
        // which the NACK rows after it observe. Anything it emits by mistake is reported as leftover at the end of the group
        return true;
    }

    bool checkNackProcessed(MrcTransportProtocolTestBench& tb, const NackTestCase& tc) 
    {
        bool returnValue = true;
        auto fail = [&](const std::string& msg) {
            std::cerr << "[FAIL " << tc.name << " psn=" << std::hex << tc.nack_psn << "] " << msg << std::endl;
            returnValue = false;
        };

        // the row owns exactly one tx packet if it expects a retransmission, nothing otherwise
        if (!tc.exp_retransmit) return true;

        std::vector<uint8_t> p;
        while (!tb.tx_data.empty()) {
            net_axis<DATA_WIDTH> word = tb.tx_data.read();
            for (size_t bit = 0; bit < DATA_WIDTH / 8; ++bit)
                if (word.keep[bit]) p.push_back(static_cast<uint8_t>(word.data(bit * 8 + 7, bit * 8).to_uint()));
            if (word.last) break;
        }
        if (!tb.tx_meta.empty()) tb.tx_meta.read();

        if (p.size() < 12) {
            fail("expected the retransmission of the NACKed psn, got nothing");
            return returnValue;
        }
        const uint32_t dqp = (p[5] << 16) | (p[6] << 8) | p[7];
        const uint32_t psn = (p[9] << 16) | (p[10] << 8) | p[11];
        if (p[0] != MRC_RDMA_WRITE_ONLY)            fail("retransmission opcode " + std::to_string(p[0]) + " != MRC_RDMA_WRITE_ONLY");
        if (dqp != REMOTE_QPN + tc.dqp - RSP_QP_0)  fail("retransmission dqp " + std::to_string(dqp) + " != " + std::to_string(REMOTE_QPN + tc.dqp - RSP_QP_0));
        if (psn != tc.nack_psn)                     fail("retransmission psn " + std::to_string(psn) + " != nack_psn " + std::to_string(tc.nack_psn));
        //if ((p[8] >> 5 & 1) == 0)                   fail("retransmission rtx is not set");
        
        return returnValue;
    }

    bool checkProbeProcessed(MrcTransportProtocolTestBench& tb, const ProbeTestCase& tc) 
    {
        // CURRENT BEHAVIOUR: parsed and logged only (the SACK response with pr = 1 is a TODO), so it owns no output
        return true;
    }

    bool checkEndpointReqProcessed(MrcTransportProtocolTestBench& tb, const EndpointReqTestCase& tc) 
    {
        // CURRENT BEHAVIOUR: parsed and logged only (the EETH response to an EV probe is a TODO), so it owns no output
        return true;
    }

    bool checkEndpointResProcessed(MrcTransportProtocolTestBench& tb, const EndpointResTestCase& tc) 
    {
        // CURRENT BEHAVIOUR: parsed and logged only (latency measurement is a TODO), so it owns no output
        return true;
    }

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
        WRITE_TC({"MRC WRITE ONLY 01a", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+140, 0, 0, 0,0,0, 0x3000, RKEY, 64, 0x00000000, buildWritePayload(64,0xEE), false, false, 0, 0, 0x00, BACK_TO_BACK}),
        
        // in-order ONLY, same va, distinct fills 
        WRITE_TC({"MRC WRITE ONLY 02a", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+0, 0, 0, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x01), true, true, FIRST_RX_PSN+0, 1, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02b", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+1, 0, 1, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x02), true, true, FIRST_RX_PSN+1, 2, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02c", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+2, 0, 2, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x03), true, true, FIRST_RX_PSN+2, 3, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02d", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+3, 0, 3, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x04), true, true, FIRST_RX_PSN+3, 4, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02e", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+4, 0, 4, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x05), true, true, FIRST_RX_PSN+4, 5, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02f", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+5, 0, 5, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x06), true, true, FIRST_RX_PSN+5, 6, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02g", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+6, 0, 6, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x07), true, true, FIRST_RX_PSN+6, 7, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02h", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+7, 0, 7, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x08), true, true, FIRST_RX_PSN+7, 8, 0x00, BACK_TO_BACK}),

        // exp_accepted, then its duplicate (not rewritten, re-ACKed)
        WRITE_TC({"MRC WRITE ONLY 03a", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+8, 0, 8, 0,0,0, 0x4000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x09), true,  true, FIRST_RX_PSN+8, 9, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 03b", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+8, 0, 8, 0,0,0, 0x4000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x09), false, true, FIRST_RX_PSN+8, 9, 0x00, BACK_TO_BACK}),

        // older PSNs, not rewritten, re-ACKed with cack, not their own PSN
        WRITE_TC({"MRC WRITE ONLY 04a", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+4,  0, 0, 0,0,0, 0x5000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, true, FIRST_RX_PSN+8,  9, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 04b", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN-21, 0, 0, 0,0,0, 0x5000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, true, FIRST_RX_PSN+8, 9, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 04c", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+8,  0, 0, 0,0,0, 0x5000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, true, FIRST_RX_PSN+8,  9, 0x00, BACK_TO_BACK}),

        // multi-packet, RETH on every packet 
        WRITE_TC({"MRC WRITE ONLY 05a", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+9,  0, 9, 0,0,0, 0x10000,         RKEY, 2*PMTU+100, 0x00000000, buildWritePayload(PMTU,0xA1), true, true, FIRST_RX_PSN+9,  9 , 0x00, BACK_TO_BACK}),
        PROBE_TC({"MRC PROBE 05a+", MRC_PROBE_REQ, 0xFFFF, RSP_QP_0, 0,0,0,0, 0, 0x5A, 0x3001, RSP_QP_0, REMOTE_QPN, 0x1234, 0, MRC_FTYPE_TS, BACK_TO_BACK}),
        ENDPOINT_REQ_TC({"MRC EP REQ 05b+", MRC_EP_REQ, 0xFFFF, MRC_EV_PROBE_DQP, 0,0,0,0, 0x3002, EP_OP_PORT_STATUS, 0x5A, 0xA5A5A5A5, 0, 0, 0, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 05b", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+10, 0, 9, 0,0,0, 0x10000+PMTU,     RKEY, 2*PMTU+100, 0x00000000, buildWritePayload(PMTU,0xA2), true, true, FIRST_RX_PSN+10, 9 , 0x00, BACK_TO_BACK}),
        PROBE_TC({"MRC PROBE 05a+", MRC_PROBE_REQ, 0xFFFF, RSP_QP_0, 0,0,0,0, 0, 0x5A, 0x3001, RSP_QP_0, REMOTE_QPN, 0x1234, 0, MRC_FTYPE_TS, BACK_TO_BACK}),
        ENDPOINT_REQ_TC({"MRC EP REQ 05b+", MRC_EP_REQ, 0xFFFF, MRC_EV_PROBE_DQP, 0,0,0,0, 0x3002, EP_OP_PORT_STATUS, 0x5A, 0xA5A5A5A5, 0, 0, 0, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 05c", MRC_RDMA_WRITE_LAST,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+11, 0, 9, 0,0,0, 0x10000+2*PMTU,   RKEY, 2*PMTU+100, 0x00000000, buildWritePayload(100,0xA3), true, true, FIRST_RX_PSN+11, 10, 0x00, BACK_TO_BACK}),

        // TSETH on/off 
        WRITE_TC({"MRC WRITE ONLY 06a", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN+12, 0, 10, 0x1234,0,1, 0x20000,       RKEY, 700,       0x00000000, buildWritePayload(700,0xB0), true, true, FIRST_RX_PSN+12, 11, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 06b", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN+13, 0, 11, 0x1234,0,1, 0x21000,       RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(PMTU,0xB1), true, true, FIRST_RX_PSN+13, 11, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 06c", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN+14, 0, 11, 0x1234,0,1, 0x21000+PMTU,   RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(PMTU,0xB2), true, true, FIRST_RX_PSN+14, 11, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 06d", MRC_RDMA_WRITE_LAST,   0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN+15, 0, 11, 0x1234,0,1, 0x21000+2*PMTU, RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(50,0xB3),  true, true, FIRST_RX_PSN+15, 12, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 06e", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+16, 0, 12, 0,0,0,      0x24000,       RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(PMTU,0xC1), true, true, FIRST_RX_PSN+16, 12, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 06f", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN+17, 0, 12, 0x1234,0,1, 0x24000+PMTU,   RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(PMTU,0xC2), true, true, FIRST_RX_PSN+17, 12, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 06g", MRC_RDMA_WRITE_LAST,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+18, 0, 12, 0,0,0,      0x24000+2*PMTU, RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(50,0xC3),  true, true, FIRST_RX_PSN+18, 13, 0x00, BACK_TO_BACK}),

        // rtx bit 
        WRITE_TC({"MRC WRITE ONLY 07a", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,1,0, FIRST_RX_PSN+19, 0, 13, 0,0,0, 0x30000, RKEY, 200, 0x00000000, buildWritePayload(200,0xD0), true, true, FIRST_RX_PSN+19, 14, 0x00, BACK_TO_BACK}),

        // sizes around the 64-byte beat boundary 
        WRITE_TC({"MRC WRITE ONLY 08a", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+20, 0, 14, 0,0,0,      0x40000, RKEY, 1,  0x00000000, buildWritePayload(1, 0x10), true, true, FIRST_RX_PSN+20, 15, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08b", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+21, 0, 15, 0,0,0,      0x40100, RKEY, 44, 0x00000000, buildWritePayload(44,0x20), true, true, FIRST_RX_PSN+21, 16, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08c", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+22, 0, 16, 0,0,0,      0x40200, RKEY, 45, 0x00000000, buildWritePayload(45,0x30), true, true, FIRST_RX_PSN+22, 17, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08d", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+23, 0, 17, 0,0,0,      0x40300, RKEY, 48, 0x00000000, buildWritePayload(48,0x40), true, true, FIRST_RX_PSN+23, 18, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08e", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+24, 0, 18, 0,0,0,      0x40400, RKEY, 49, 0x00000000, buildWritePayload(49,0x50), true, true, FIRST_RX_PSN+24, 19, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08f", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN+25, 0, 19, 0x1234,0,1, 0x40500, RKEY, 40, 0x00000000, buildWritePayload(40,0x60), true, true, FIRST_RX_PSN+25, 20, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08g", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN+26, 0, 20, 0x1234,0,1, 0x40600, RKEY, 48, 0x00000000, buildWritePayload(48,0x70), true, true, FIRST_RX_PSN+26, 21, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08h", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN+27, 0, 21, 0x1234,0,1, 0x40700, RKEY, 49, 0x00000000, buildWritePayload(49,0x80), true, true, FIRST_RX_PSN+27, 22, 0x00, BACK_TO_BACK}),

        // ONLY todo: add control packets in the middle
        WRITE_TC({"MRC WRITE ONLY 09a", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+28, 0, 22, 0,0,0, 0x50000, RKEY, 256, 0x00000000, buildWritePayload(256,0x90), true, true, FIRST_RX_PSN+28, 23, 0x00, BACK_TO_BACK}),
        ACK_TC ({"MRC ACK 09b",  MRC_ACK,  0xFFFF, RSP_QP_0, 0,0,0,0, 0x0000FF, 0x1F, 0, 0, BACK_TO_BACK}),
        SACK_TC({"MRC SACK 09c", MRC_SACK, 0xFFFF, RSP_QP_0, 0,0,0,0, 0x0000FF, SACK_M_NONE, 0, 0, 0, RSP_QP_0, REMOTE_QPN, 0x0000FF, MRC_CC_TYPE_DEFAULT, 0, 0, 0, 0, 0, 0, 0, 0, 0, BACK_TO_BACK}),
        NACK_TC({"MRC NACK 09d", MRC_NACK, 0xFFFF, RSP_QP_0, 0,0,0,0, 0x000100, NACK_TRIMMED, 0, 0, RSP_QP_0, REMOTE_QPN, 0x000100, 0x2, 0, 0x0000, false, BACK_TO_BACK}),
        PROBE_TC({"MRC PROBE 09e", MRC_PROBE_REQ, 0xFFFF, RSP_QP_0, 0,0,0,0, 0, 0, 0x3003, RSP_QP_0, REMOTE_QPN, 0, 0, 0, BACK_TO_BACK}),
        ENDPOINT_REQ_TC({"MRC EP REQ 09f", MRC_EP_REQ, 0xFFFF, MRC_EV_PROBE_DQP, 0,0,0,0, 0x3004, EP_OP_EV_PROBE, 0, 0, 0x2222, 0, MRC_FTYPE_TS, BACK_TO_BACK}),
        ENDPOINT_RES_TC({"MRC EP RES 09g", MRC_EP_RSP, 0xFFFF, MRC_EV_PROBE_DQP, 0,0,0,0, 0x3004, EP_OP_EV_PROBE, 0x3333, BACK_TO_BACK}),

        // Write with Immediate 
        WRITE_TC({"MRC WRITE ONLY 10a", MRC_RDMA_WRITE_FIRST,     0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+29, 0, 23, 0,0,0, 0x51000,     RKEY, PMTU+100, 0x00000000, buildWritePayload(PMTU,0xE1), true, true, FIRST_RX_PSN+29, 23, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 10b", MRC_RDMA_WRITE_LAST_IMM,  0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+30, 0, 23, 0,0,0, 0x51000+PMTU, RKEY, PMTU+100, 0x00000000, buildWritePayload(100,0xE2), true, true, FIRST_RX_PSN+30, 24, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 10c", MRC_RDMA_WRITE_ONLY_IMM,  0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+31, 0, 24, 0,0,0, 0x52000,     RKEY, 300,     0x00000000, buildWritePayload(300,0xE3), true, true, FIRST_RX_PSN+31, 25, 0x00, BACK_TO_BACK}),

        // PSN ahead (repo: NAK once, then silent), in order resets it 
        WRITE_TC({"MRC WRITE ONLY 11a", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+182, 0, 0,  0,0,0, 0x2000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, false, 0, 0, 0x00, BACK_TO_BACK}), 
        WRITE_TC({"MRC WRITE ONLY 11b", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+182, 0, 0,  0,0,0, 0x2000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, false, 0, 0, 0x00, BACK_TO_BACK}), 
        WRITE_TC({"MRC WRITE ONLY 11c", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+182, 0, 0,  0,0,0, 0x2000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, false, 0, 0, 0x00, BACK_TO_BACK}), 
        WRITE_TC({"MRC WRITE ONLY 11d", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+32,  0, 25, 0,0,0, 0x2000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0x11), true,  true,  FIRST_RX_PSN+32, 26, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 11e", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+183, 0, 0,  0,0,0, 0x2000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, false, 0, 0, 0x00, BACK_TO_BACK}), 
        WRITE_TC({"MRC WRITE ONLY 11f", MRC_RDMA_WRITE_ONLY, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+183, 0, 0,  0,0,0, 0x2000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, false, 0, 0, 0x00, BACK_TO_BACK}),

        // burst
        WRITE_TC({"MRC WRITE ONLY 12a", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+33, 0, 26, 0,0,0,      0x60000,       RKEY, 500,       0x00000000, buildWritePayload(500,0xF1), true, true, FIRST_RX_PSN+33, 27, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 12b", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN+34, 0, 27, 0x1234,0,1, 0x61000,       RKEY, 45,        0x00000000, buildWritePayload(45,0xF2),  true, true, FIRST_RX_PSN+34, 28, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 12c", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+35, 0, 28, 0,0,0,      0x62000,       RKEY, 2*PMTU+10,  0x00000000, buildWritePayload(PMTU,0xF3), true, true, FIRST_RX_PSN+35, 28, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 12d", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+36, 0, 28, 0,0,0,      0x62000+PMTU,   RKEY, 2*PMTU+10,  0x00000000, buildWritePayload(PMTU,0xF4), true, true, FIRST_RX_PSN+36, 28, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 12e", MRC_RDMA_WRITE_LAST,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+37, 0, 28, 0,0,0,      0x62000+2*PMTU, RKEY, 2*PMTU+10,  0x00000000, buildWritePayload(10,0xF5),  true, true, FIRST_RX_PSN+37, 29, 0x00, BACK_TO_BACK}),
        
        // same as before but waiting random amount of cycles in between
        WRITE_TC({"MRC WRITE ONLY 13a", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+38, 0, 29, 0,0,0,      0x60000,        RKEY, 500,       0x00000000, buildWritePayload(500,0xF1), true, true, FIRST_RX_PSN+38, 30, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 13b", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,1, FIRST_RX_PSN+39, 0, 30, 0x1234,0,1, 0x61000,        RKEY, 45,        0x00000000, buildWritePayload(45,0xF2),  true, true, FIRST_RX_PSN+39, 31, 0x00, DRAIN}),
        WRITE_TC({"MRC WRITE ONLY 13c", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+40, 0, 31, 0,0,0,      0x62000,        RKEY, 2*PMTU+10,  0x00000000, buildWritePayload(PMTU,0xF3), true, true, FIRST_RX_PSN+40, 31, 0x00, 2}),
        WRITE_TC({"MRC WRITE ONLY 13d", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+41, 0, 31, 0,0,0,      0x62000+PMTU,   RKEY, 2*PMTU+10,  0x00000000, buildWritePayload(PMTU,0xF4), true, true, FIRST_RX_PSN+41, 31, 0x00, 200}),
        WRITE_TC({"MRC WRITE ONLY 13e", MRC_RDMA_WRITE_LAST,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+42, 0, 31, 0,0,0,      0x62000+2*PMTU, RKEY, 2*PMTU+10,  0x00000000, buildWritePayload(10,0xF5),  true, true, FIRST_RX_PSN+42, 32, 0x00, 1000}),

        // BTH.a = 0 -> written, MSN still advances, but no ACK. Each a=0 row is followed by an a=1 row
        // in the same group, so a stray ACK would be caught by the next ACK check
        WRITE_TC({"MRC WRITE ONLY 14a", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN+43, 0, 32, 0,0,0, 0x80000,        RKEY, 128,       0x00000000, buildWritePayload(128,0x14),  true, false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 14b", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+44, 0, 33, 0,0,0, 0x80100,        RKEY, 128,       0x00000000, buildWritePayload(128,0x15),  true, true,  FIRST_RX_PSN+44, 34, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 14c", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN+45, 0, 34, 0,0,0, 0x81000,        RKEY, 2*PMTU+30, 0x00000000, buildWritePayload(PMTU,0x16), true, false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 14d", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN+46, 0, 34, 0,0,0, 0x81000+PMTU,   RKEY, 2*PMTU+30, 0x00000000, buildWritePayload(PMTU,0x17), true, false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 14e", MRC_RDMA_WRITE_LAST,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+47, 0, 34, 0,0,0, 0x81000+2*PMTU, RKEY, 2*PMTU+30, 0x00000000, buildWritePayload(30,0x18),   true, true,  FIRST_RX_PSN+47, 35, 0x00, DRAIN}),

        // two multi-packet messages out of order: A = +48..+51, B = +52..+54. Packets beyond the hole are written
        // but not ACKed, a duplicate inside the window is ACKed with the current cack_psn. 15h closes the hole:
        // cack_psn jumps to +54 and the pending ACK requests of 15c and 15e produce one ACK, MSN +2, although 15h has a = 0
        WRITE_TC({"MRC WRITE ONLY 15a", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN+53, 0, 36, 0,0,0, 0x86000+PMTU,   RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(PMTU,0x40), true,  false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 15b", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN+50, 0, 35, 0,0,0, 0x82000+2*PMTU, RKEY, 3*PMTU+100, 0x00000000, buildWritePayload(PMTU,0x41), true,  false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 15c", MRC_RDMA_WRITE_LAST,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+54, 0, 36, 0,0,0, 0x86000+2*PMTU, RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(50,0x42),   true,  false, 0,               0,  0x00, BACK_TO_BACK}),
        // for ackmap
        //WRITE_TC({"MRC WRITE ONLY 15d", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN+48, 0, 35, 0,0,0, 0x82000,        RKEY, 3*PMTU+100, 0x00000000, buildWritePayload(PMTU,0x43), true,  false, 0,               0,  0x00, BACK_TO_BACK}),
        // for prevAr
        WRITE_TC({"MRC WRITE ONLY 15d", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN+48, 0, 35, 0,0,0, 0x82000,        RKEY, 3*PMTU+100, 0x00000000, buildWritePayload(PMTU,0x43), true,  true,  FIRST_RX_PSN+48, 35, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 15e", MRC_RDMA_WRITE_LAST,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+51, 0, 35, 0,0,0, 0x82000+3*PMTU, RKEY, 3*PMTU+100, 0x00000000, buildWritePayload(100,0x44),  true,  false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 15f", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN+52, 0, 36, 0,0,0, 0x86000,        RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(PMTU,0x45), true,  false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 15g", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+53, 0, 36, 0,0,0, 0x86000+PMTU,   RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(PMTU,0xEE), false, true,  FIRST_RX_PSN+48, 35, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 15h", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN+49, 0, 35, 0,0,0, 0x82000+PMTU,   RKEY, 3*PMTU+100, 0x00000000, buildWritePayload(PMTU,0x46), true,  true,  FIRST_RX_PSN+54, 37, 0x00, DRAIN}),

        // window edge (epsn = +55): epsn+128 is dropped silently, epsn+127 is written but not ACKed (hole below it),
        // epsn+128 is still dropped because accepting a packet beyond a hole does not move the window
        WRITE_TC({"MRC WRITE ONLY 16a", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+183, 0, 37, 0,0,0, 0x83000,        RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 16b", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+182, 0, 37, 0,0,0, 0x83100,        RKEY, 64,        0x00000000, buildWritePayload(64,0x1B),   true,  false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 16c", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+183, 0, 38, 0,0,0, 0x83200,        RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, false, 0,               0,  0x00, DRAIN}),

        // out-of-window packets are dropped silently whatever BTH.a; the duplicate and in-order packets around them are unaffected
        WRITE_TC({"MRC WRITE ONLY 17a", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+300, 0, 39, 0,0,0, 0x84000,       RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 17b", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+54,  0, 38, 0,0,0, 0x84000,       RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, true,  FIRST_RX_PSN+54, 37, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 17c", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+300, 0, 39, 0,0,0, 0x84000,       RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 17d", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+55,  0, 39, 0,0,0, 0x84100,       RKEY, 64,        0x00000000, buildWritePayload(64,0x1D),   true,  true,  FIRST_RX_PSN+55, 38, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 17e", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN+301, 0, 40, 0,0,0, 0x84000,       RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 17f", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+56,  0, 40, 0,0,0, 0x84200,       RKEY, 64,        0x00000000, buildWritePayload(64,0x1E),   true,  true,  FIRST_RX_PSN+56, 39, 0x00, DRAIN}),

        // duplicates are ACKed with the current cack_psn whatever BTH.a and opcode, and never rewritten
        WRITE_TC({"MRC WRITE ONLY 18a", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 0,0,0,0, FIRST_RX_PSN+56, 0, 40, 0,0,0, 0x84200,        RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, true, FIRST_RX_PSN+56, 39, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 18b", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+46, 0, 34, 0,0,0, 0x81000+PMTU,   RKEY, 2*PMTU+30, 0x00000000, buildWritePayload(PMTU,0xEE), false, true, FIRST_RX_PSN+56, 39, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 18c", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_0, 1,0,0,0, FIRST_RX_PSN+57, 0, 41, 0,0,0, 0x85000,        RKEY, 64,        0x00000000, buildWritePayload(64,0x1F),   true,  true, FIRST_RX_PSN+57, 40, 0x00, DRAIN}),

        // QP 0x06 (epsn 0xFFFFFE): multi-packet write in order across the 24-bit wrap, then a duplicate behind the wrap
        WRITE_TC({"MRC WRITE ONLY 19a", MRC_RDMA_WRITE_FIRST,  0xFFFF, RSP_QP_1, 1,0,0,0, 0xFFFFFE, 0, 0, 0,0,0, 0x70000,        RKEY, 2*PMTU+20, 0x00000000, buildWritePayload(PMTU,0x21), true,  true, 0xFFFFFE, 0, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 19b", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, RSP_QP_1, 1,0,0,0, 0xFFFFFF, 0, 0, 0,0,0, 0x70000+PMTU,   RKEY, 2*PMTU+20, 0x00000000, buildWritePayload(PMTU,0x22), true,  true, 0xFFFFFF, 0, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 19c", MRC_RDMA_WRITE_LAST,   0xFFFF, RSP_QP_1, 1,0,0,0, 0x000000, 0, 0, 0,0,0, 0x70000+2*PMTU, RKEY, 2*PMTU+20, 0x00000000, buildWritePayload(20,0x23),   true,  true, 0x000000, 1, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 19d", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_1, 1,0,0,0, 0xFFFFFF, 0, 0, 0,0,0, 0x71000,        RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, true, 0x000000, 1, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 19e", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_1, 1,0,0,0, 0x000001, 0, 1, 0,0,0, 0x71100,        RKEY, 64,        0x00000000, buildWritePayload(64,0x24),   true,  true, 0x000001, 2, 0x00, DRAIN}),

        // QP 0x07 (epsn 0xFFFFFB): ahead by 7 ACROSS the wrap. Expected = same behaviour as 15a (accepted).
        // QP 0x0A (epsn 0xFFFFFB): ahead by 7 ACROSS the wrap: written, ACK request pending until 0xFFFFFB..0x000001 arrive
        WRITE_TC({"MRC WRITE ONLY 20a", MRC_RDMA_WRITE_ONLY,   0xFFFF, RSP_QP_2, 1,0,0,0, 0x000002, 0, 0, 0,0,0, 0x78000,        RKEY, 64,        0x00000000, buildWritePayload(64,0x25),   true,  false, 0, 0, 0x00, DRAIN}),
                
        // RESPONDER SIDE
    };
} // namespace

int runMrcTransportProtocolTest()
{
    MrcTransportProtocolTestBench tb;

    tb.qp_interface.write(qpContext(READY_RECV, RSP_QP_0, 0x100, FIRST_RX_PSN, RKEY, 0x70000));
    tb.qp_connection_interface.write(ifConnReq(RSP_QP_0, REMOTE_QPN, REMOTE_IP, REMOTE_UDP_PORT));

    tb.qp_interface.write(qpContext(READY_RECV, RSP_QP_1, 0x200, 0xFFFFFE, RKEY, 0x80000));   // wrap, in order
    tb.qp_connection_interface.write(ifConnReq(RSP_QP_1, REMOTE_QPN + 1, REMOTE_IP, REMOTE_UDP_PORT));

    tb.qp_interface.write(qpContext(READY_RECV, RSP_QP_2, 0x300, 0xFFFFFB, RKEY, 0x90000));   // wrap, ahead in window
    tb.qp_connection_interface.write(ifConnReq(RSP_QP_2, REMOTE_QPN + 2, REMOTE_IP, REMOTE_UDP_PORT));
    
    for (uint16_t qpn = REQ_QP_0; qpn <= REQ_QP_4; ++qpn) { // remote_psn = first PSN this QP sends
        tb.qp_interface.write(qpContext(READY_RECV, qpn, qpn == REQ_QP_4 ? REQ_PSN_F : REQ_PSN, 0, RKEY, 0));
        tb.qp_connection_interface.write(ifConnReq(qpn, REMOTE_QPN + qpn - RSP_QP_0, REMOTE_IP, REMOTE_UDP_PORT));
    }

    for (int i = 0; i < 60; ++i) tb.tickOneCycle();

    // the requestor QPs transmit their writes; the first transmissions are not part of any test case
    // host = 1 and laddr[51:48] = psn[3:0]: the completion of each write comes back with ackMeta.dst = psn[3:0]
    for (uint16_t qpn = REQ_QP_0; qpn <= REQ_QP_4; ++qpn)
        for (uint32_t i = 0; i < REQ_WRITES; ++i) {
            const uint64_t psnTag = ((qpn == REQ_QP_4 ? REQ_PSN_F : REQ_PSN) + i) & 0xF;
            // send 6 writes, do not receive them, SEND them
            tb.sq_meta.write(txMeta(MRC_RDMA_WRITE_ONLY, qpn, 1, 1, 0, psnTag << 48, 0xA00000, 64, 0));
        }
    for (int i = 0; i < 300; ++i) tb.tickOneCycle();
    while (!tb.tx_data.empty()) tb.tx_data.read();
    while (!tb.tx_meta.empty()) tb.tx_meta.read();

    std::vector<size_t> group; // rows fed but not checked yet
    int failures = 0;
    const size_t testCaseCount = sizeof(testCases) / sizeof(testCases[0]);
    for (size_t tcIndex = 0; tcIndex < testCaseCount; ++tcIndex) {
        const TestCase& tc = testCases[tcIndex];
        const char* name = nullptr;
        int32_t afterCycles = 0;
        std::vector<uint8_t> packet;
        switch (tc.kind) {
            case TestCaseKind::WRITE:        name = tc.write.name;        afterCycles = tc.write.after_cycles;        packet = buildWritePacket(tc.write);              break;
            case TestCaseKind::ACK:          name = tc.ack.name;          afterCycles = tc.ack.after_cycles;          packet = buildAckPacket(tc.ack);                  break;
            case TestCaseKind::SACK:         name = tc.sack.name;         afterCycles = tc.sack.after_cycles;         packet = buildSackPacket(tc.sack);                break;
            case TestCaseKind::NACK:         name = tc.nack.name;         afterCycles = tc.nack.after_cycles;         packet = buildNackPacket(tc.nack);                break;
            case TestCaseKind::PROBE:        name = tc.probe.name;        afterCycles = tc.probe.after_cycles;        packet = buildProbePacket(tc.probe);              break;
            case TestCaseKind::ENDPOINT_REQ: name = tc.endpoint_req.name; afterCycles = tc.endpoint_req.after_cycles; packet = buildEndpointReqPacket(tc.endpoint_req); break;
            case TestCaseKind::ENDPOINT_RES: name = tc.endpoint_res.name; afterCycles = tc.endpoint_res.after_cycles; packet = buildEndpointResPacket(tc.endpoint_res); break;
        }
        std::cout << "\n" << name << "\n" << std::endl;

        const uint16_t udp_length = static_cast<uint16_t>(8 + packet.size() + 4);
        tb.rx_meta.write(ipUdpMeta(REMOTE_IP, REMOTE_UDP_PORT, RDMA_DEFAULT_PORT, udp_length));

        const size_t bytesPerWord = DATA_WIDTH / 8;
        for (size_t byteIndex = 0; byteIndex < packet.size(); byteIndex += bytesPerWord) {
            net_axis<DATA_WIDTH> wordToSend;
            wordToSend.data = 0;
            wordToSend.keep = 0;

            const size_t howManyBits = std::min(bytesPerWord, packet.size() - byteIndex);
            for (size_t bit = 0; bit < howManyBits; bit++) {
                wordToSend.data(bit * 8 + 7, bit * 8) = packet[byteIndex + bit];
                wordToSend.keep[bit] = 1;
            }
            wordToSend.last = byteIndex + bytesPerWord >= packet.size();

            tb.rx_data.write(wordToSend);
        }

        group.push_back(tcIndex);
        for (int c = 0; c < afterCycles; ++c) tb.tickOneCycle(); // idle gap only, no check
        if (afterCycles == DRAIN || tcIndex + 1 == testCaseCount) { // drain or last test case
            for (int c = 0; c < VERY_BIG_NUMBER; ++c) tb.tickOneCycle(); // drain
            std::cout << "checking group of " << std::dec << group.size() << " packet(s)" << std::endl;
            for (size_t k : group) {
                const TestCase& c = testCases[k];
                switch (c.kind) {
                    case TestCaseKind::WRITE:        failures += !checkWriteProcessed(tb, c.write);              break;
                    case TestCaseKind::ACK:          failures += !checkAckProcessed(tb, c.ack);                  break;
                    case TestCaseKind::SACK:         failures += !checkSackProcessed(tb, c.sack);                break;
                    case TestCaseKind::NACK:         failures += !checkNackProcessed(tb, c.nack);                break;
                    case TestCaseKind::PROBE:        failures += !checkProbeProcessed(tb, c.probe);              break;
                    case TestCaseKind::ENDPOINT_REQ: failures += !checkEndpointReqProcessed(tb, c.endpoint_req); break;
                    case TestCaseKind::ENDPOINT_RES: failures += !checkEndpointResProcessed(tb, c.endpoint_res); break;
                }
            }

            if (!tb.mem_write_cmd.empty() || !tb.mem_write_data.empty()) {
                std::cerr << "[FAIL] leftover memory output at end of group" << std::endl;
                ++failures;
            }
            if (!tb.rx_ack_meta.empty()) {
                std::cerr << "[FAIL] " << tb.rx_ack_meta.size() << " leftover completion(s) at end of group" << std::endl;
                ++failures;
            }
            while (!tb.mem_write_cmd.empty())  tb.mem_write_cmd.read();
            while (!tb.mem_write_data.empty()) tb.mem_write_data.read();
            while (!tb.rx_ack_meta.empty())    tb.rx_ack_meta.read();
            bool packetStart = true;
            while (!tb.tx_data.empty()) {   // unclaimed NAKs/SACKs are fine, anything else (e.g. a retransmission) is not
                const net_axis<DATA_WIDTH> word = tb.tx_data.read();
                const unsigned opcode = word.data(7, 0).to_uint();
                if (packetStart && opcode != MRC_ACK && opcode != MRC_SACK) {
                    std::cerr << "[FAIL] leftover tx packet at end of group, opcode 0x" << std::hex << opcode << std::dec << std::endl;
                    ++failures;
                }
                packetStart = word.last;
            }
            while (!tb.tx_meta.empty())        tb.tx_meta.read();
            
            group.clear();
        }
    }

    return failures;
}