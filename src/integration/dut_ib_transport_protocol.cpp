#include <cstdint>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "axi_utils.hpp"
#include "ib_transport_protocol/ib_transport_protocol.hpp"
#include "rocev2_config.hpp"

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

    // how many cycles before feeding next packet
    enum : int32_t { DRAIN = -1, BACK_TO_BACK = 0 };
    
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

    struct AckTestCase { 
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

    };

    struct SackTestCase {
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
        // CC
    };

    struct NackTestCase {
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
    };

    struct ProbeTestCase {
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
    };

    struct EndpointReqTestCase {
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
    };

    struct EndpointResTestCase {
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
    };

    enum class TestCaseKind : uint8_t { WRITE, ACK, SACK, NACK, PROBE, ENDPOINT_REQ, ENDPOINT_RES };
    struct TestCase {
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

    struct IbTransportProtocolTestBench
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

        void tickOneCycle()
        {
            ib_transport_protocol<DATA_WIDTH, 0>(
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
    };

    void appendNetworkOrder8(std::vector<uint8_t>& packet, uint8_t byte)
    {
        packet.push_back(byte);
    }

    void appendNetworkOrder16(std::vector<uint8_t>& packet, uint16_t value)
    {
        appendNetworkOrder8(packet, static_cast<uint8_t>(value >> 8));
        appendNetworkOrder8(packet, static_cast<uint8_t>(value));
    }

    void appendNetworkOrder24(std::vector<uint8_t>& packet, uint32_t value)
    {
        appendNetworkOrder8(packet, static_cast<uint8_t>(value >> 16));
        appendNetworkOrder16(packet, static_cast<uint16_t>(value));
    }

    void appendNetworkOrder32(std::vector<uint8_t>& packet, uint32_t value)
    {
        appendNetworkOrder16(packet, static_cast<uint16_t>(value >> 16));
        appendNetworkOrder16(packet, static_cast<uint16_t>(value));
    }

    void appendNetworkOrder64(std::vector<uint8_t>& packet, uint64_t value)
    {
        appendNetworkOrder32(packet, static_cast<uint32_t>(value >> 32));
        appendNetworkOrder32(packet, static_cast<uint32_t>(value));
    }

    std::vector<uint8_t> buildWritePayload(size_t len, uint8_t seed)
    {
        std::vector<uint8_t> v(len);
        for (size_t i = 0; i < len; i++) v[i] = static_cast<uint8_t>(seed + i);
        return v;
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

    bool checkWriteProcessed(IbTransportProtocolTestBench& tb, const WriteTestCase& tc)
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

    const TestCase testCases[] = {
        // PSN far ahead todo
        WRITE_TC({"MRC WRITE ONLY 01a", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+140, 0, 0, 0,0,0, 0x3000, RKEY, 64, 0x00000000, buildWritePayload(64,0xEE), false, true, FIRST_RX_PSN+0, 0, 0x60, BACK_TO_BACK}),
        
        // in-order ONLY, same va, distinct fills 
        WRITE_TC({"MRC WRITE ONLY 02a", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+0, 0, 0, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x01), true, true, FIRST_RX_PSN+0, 1, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02b", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+1, 0, 1, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x02), true, true, FIRST_RX_PSN+1, 2, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02c", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+2, 0, 2, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x03), true, true, FIRST_RX_PSN+2, 3, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02d", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+3, 0, 3, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x04), true, true, FIRST_RX_PSN+3, 4, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02e", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+4, 0, 4, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x05), true, true, FIRST_RX_PSN+4, 5, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02f", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+5, 0, 5, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x06), true, true, FIRST_RX_PSN+5, 6, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02g", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+6, 0, 6, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x07), true, true, FIRST_RX_PSN+6, 7, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 02h", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+7, 0, 7, 0,0,0, 0x1000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x08), true, true, FIRST_RX_PSN+7, 8, 0x00, BACK_TO_BACK}),

        // exp_accepted, then its duplicate (not rewritten, re-ACKed)
        WRITE_TC({"MRC WRITE ONLY 03a", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+8, 0, 8, 0,0,0, 0x4000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x09), true,  true, FIRST_RX_PSN+8, 9, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 03b", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+8, 0, 8, 0,0,0, 0x4000, RKEY, 1000, 0x00000000, buildWritePayload(1000,0x09), false, true, FIRST_RX_PSN+8, 9, 0x00, BACK_TO_BACK}),

        // older PSNs, not rewritten, re-ACKed todo: understand what PSN
        WRITE_TC({"MRC WRITE ONLY 04a", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+4,  0, 0, 0,0,0, 0x5000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, true, FIRST_RX_PSN+4,  9, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 04b", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN-21, 0, 0, 0,0,0, 0x5000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, true, FIRST_RX_PSN-21, 9, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 04c", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+8,  0, 0, 0,0,0, 0x5000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, true, FIRST_RX_PSN+8,  9, 0x00, BACK_TO_BACK}),

        // multi-packet, RETH on every packet 
        WRITE_TC({"MRC WRITE ONLY 05a", MRC_RDMA_WRITE_FIRST,  0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+9,  0, 9, 0,0,0, 0x10000,         RKEY, 2*PMTU+100, 0x00000000, buildWritePayload(PMTU,0xA1), true, true, FIRST_RX_PSN+9,  9 , 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 05b", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+10, 0, 9, 0,0,0, 0x10000+PMTU,     RKEY, 2*PMTU+100, 0x00000000, buildWritePayload(PMTU,0xA2), true, true, FIRST_RX_PSN+10, 9 , 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 05c", MRC_RDMA_WRITE_LAST,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+11, 0, 9, 0,0,0, 0x10000+2*PMTU,   RKEY, 2*PMTU+100, 0x00000000, buildWritePayload(100,0xA3), true, true, FIRST_RX_PSN+11, 10, 0x00, BACK_TO_BACK}),

        // TSETH on/off 
        WRITE_TC({"MRC WRITE ONLY 06a", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 1,0,0,1, FIRST_RX_PSN+12, 0, 10, 0x1234,0,1, 0x20000,       RKEY, 700,       0x00000000, buildWritePayload(700,0xB0), true, true, FIRST_RX_PSN+12, 11, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 06b", MRC_RDMA_WRITE_FIRST,  0xFFFF, 0x05, 1,0,0,1, FIRST_RX_PSN+13, 0, 11, 0x1234,0,1, 0x21000,       RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(PMTU,0xB1), true, true, FIRST_RX_PSN+13, 11, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 06c", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, 0x05, 1,0,0,1, FIRST_RX_PSN+14, 0, 11, 0x1234,0,1, 0x21000+PMTU,   RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(PMTU,0xB2), true, true, FIRST_RX_PSN+14, 11, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 06d", MRC_RDMA_WRITE_LAST,   0xFFFF, 0x05, 1,0,0,1, FIRST_RX_PSN+15, 0, 11, 0x1234,0,1, 0x21000+2*PMTU, RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(50,0xB3),  true, true, FIRST_RX_PSN+15, 12, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 06e", MRC_RDMA_WRITE_FIRST,  0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+16, 0, 12, 0,0,0,      0x24000,       RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(PMTU,0xC1), true, true, FIRST_RX_PSN+16, 12, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 06f", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, 0x05, 1,0,0,1, FIRST_RX_PSN+17, 0, 12, 0x1234,0,1, 0x24000+PMTU,   RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(PMTU,0xC2), true, true, FIRST_RX_PSN+17, 12, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 06g", MRC_RDMA_WRITE_LAST,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+18, 0, 12, 0,0,0,      0x24000+2*PMTU, RKEY, 2*PMTU+50,  0x00000000, buildWritePayload(50,0xC3),  true, true, FIRST_RX_PSN+18, 13, 0x00, BACK_TO_BACK}),

        // rtx bit 
        WRITE_TC({"MRC WRITE ONLY 07a", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,1,0, FIRST_RX_PSN+19, 0, 13, 0,0,0, 0x30000, RKEY, 200, 0x00000000, buildWritePayload(200,0xD0), true, true, FIRST_RX_PSN+19, 14, 0x00, BACK_TO_BACK}),

        // sizes around the 64-byte beat boundary 
        WRITE_TC({"MRC WRITE ONLY 08a", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+20, 0, 14, 0,0,0,      0x40000, RKEY, 1,  0x00000000, buildWritePayload(1, 0x10), true, true, FIRST_RX_PSN+20, 15, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08b", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+21, 0, 15, 0,0,0,      0x40100, RKEY, 44, 0x00000000, buildWritePayload(44,0x20), true, true, FIRST_RX_PSN+21, 16, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08c", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+22, 0, 16, 0,0,0,      0x40200, RKEY, 45, 0x00000000, buildWritePayload(45,0x30), true, true, FIRST_RX_PSN+22, 17, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08d", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+23, 0, 17, 0,0,0,      0x40300, RKEY, 48, 0x00000000, buildWritePayload(48,0x40), true, true, FIRST_RX_PSN+23, 18, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08e", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+24, 0, 18, 0,0,0,      0x40400, RKEY, 49, 0x00000000, buildWritePayload(49,0x50), true, true, FIRST_RX_PSN+24, 19, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08f", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,1, FIRST_RX_PSN+25, 0, 19, 0x1234,0,1, 0x40500, RKEY, 40, 0x00000000, buildWritePayload(40,0x60), true, true, FIRST_RX_PSN+25, 20, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08g", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,1, FIRST_RX_PSN+26, 0, 20, 0x1234,0,1, 0x40600, RKEY, 48, 0x00000000, buildWritePayload(48,0x70), true, true, FIRST_RX_PSN+26, 21, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 08h", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,1, FIRST_RX_PSN+27, 0, 21, 0x1234,0,1, 0x40700, RKEY, 49, 0x00000000, buildWritePayload(49,0x80), true, true, FIRST_RX_PSN+27, 22, 0x00, BACK_TO_BACK}),

        // ONLY todo: add control packets in the middle
        WRITE_TC({"MRC WRITE ONLY 09a", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+28, 0, 22, 0,0,0, 0x50000, RKEY, 256, 0x00000000, buildWritePayload(256,0x90), true, true, FIRST_RX_PSN+28, 23, 0x00, BACK_TO_BACK}),

        // Write with Immediate 
        WRITE_TC({"MRC WRITE ONLY 10a", MRC_RDMA_WRITE_FIRST,     0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+29, 0, 23, 0,0,0, 0x51000,     RKEY, PMTU+100, 0x00000000, buildWritePayload(PMTU,0xE1), true, true, FIRST_RX_PSN+29, 23, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 10b", MRC_RDMA_WRITE_LAST_IMM,  0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+30, 0, 23, 0,0,0, 0x51000+PMTU, RKEY, PMTU+100, 0x00000000, buildWritePayload(100,0xE2), true, true, FIRST_RX_PSN+30, 24, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 10c", MRC_RDMA_WRITE_ONLY_IMM,  0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+31, 0, 24, 0,0,0, 0x52000,     RKEY, 300,     0x00000000, buildWritePayload(300,0xE3), true, true, FIRST_RX_PSN+31, 25, 0x00, BACK_TO_BACK}),

        // PSN ahead (repo: NAK once, then silent), in order resets it 
        WRITE_TC({"MRC WRITE ONLY 11a", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+182, 0, 0,  0,0,0, 0x2000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, true,  FIRST_RX_PSN+32, 25, 0x60, BACK_TO_BACK}), 
        WRITE_TC({"MRC WRITE ONLY 11b", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+182, 0, 0,  0,0,0, 0x2000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, false, 0, 0, 0x00, BACK_TO_BACK}), 
        WRITE_TC({"MRC WRITE ONLY 11c", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+182, 0, 0,  0,0,0, 0x2000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, false, 0, 0, 0x00, BACK_TO_BACK}), 
        WRITE_TC({"MRC WRITE ONLY 11d", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+32,  0, 25, 0,0,0, 0x2000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0x11), true,  true,  FIRST_RX_PSN+32, 26, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 11e", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+183, 0, 0,  0,0,0, 0x2000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, true,  FIRST_RX_PSN+33, 26, 0x60, BACK_TO_BACK}), 
        WRITE_TC({"MRC WRITE ONLY 11f", MRC_RDMA_WRITE_ONLY, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+183, 0, 0,  0,0,0, 0x2000, RKEY, 1024, 0x00000000, buildWritePayload(1024,0xEE), false, false, 0, 0, 0x00, BACK_TO_BACK}),

        // burst
        WRITE_TC({"MRC WRITE ONLY 12a", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+33, 0, 26, 0,0,0,      0x60000,       RKEY, 500,       0x00000000, buildWritePayload(500,0xF1), true, true, FIRST_RX_PSN+33, 27, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 12b", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 1,0,0,1, FIRST_RX_PSN+34, 0, 27, 0x1234,0,1, 0x61000,       RKEY, 45,        0x00000000, buildWritePayload(45,0xF2),  true, true, FIRST_RX_PSN+34, 28, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 12c", MRC_RDMA_WRITE_FIRST,  0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+35, 0, 28, 0,0,0,      0x62000,       RKEY, 2*PMTU+10,  0x00000000, buildWritePayload(PMTU,0xF3), true, true, FIRST_RX_PSN+35, 28, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 12d", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+36, 0, 28, 0,0,0,      0x62000+PMTU,   RKEY, 2*PMTU+10,  0x00000000, buildWritePayload(PMTU,0xF4), true, true, FIRST_RX_PSN+36, 28, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 12e", MRC_RDMA_WRITE_LAST,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+37, 0, 28, 0,0,0,      0x62000+2*PMTU, RKEY, 2*PMTU+10,  0x00000000, buildWritePayload(10,0xF5),  true, true, FIRST_RX_PSN+37, 29, 0x00, BACK_TO_BACK}),
        
        // same as before but waiting random amount of cycles in between
        WRITE_TC({"MRC WRITE ONLY 13a", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+38, 0, 29, 0,0,0,      0x60000,        RKEY, 500,       0x00000000, buildWritePayload(500,0xF1), true, true, FIRST_RX_PSN+38, 30, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 13b", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 1,0,0,1, FIRST_RX_PSN+39, 0, 30, 0x1234,0,1, 0x61000,        RKEY, 45,        0x00000000, buildWritePayload(45,0xF2),  true, true, FIRST_RX_PSN+39, 31, 0x00, DRAIN}),
        WRITE_TC({"MRC WRITE ONLY 13c", MRC_RDMA_WRITE_FIRST,  0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+40, 0, 31, 0,0,0,      0x62000,        RKEY, 2*PMTU+10,  0x00000000, buildWritePayload(PMTU,0xF3), true, true, FIRST_RX_PSN+40, 31, 0x00, 2}),
        WRITE_TC({"MRC WRITE ONLY 13d", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+41, 0, 31, 0,0,0,      0x62000+PMTU,   RKEY, 2*PMTU+10,  0x00000000, buildWritePayload(PMTU,0xF4), true, true, FIRST_RX_PSN+41, 31, 0x00, 200}),
        WRITE_TC({"MRC WRITE ONLY 13e", MRC_RDMA_WRITE_LAST,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+42, 0, 31, 0,0,0,      0x62000+2*PMTU, RKEY, 2*PMTU+10,  0x00000000, buildWritePayload(10,0xF5),  true, true, FIRST_RX_PSN+42, 32, 0x00, 1000}),

        // BTH.a = 0 -> written, MSN still advances, but no ACK. Each a=0 row is followed by an a=1 row
        // in the same group, so a stray ACK would be caught by the next ACK check
        WRITE_TC({"MRC WRITE ONLY 14a", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 0,0,0,0, FIRST_RX_PSN+43, 0, 32, 0,0,0, 0x80000,        RKEY, 128,       0x00000000, buildWritePayload(128,0x14),  true, false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 14b", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+44, 0, 33, 0,0,0, 0x80100,        RKEY, 128,       0x00000000, buildWritePayload(128,0x15),  true, true,  FIRST_RX_PSN+44, 34, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 14c", MRC_RDMA_WRITE_FIRST,  0xFFFF, 0x05, 0,0,0,0, FIRST_RX_PSN+45, 0, 34, 0,0,0, 0x81000,        RKEY, 2*PMTU+30, 0x00000000, buildWritePayload(PMTU,0x16), true, false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 14d", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, 0x05, 0,0,0,0, FIRST_RX_PSN+46, 0, 34, 0,0,0, 0x81000+PMTU,   RKEY, 2*PMTU+30, 0x00000000, buildWritePayload(PMTU,0x17), true, false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 14e", MRC_RDMA_WRITE_LAST,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+47, 0, 34, 0,0,0, 0x81000+2*PMTU, RKEY, 2*PMTU+30, 0x00000000, buildWritePayload(30,0x18),   true, true,  FIRST_RX_PSN+47, 35, 0x00, DRAIN}),

        // PSN ahead of epsn but inside the 32-PSN window. CURRENT BEHAVIOUR: accepted, epsn jumps to psn+1,
        // the skipped PSNs become "duplicates" (ACKed at their own PSN, never written).
        // RC would NAK 15a; MRC would accept it, keep the hole in the bitmap and accept 15b later.
        WRITE_TC({"MRC WRITE ONLY 15a", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+53, 0, 35, 0,0,0, 0x82000,        RKEY, 256,       0x00000000, buildWritePayload(256,0x19),  true,  true, FIRST_RX_PSN+53, 36, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 15b", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+50, 0, 35, 0,0,0, 0x82100,        RKEY, 256,       0x00000000, buildWritePayload(256,0xEE),  false, true, FIRST_RX_PSN+50, 36, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 15c", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+54, 0, 36, 0,0,0, 0x82200,        RKEY, 256,       0x00000000, buildWritePayload(256,0x1A),  true,  true, FIRST_RX_PSN+54, 37, 0x00, DRAIN}),

        // window edge (epsn = +55): epsn+32 is NAKed, epsn+31 is accepted, then epsn+32 again is in order
        WRITE_TC({"MRC WRITE ONLY 16a", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+87, 0, 37, 0,0,0, 0x83000,        RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, true, FIRST_RX_PSN+55, 37, 0x60, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 16b", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+86, 0, 37, 0,0,0, 0x83100,        RKEY, 64,        0x00000000, buildWritePayload(64,0x1B),   true,  true, FIRST_RX_PSN+86, 38, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 16c", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+87, 0, 38, 0,0,0, 0x83200,        RKEY, 64,        0x00000000, buildWritePayload(64,0x1C),   true,  true, FIRST_RX_PSN+87, 39, 0x00, DRAIN}),

        // only a VALID packet re-arms the NAK: a duplicate in between does not.
        // The NAK is sent regardless of BTH.a (17e has a = 0)
        WRITE_TC({"MRC WRITE ONLY 17a", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+200, 0, 39, 0,0,0, 0x84000,       RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, true,  FIRST_RX_PSN+88, 39, 0x60, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 17b", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+87,  0, 38, 0,0,0, 0x84000,       RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, true,  FIRST_RX_PSN+87, 39, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 17c", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+200, 0, 39, 0,0,0, 0x84000,       RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, false, 0,               0,  0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 17d", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+88,  0, 39, 0,0,0, 0x84100,       RKEY, 64,        0x00000000, buildWritePayload(64,0x1D),   true,  true,  FIRST_RX_PSN+88, 40, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 17e", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 0,0,0,0, FIRST_RX_PSN+201, 0, 40, 0,0,0, 0x84000,       RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, true,  FIRST_RX_PSN+89, 40, 0x60, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 17f", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+89,  0, 40, 0,0,0, 0x84200,       RKEY, 64,        0x00000000, buildWritePayload(64,0x1E),   true,  true,  FIRST_RX_PSN+89, 41, 0x00, DRAIN}),

        // duplicates. CURRENT BEHAVIOUR: ACKed even with BTH.a = 0; a duplicate MIDDLE is ACKed like an ONLY
        WRITE_TC({"MRC WRITE ONLY 18a", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 0,0,0,0, FIRST_RX_PSN+89, 0, 40, 0,0,0, 0x84200,        RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, true, FIRST_RX_PSN+89, 41, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 18b", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+46, 0, 34, 0,0,0, 0x81000+PMTU,   RKEY, 2*PMTU+30, 0x00000000, buildWritePayload(PMTU,0xEE), false, true, FIRST_RX_PSN+46, 41, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 18c", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x05, 1,0,0,0, FIRST_RX_PSN+90, 0, 41, 0,0,0, 0x85000,        RKEY, 64,        0x00000000, buildWritePayload(64,0x1F),   true,  true, FIRST_RX_PSN+90, 42, 0x00, DRAIN}),

        // QP 0x06 (epsn 0xFFFFFE): multi-packet write in order across the 24-bit wrap, then a duplicate behind the wrap
        WRITE_TC({"MRC WRITE ONLY 19a", MRC_RDMA_WRITE_FIRST,  0xFFFF, 0x06, 1,0,0,0, 0xFFFFFE, 0, 0, 0,0,0, 0x70000,        RKEY, 2*PMTU+20, 0x00000000, buildWritePayload(PMTU,0x21), true,  true, 0xFFFFFE, 0, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 19b", MRC_RDMA_WRITE_MIDDLE, 0xFFFF, 0x06, 1,0,0,0, 0xFFFFFF, 0, 0, 0,0,0, 0x70000+PMTU,   RKEY, 2*PMTU+20, 0x00000000, buildWritePayload(PMTU,0x22), true,  true, 0xFFFFFF, 0, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 19c", MRC_RDMA_WRITE_LAST,   0xFFFF, 0x06, 1,0,0,0, 0x000000, 0, 0, 0,0,0, 0x70000+2*PMTU, RKEY, 2*PMTU+20, 0x00000000, buildWritePayload(20,0x23),   true,  true, 0x000000, 1, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 19d", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x06, 1,0,0,0, 0xFFFFFF, 0, 0, 0,0,0, 0x71000,        RKEY, 64,        0x00000000, buildWritePayload(64,0xEE),   false, true, 0xFFFFFF, 1, 0x00, BACK_TO_BACK}),
        WRITE_TC({"MRC WRITE ONLY 19e", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x06, 1,0,0,0, 0x000001, 0, 1, 0,0,0, 0x71100,        RKEY, 64,        0x00000000, buildWritePayload(64,0x24),   true,  true, 0x000001, 2, 0x00, DRAIN}),

        // QP 0x07 (epsn 0xFFFFFB): ahead by 7 ACROSS the wrap. Expected = same behaviour as 15a (accepted).
        WRITE_TC({"MRC WRITE ONLY 20a", MRC_RDMA_WRITE_ONLY,   0xFFFF, 0x07, 1,0,0,0, 0x000002, 0, 0, 0,0,0, 0x78000,        RKEY, 64,        0x00000000, buildWritePayload(64,0x25),   true,  true, 0x000002, 1, 0x00, DRAIN}),
    };
} // namespace

int runIbTransportProtocolTest()
{
    IbTransportProtocolTestBench tb;

    tb.qp_interface.write(qpContext(READY_RECV, 0x05, 0x100, FIRST_RX_PSN, RKEY, 0x70000));
    tb.qp_connection_interface.write(ifConnReq(0x05, REMOTE_QPN, REMOTE_IP, REMOTE_UDP_PORT));
    
    tb.qp_interface.write(qpContext(READY_RECV, 0x06, 0x200, 0xFFFFFE, RKEY, 0x80000));   // wrap, in order
    tb.qp_connection_interface.write(ifConnReq(0x06, REMOTE_QPN + 1, REMOTE_IP, REMOTE_UDP_PORT));
    
    tb.qp_interface.write(qpContext(READY_RECV, 0x07, 0x300, 0xFFFFFB, RKEY, 0x90000));   // wrap, ahead in window
    tb.qp_connection_interface.write(ifConnReq(0x07, REMOTE_QPN + 2, REMOTE_IP, REMOTE_UDP_PORT));

    for (int i = 0; i < 60; ++i) tb.tickOneCycle();

    std::vector<size_t> group; // rows fed but not checked yet
    int failures = 0;
    const size_t testCaseCount = sizeof(testCases) / sizeof(testCases[0]);
    for (size_t tcIndex = 0; tcIndex < testCaseCount; ++tcIndex) {
        std::cout << "\n" << testCases[tcIndex].write.name << "\n" << std::endl;
        
        const std::vector<uint8_t> packet = buildWritePacket(testCases[tcIndex].write);
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
        for (int c = 0; c < testCases[tcIndex].write.after_cycles; ++c) tb.tickOneCycle(); // idle gap only, no check
        if (testCases[tcIndex].write.after_cycles == DRAIN || tcIndex + 1 == testCaseCount) { // drain or last test case
            for (int c = 0; c < VERY_BIG_NUMBER; ++c) tb.tickOneCycle(); // drain
            std::cout << "checking group of " << std::dec << group.size() << " packet(s)" << std::endl;
            for (size_t k : group) {
                if (!checkWriteProcessed(tb, testCases[k].write)) ++failures;
            }

            if (!tb.mem_write_cmd.empty() || !tb.mem_write_data.empty()) {
                std::cerr << "[FAIL] leftover memory output at end of group" << std::endl;
                ++failures;
            }
            while (!tb.mem_write_cmd.empty())  tb.mem_write_cmd.read();
            while (!tb.mem_write_data.empty()) tb.mem_write_data.read();
            while (!tb.tx_data.empty())        tb.tx_data.read();   // unclaimed NAKs/SACKs
            while (!tb.tx_meta.empty())        tb.tx_meta.read();
            
            group.clear();
        }
    }

    return failures;
}