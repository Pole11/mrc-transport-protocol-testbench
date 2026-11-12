#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "rocev2_config.hpp"
#include "tests.hpp"
#include "util.hpp"

#include "rocev2/rocev2.hpp"

#ifndef __VITIS_HLS__ // uses the g++ rocev2_top (net_axis ports): make run only

// rocev2_top (the IP) with real IPv4/UDP frames: the WriteIMM completions of the transport must reach its
// m_axis_rx_imm_meta port, and the write commands must carry lst only on the packet that completes WriteIMMs.
// The frames get their ICRC from a second crc instance (INSTID 1), the same code the stack uses on TX

void rocev2_top(stream<net_axis<DATA_WIDTH> >& s_axis_rx_data, stream<net_axis<DATA_WIDTH> >& m_axis_tx_data,
                stream<txMeta>& s_axis_sq_meta, stream<ackMeta>& m_axis_rx_ack_meta, stream<immMeta>& m_axis_rx_imm_meta,
                stream<memCmd>& m_axis_mem_write_cmd, stream<memCmd>& m_axis_mem_read_cmd,
                stream<net_axis<DATA_WIDTH> >& m_axis_mem_write_data, stream<net_axis<DATA_WIDTH> >& s_axis_mem_read_data,
                stream<qpContext>& s_axis_qp_interface, stream<ifConnReq>& s_axis_qp_conn_interface, ap_uint<128> local_ip_address,
#ifdef DBG_IBV
                stream<psnPkg>& m_axis_dbg_0, stream<psnPkg>& m_axis_dbg_1, stream<psnPkg>& m_axis_dbg_2,
#endif
                ap_uint<32>& regCrcDropPkgCount, ap_uint<32>& regInvalidPsnDropCount, ap_uint<32>& regRetransCount,
                ap_uint<32>& regIbvCountRx, ap_uint<32>& regIbvCountTx);

namespace
{
    const uint16_t QPN       = 3;
    const uint32_t FIRST_PSN = 0x100;

    struct Rocev2TestCase
    {
        const char* name;

        // input: a WRITE ONLY (ImmDt with MRC_RDMA_WRITE_ONLY_IMM), 64 bytes of payload
        uint8_t opcode;
        uint32_t psn;
        uint16_t rqmsn;
        uint32_t immDt;

        // expected
        bool exp_lst;                  // lst of its write command
        std::vector<uint32_t> exp_imm; // ImmDt on m_axis_rx_imm_meta, in order, last = 1 only on the final one
    };

    struct Rocev2TestBench
    {
        stream<net_axis<DATA_WIDTH> > rx_data, tx_data, mem_write_data, mem_read_data, frame, frame_icrc;
        stream<txMeta> sq_meta;
        stream<ackMeta> ack_meta;
        stream<immMeta> imm_meta;
        stream<memCmd> mem_write_cmd, mem_read_cmd;
        stream<qpContext> qp_interface;
        stream<ifConnReq> conn_interface;
#ifdef DBG_IBV
        stream<psnPkg> dbg_0, dbg_1, dbg_2;
#endif
        ap_uint<32> crcDrops = 0, icrcDrops = 0, psnDrops = 0, retrans = 0, rxCount = 0, txCount = 0;
        stream<net_axis<DATA_WIDTH> > unused_in, unused_out;

        void tickOneCycle()
        {
            rocev2_top(rx_data, tx_data, sq_meta, ack_meta, imm_meta, mem_write_cmd, mem_read_cmd, mem_write_data, mem_read_data,
                       qp_interface, conn_interface, 0x0A000001,
#ifdef DBG_IBV
                       dbg_0, dbg_1, dbg_2,
#endif
                       crcDrops, psnDrops, retrans, rxCount, txCount);
            crc<DATA_WIDTH, 1>(unused_in, unused_out, frame, frame_icrc, icrcDrops);
            while (!frame_icrc.empty()) rx_data.write(frame_icrc.read());
        }
    };

    // IPv4 + UDP + BTH + METH + RETH [+ ImmDt] + payload, without ICRC (added by the crc instance)
    void sendFrame(Rocev2TestBench& tb, const Rocev2TestCase& tc)
    {
        std::vector<uint8_t> transport;
        appendNetworkOrder8(transport, tc.opcode);
        appendNetworkOrder8(transport, 0);          // se, m, PadCnt = 0 (64 bytes), tver
        appendNetworkOrder16(transport, 0xFFFF);    // P_Key
        appendNetworkOrder8(transport, 0);
        appendNetworkOrder24(transport, QPN);
        appendNetworkOrder8(transport, 1 << 7);     // a = 1
        appendNetworkOrder24(transport, tc.psn);
        appendNetworkOrder16(transport, tc.rqmsn);  // METH
        appendNetworkOrder16(transport, 0);
        appendNetworkOrder64(transport, 0x1000);    // RETH
        appendNetworkOrder32(transport, 0);
        appendNetworkOrder32(transport, 64);
        if (tc.opcode == MRC_RDMA_WRITE_ONLY_IMM)
            appendNetworkOrder32(transport, tc.immDt);
        const std::vector<uint8_t> payload = buildWritePayload(64, tc.psn);
        transport.insert(transport.end(), payload.begin(), payload.end());

        const uint16_t udpLength = 8 + transport.size() + 4;
        std::vector<uint8_t> frame;
        appendNetworkOrder8(frame, 0x45);           // IPv4, 20-byte header
        appendNetworkOrder8(frame, 0);
        appendNetworkOrder16(frame, 20 + udpLength);
        appendNetworkOrder32(frame, 0x00004000);    // id, don't fragment
        appendNetworkOrder8(frame, 64);             // TTL
        appendNetworkOrder8(frame, 17);             // UDP
        appendNetworkOrder16(frame, 0);             // checksum, filled below
        appendNetworkOrder32(frame, 0x0A000002);    // source IP
        appendNetworkOrder32(frame, 0x0A000001);    // destination IP
        uint32_t sum = 0;
        for (int i = 0; i < 20; i += 2) sum += (frame[i] << 8) | frame[i + 1];
        sum = (sum & 0xFFFF) + (sum >> 16);
        frame[10] = ~sum >> 8;
        frame[11] = ~sum;
        appendNetworkOrder16(frame, 0xC000);        // UDP source port (entropy)
        appendNetworkOrder16(frame, RDMA_DEFAULT_PORT);
        appendNetworkOrder16(frame, udpLength);
        appendNetworkOrder16(frame, 0);
        frame.insert(frame.end(), transport.begin(), transport.end());

        for (size_t i = 0; i < frame.size(); i += DATA_WIDTH / 8) {
            net_axis<DATA_WIDTH> word;
            word.data = 0;
            word.keep = 0;
            for (size_t b = 0; b < DATA_WIDTH / 8 && i + b < frame.size(); ++b) {
                word.data(b * 8 + 7, b * 8) = frame[i + b];
                word.keep[b] = 1;
            }
            word.last = i + DATA_WIDTH / 8 >= frame.size();
            tb.frame.write(word);
        }
    }

