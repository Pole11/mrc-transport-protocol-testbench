#include <cstdint>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <fstream>
#include <iomanip>

#include "rocev2_config.hpp"
#include "util.hpp"
#include "tests.hpp"
#include "mrc_transport_protocol_top.hpp"
#include "mrc_transport_protocol_integration_test_cases.hpp"

#include "axi_utils.hpp"
#include "mrc_transport_protocol/mrc_transport_protocol.hpp"
#include "mrc_transport_protocol/mrc_headers.hpp"
// todo: also add send queues and receive queues

namespace
{

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
        const uint8_t pad = (4 - testCase.payload.size() % 4) % 4;
        appendNetworkOrder8(packet, pad << 4); // se=0, m=0, PadCnt[5:4], tver=0
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
        packet.insert(packet.end(), pad, 0); // pad bytes because ICRC is 4 byte aligned
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

} // namespace

int runMrcTransportProtocolTest()
{
    MrcTransportProtocolTestBench tb;

    std::ofstream trace("integration_trace.jsonl");
    trace << std::boolalpha;
    qpContext rsp[] = {
        qpContext(READY_RECV, RSP_QP_0, 0x100, FIRST_RX_PSN_0, RKEY, 0x70000),
        qpContext(READY_RECV, RSP_QP_1, 0x200, 0xFFFFFE, RKEY, 0x80000),         // wrap, in order
        qpContext(READY_RECV, RSP_QP_2, 0x300, 0xFFFFFB, RKEY, 0x90000),         // wrap, ahead in window
        qpContext(READY_RECV, RSP_QP_3, 0x400, FIRST_RX_PSN_3, RKEY, 0xA0000)    // fresh QP for the rows 21-23
    };
    for (const qpContext& c : rsp) {
        tb.qp_interface.write(c);
        tb.qp_connection_interface.write(ifConnReq(c.qp_num, REMOTE_QPN + c.qp_num - RSP_QP_0, REMOTE_IP, REMOTE_UDP_PORT));
        trace << "{\"qp\": " << c.qp_num.to_uint() << ", \"epsn\": " << c.local_psn.to_uint() << "}\n";
    }

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

        trace << "{\"name\": \"" << name << "\", \"group_end\": " << (afterCycles == DRAIN || tcIndex + 1 == testCaseCount)
              << ", \"exp_accepted\": " << tc.write.exp_accepted << ", \"exp_ack\": " << tc.write.exp_ack
              << ", \"exp_cack_psn\": " << tc.write.exp_cack_psn << ", \"exp_ack_msn\": " << tc.write.exp_ack_msn
              << ", \"exp_syndrome\": " << (tc.write.exp_syndrome ? (int) tc.write.exp_syndrome : 0x1f) << ", \"packet\": \"" << std::hex;
        for (uint8_t b : packet) trace << std::setw(2) << std::setfill('0') << (int) b;
        trace << std::dec << "\"}\n";

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
                const bool isNak = opcode == MRC_ACK && word.data(102, 101) == AETH_NAK;
                if (packetStart && opcode != MRC_SACK && !isNak) {
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