    bool runAndCheck(Rocev2TestBench& tb, const Rocev2TestCase& tc)
    {
        bool returnValue = true;
        auto fail = [&](const std::string& msg) {
            std::cerr << "[FAIL " << tc.name << "] " << msg << std::endl;
            returnValue = false;
        };

        sendFrame(tb, tc);
        for (int c = 0; c < 500; ++c) tb.tickOneCycle();

        if (tb.mem_write_cmd.size() != 1) fail("expected 1 write command, got " + std::to_string(tb.mem_write_cmd.size()));
        else if (tb.mem_write_cmd.read().lst != tc.exp_lst) fail(std::string("write command lst != ") + (tc.exp_lst ? "1" : "0"));

        if (tb.imm_meta.size() != tc.exp_imm.size())
            fail("expected " + std::to_string(tc.exp_imm.size()) + " WriteIMM completion(s), got " + std::to_string(tb.imm_meta.size()));
        for (size_t k = 0; k < tc.exp_imm.size() && !tb.imm_meta.empty(); ++k) {
            const immMeta m = tb.imm_meta.read();
            if (m.qpn != QPN || m.imm != tc.exp_imm[k] || m.last != (k + 1 == tc.exp_imm.size()))
                fail("completion " + std::to_string(k) + ": qpn " + std::to_string(m.qpn.to_uint()) + ", ImmDt " +
                     m.imm.to_string(16) + ", last " + std::to_string(m.last.to_uint()));
        }

        while (!tb.mem_write_cmd.empty())  tb.mem_write_cmd.read();
        while (!tb.imm_meta.empty())       tb.imm_meta.read();
        while (!tb.mem_write_data.empty()) tb.mem_write_data.read();
        while (!tb.tx_data.empty())        tb.tx_data.read();
        while (!tb.ack_meta.empty())       tb.ack_meta.read();
        return returnValue;
    }

    const Rocev2TestCase rocev2TestCases[] = {
        {.name = "ROCEV2 01 WriteIMM",              .opcode = MRC_RDMA_WRITE_ONLY_IMM, .psn = FIRST_PSN,     .rqmsn = 1, .immDt = 0xCAFE0001, .exp_lst = true,  .exp_imm = {0xCAFE0001}},
        {.name = "ROCEV2 02 plain Write",           .opcode = MRC_RDMA_WRITE_ONLY,     .psn = FIRST_PSN + 1, .rqmsn = 0, .immDt = 0,          .exp_lst = false, .exp_imm = {}},
        // out of order: RQMSN 3 is held behind the hole at FIRST_PSN + 2, which then releases both
        {.name = "ROCEV2 03 WriteIMM held",         .opcode = MRC_RDMA_WRITE_ONLY_IMM, .psn = FIRST_PSN + 3, .rqmsn = 3, .immDt = 0xCAFE0003, .exp_lst = false, .exp_imm = {}},
        {.name = "ROCEV2 04 WriteIMM closes hole",  .opcode = MRC_RDMA_WRITE_ONLY_IMM, .psn = FIRST_PSN + 2, .rqmsn = 2, .immDt = 0xCAFE0002, .exp_lst = true,  .exp_imm = {0xCAFE0002, 0xCAFE0003}},
    };
} // namespace

int runRocev2Test()
{
    Rocev2TestBench tb;

    tb.qp_interface.write(qpContext(READY_RECV, QPN, 0x500, FIRST_PSN, 0x1234, 0));
    tb.qp_interface.write(qpContext(READY_RECV, QPN, 0, 0, 16, 0xCAFEBEEF)); // post 16 receives: every WriteIMM completion consumes one
    tb.conn_interface.write(ifConnReq(QPN, 0x22, 0x0A000002, RDMA_DEFAULT_PORT));
    for (int c = 0; c < 50; ++c) tb.tickOneCycle();

    int failures = 0;
    for (const Rocev2TestCase& tc : rocev2TestCases) {
        std::cout << "\n" << tc.name << "\n" << std::endl;
        failures += !runAndCheck(tb, tc);
    }

    // a frame with a bad ICRC would be dropped silently and the rows above would fail for the wrong reason
    if (tb.crcDrops != 0) {
        std::cerr << "[FAIL] " << tb.crcDrops.to_uint() << " frame(s) dropped for a bad ICRC" << std::endl;
        ++failures;
    }
    return failures;
}
#else
int runRocev2Test() { return 0; }
#endif