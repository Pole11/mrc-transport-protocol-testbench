#include <cstdint>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "rocev2_config.hpp"
#include "util.hpp"

#include "axi_utils.hpp"
#include "ib_transport_protocol/ib_transport_protocol.hpp"
#include "ib_transport_protocol/ib_transport_protocol_impl.hpp"
#include "ib_transport_protocol/mrc_headers.hpp"

// this module must output
// 1. process BTH (extract opcode, partition key, destination QP, psn, and so on)
// 2. opcode
// 3. whole unchanged packet
// 4. extract metadata of control packets
// 5. say if packet is control or not
// 6. count how many packets passed through it

namespace 
{
    #define RX_PROCESS_IBH_INSTID 2

    struct BthTestCase
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

        // bytes after the BTH (not parsed by rx_process_ibh, only forwarded)
        uint16_t payload_len;

        // expected
        ibhMeta exp_metaToIbhFsmOut;
        mrcEthMeta exp_metaToStripCtrlOut;
        mrcCtrlBthMeta exp_metaToProcCtrlOut;
        bool exp_isCtrlDataOut;
	    bool exp_isCtrlUdpOut;
        ap_uint<32> exp_regIbvCountRx;
    };

    struct RxProcessIbhTestBench
    {
#ifdef DBG_IBV
        hls::stream<psnPkg> m_axis_dbg;
#endif
        hls::stream<net_axis<DATA_WIDTH> > input;
    
        hls::stream<ibhMeta> metaToIbhFsmOut;              	// data path only (requests)
        hls::stream<mrcEthMeta> metaToStripCtrlOut;        	// data path only, you only need to know if it has METH and TSETH
        hls::stream<mrcCtrlBthMeta> metaToProcCtrlOut;   	// control path only, you must have all the possible information of BTH needed for ctrl
        hls::stream<bool> isCtrlDataOut;            // every packet, true = control
        hls::stream<bool> isCtrlUdpOut;             // every packet, true = control
        hls::stream<net_axis<DATA_WIDTH> > forwardInputOut;
        ap_uint<32>		regIbvCountRx;
    
        void tickOneCycle()
        {
            rx_process_ibh<DATA_WIDTH, RX_PROCESS_IBH_INSTID>(
#ifdef DBG_IBV
                m_axis_dbg,
#endif
                input,
                metaToIbhFsmOut, metaToStripCtrlOut, metaToProcCtrlOut,
                isCtrlDataOut, isCtrlUdpOut,
                forwardInputOut,
                regIbvCountRx
            );
#ifdef DBG_IBV
            while (!m_axis_dbg.empty()) m_axis_dbg.read();
#endif
        }
    };

    std::vector<uint8_t> buildBthHeader(const BthTestCase& testCase)
    {
        std::vector<uint8_t> packet;
        
        appendNetworkOrder8(packet, testCase.opcode);
        appendNetworkOrder8(packet, 0); // pad and tver=0
        appendNetworkOrder16(packet, testCase.p_key);
        appendNetworkOrder8(packet, 0); // var-res
        appendNetworkOrder24(packet, testCase.dqp);
        appendNetworkOrder8(packet, (testCase.a << 7) | (testCase.R << 6) | (testCase.rtx << 5) | (testCase.ts << 4)); // a, R, rtx, ts
        appendNetworkOrder24(packet, testCase.psn);

        return packet;
    }

    bool checkBthProcessed(RxProcessIbhTestBench& tb, const BthTestCase& tc, const std::vector<net_axis<DATA_WIDTH> >& sentWords)
    {
        bool returnValue = true;
        auto fail = [&](const std::string& msg) {
            std::cerr << "[FAIL " << tc.name << "] " << msg << std::endl;
            returnValue = false;
        };
        auto expect = [&](const std::string& field, uint64_t got, uint64_t exp) {
            if (got != exp) fail(field + " 0x" + [&]{ std::ostringstream s; s << std::hex << got << " != 0x" << exp; return s.str(); }());
        };

        // control flags: exactly one per packet on each stream, also for packets of many words
        if (tb.isCtrlDataOut.size() != 1) fail("isCtrlDataOut: expected 1 value, got " + std::to_string(tb.isCtrlDataOut.size()));
        if (!tb.isCtrlDataOut.empty())    expect("isCtrlDataOut", tb.isCtrlDataOut.read(), tc.exp_isCtrlDataOut);
        if (tb.isCtrlUdpOut.size() != 1)  fail("isCtrlUdpOut: expected 1 value, got " + std::to_string(tb.isCtrlUdpOut.size()));
        if (!tb.isCtrlUdpOut.empty())     expect("isCtrlUdpOut", tb.isCtrlUdpOut.read(), tc.exp_isCtrlUdpOut);

        // data path metadata: one ibhMeta and one mrcEthMeta for a request, none for a control packet
        const size_t expDataMetas = tc.exp_isCtrlDataOut ? 0 : 1;
        if (tb.metaToIbhFsmOut.size() != expDataMetas)    fail("metaToIbhFsmOut: expected " + std::to_string(expDataMetas) + " meta, got " + std::to_string(tb.metaToIbhFsmOut.size()));
        if (tb.metaToStripCtrlOut.size() != expDataMetas) fail("metaToStripCtrlOut: expected " + std::to_string(expDataMetas) + " meta, got " + std::to_string(tb.metaToStripCtrlOut.size()));
        if (expDataMetas && !tb.metaToIbhFsmOut.empty()) {
            const ibhMeta m = tb.metaToIbhFsmOut.read();
            const ibhMeta& e = tc.exp_metaToIbhFsmOut;
            expect("ibhMeta.op_code",       m.op_code,       e.op_code);
            expect("ibhMeta.partition_key", m.partition_key, e.partition_key);
            expect("ibhMeta.dest_qp",       m.dest_qp,       e.dest_qp);
            expect("ibhMeta.psn",           m.psn,           e.psn);
            expect("ibhMeta.validPSN",      m.validPSN,      e.validPSN);
            expect("ibhMeta.numPkg",        m.numPkg,        e.numPkg);
            expect("ibhMeta.ackReq",        m.ackReq,        e.ackReq);
            expect("ibhMeta.rtx",           m.rtx,           e.rtx);
            expect("ibhMeta.ts",            m.ts,            e.ts);
        }
        if (expDataMetas && !tb.metaToStripCtrlOut.empty()) {
            const mrcEthMeta m = tb.metaToStripCtrlOut.read();
            expect("mrcEthMeta.op_code", m.op_code, tc.exp_metaToStripCtrlOut.op_code);
            expect("mrcEthMeta.ts",      m.ts,      tc.exp_metaToStripCtrlOut.ts);
        }

        // control path metadata: one mrcCtrlBthMeta for a control packet, none for a request
        const size_t expCtrlMetas = tc.exp_isCtrlDataOut ? 1 : 0;
        if (tb.metaToProcCtrlOut.size() != expCtrlMetas) fail("metaToProcCtrlOut: expected " + std::to_string(expCtrlMetas) + " meta, got " + std::to_string(tb.metaToProcCtrlOut.size()));
        if (expCtrlMetas && !tb.metaToProcCtrlOut.empty()) {
            const mrcCtrlBthMeta m = tb.metaToProcCtrlOut.read();
            const mrcCtrlBthMeta& e = tc.exp_metaToProcCtrlOut;
            expect("mrcCtrlBthMeta.op_code",       m.op_code,       e.op_code);
            expect("mrcCtrlBthMeta.partition_key", m.partition_key, e.partition_key);
            expect("mrcCtrlBthMeta.dest_qp",       m.dest_qp,       e.dest_qp);
            expect("mrcCtrlBthMeta.psn",           m.psn,           e.psn);
            expect("mrcCtrlBthMeta.ackReq",        m.ackReq,        e.ackReq);
            expect("mrcCtrlBthMeta.rtx",           m.rtx,           e.rtx);
        }

        // forwarded packet: every word, unchanged, BTH included
        if (tb.forwardInputOut.size() != sentWords.size())
            fail("forwardInputOut: expected " + std::to_string(sentWords.size()) + " word(s), got " + std::to_string(tb.forwardInputOut.size()));
        for (size_t i = 0; i < sentWords.size() && !tb.forwardInputOut.empty(); ++i) {
            const net_axis<DATA_WIDTH> word = tb.forwardInputOut.read();
            if (word.data != sentWords[i].data) fail("forwarded word " + std::to_string(i) + ": data changed");
            if (word.keep != sentWords[i].keep) fail("forwarded word " + std::to_string(i) + ": keep changed");
            if (word.last != sentWords[i].last) fail("forwarded word " + std::to_string(i) + ": last " + std::to_string(word.last.to_uint()));
        }

        // packet counter: cumulative, control packets included
        expect("regIbvCountRx", tb.regIbvCountRx, tc.exp_regIbvCountRx);

        // whatever is left (wrong counts above) must not leak into the next test case
        while (!tb.isCtrlDataOut.empty())      tb.isCtrlDataOut.read();
        while (!tb.isCtrlUdpOut.empty())       tb.isCtrlUdpOut.read();
        while (!tb.metaToIbhFsmOut.empty())    tb.metaToIbhFsmOut.read();
        while (!tb.metaToStripCtrlOut.empty()) tb.metaToStripCtrlOut.read();
        while (!tb.metaToProcCtrlOut.empty())  tb.metaToProcCtrlOut.read();
        while (!tb.forwardInputOut.empty())    tb.forwardInputOut.read();

        return returnValue;
    }

    // rx_process_ibh only parses the BTH, the bytes after it are random-looking filler of payload_len bytes.
    // A request carries every BTH field into ibhMeta (validPSN = true, numPkg = 1); a control packet into mrcCtrlBthMeta (no ts).
    // exp_regIbvCountRx counts every packet since the start of the test, so the rows must stay in this order.
    const BthTestCase bthTestCases[] = {
        // requests: every MRC write opcode, flags one at a time, packets of 1, 2 and many words
        {"IBH 01 WRITE ONLY",     MRC_RDMA_WRITE_ONLY,     0xFFFF, 0x000005, 1,0,0,0, 0x0A0B00, 64,   ibhMeta(MRC_RDMA_WRITE_ONLY,     0xFFFF, 0x000005, 0x0A0B00, true, true,  false, false), mrcEthMeta(MRC_RDMA_WRITE_ONLY,     false), mrcCtrlBthMeta(), false, false, 1},
        {"IBH 02 WRITE FIRST ts", MRC_RDMA_WRITE_FIRST,    0xFFFF, 0x000005, 1,0,0,1, 0x0A0B01, 1024, ibhMeta(MRC_RDMA_WRITE_FIRST,    0xFFFF, 0x000005, 0x0A0B01, true, true,  false, true),  mrcEthMeta(MRC_RDMA_WRITE_FIRST,    true),  mrcCtrlBthMeta(), false, false, 2},
        {"IBH 03 WRITE MIDDLE",   MRC_RDMA_WRITE_MIDDLE,   0xFFFF, 0x000005, 0,0,1,0, 0x0A0B02, 1024, ibhMeta(MRC_RDMA_WRITE_MIDDLE,   0xFFFF, 0x000005, 0x0A0B02, true, false, true,  false), mrcEthMeta(MRC_RDMA_WRITE_MIDDLE,   false), mrcCtrlBthMeta(), false, false, 3},
        {"IBH 04 WRITE LAST R",   MRC_RDMA_WRITE_LAST,     0x1234, 0x000005, 1,1,0,0, 0x0A0B03, 100,  ibhMeta(MRC_RDMA_WRITE_LAST,     0x1234, 0x000005, 0x0A0B03, true, true,  false, false), mrcEthMeta(MRC_RDMA_WRITE_LAST,     false), mrcCtrlBthMeta(), false, false, 4},
        {"IBH 05 WRITE LAST IMM", MRC_RDMA_WRITE_LAST_IMM, 0xFFFF, 0x000006, 1,0,0,1, 0x000000, 52,   ibhMeta(MRC_RDMA_WRITE_LAST_IMM, 0xFFFF, 0x000006, 0x000000, true, true,  false, true),  mrcEthMeta(MRC_RDMA_WRITE_LAST_IMM, true),  mrcCtrlBthMeta(), false, false, 5},
        {"IBH 06 WRITE ONLY IMM", MRC_RDMA_WRITE_ONLY_IMM, 0x0000, 0xFFFFFF, 1,1,1,1, 0xFFFFFF, 53,   ibhMeta(MRC_RDMA_WRITE_ONLY_IMM, 0x0000, 0xFFFFFF, 0xFFFFFF, true, true,  true,  true),  mrcEthMeta(MRC_RDMA_WRITE_ONLY_IMM, true),  mrcCtrlBthMeta(), false, false, 6},
        {"IBH 07 BTH only",       MRC_RDMA_WRITE_ONLY,     0xFFFF, 0x123456, 0,0,0,0, 0x654321, 0,    ibhMeta(MRC_RDMA_WRITE_ONLY,     0xFFFF, 0x123456, 0x654321, true, false, false, false), mrcEthMeta(MRC_RDMA_WRITE_ONLY,     false), mrcCtrlBthMeta(), false, false, 7},

        // control packets: every control opcode, header-sized payload (AETH 4, SETH+CC 36, NETH 20, PETH/ERTH 16, EETH 36)
        {"IBH 08 ACK",            MRC_ACK,                 0xFFFF, 0x000008, 0,0,0,0, 0x000202, 4,    ibhMeta(), mrcEthMeta(), mrcCtrlBthMeta(MRC_ACK,       0xFFFF, 0x000008, 0x000202, false, false), true, true, 8},
        {"IBH 09 SACK",           MRC_SACK,                0xFFFF, 0x000008, 0,0,0,0, 0x00ABCD, 36,   ibhMeta(), mrcEthMeta(), mrcCtrlBthMeta(MRC_SACK,      0xFFFF, 0x000008, 0x00ABCD, false, false), true, true, 9},
        {"IBH 10 NACK",           MRC_NACK,                0xFFFF, 0x000009, 0,0,0,0, 0x000200, 20,   ibhMeta(), mrcEthMeta(), mrcCtrlBthMeta(MRC_NACK,      0xFFFF, 0x000009, 0x000200, false, false), true, true, 10},
        {"IBH 11 PROBE",          MRC_PROBE_REQ,           0xFFFF, 0x000005, 0,0,0,0, 0x000000, 16,   ibhMeta(), mrcEthMeta(), mrcCtrlBthMeta(MRC_PROBE_REQ, 0xFFFF, 0x000005, 0x000000, false, false), true, true, 11},
        {"IBH 12 EP REQ",         MRC_EP_REQ,              0xFFFF, 0x000002, 0,0,0,0, 0x003002, 16,   ibhMeta(), mrcEthMeta(), mrcCtrlBthMeta(MRC_EP_REQ,    0xFFFF, 0x000002, 0x003002, false, false), true, true, 12},
        {"IBH 13 EP RES",         MRC_EP_RSP,              0xFFFF, 0x000002, 0,0,0,0, 0x003002, 36,   ibhMeta(), mrcEthMeta(), mrcCtrlBthMeta(MRC_EP_RSP,    0xFFFF, 0x000002, 0x003002, false, false), true, true, 13},

        // control packet with every flag set (ts is not carried on the control path) and a payload longer than one word
        {"IBH 14 NACK flags",     MRC_NACK,                0xABCD, 0xFFFFFF, 1,1,1,1, 0xFFFFFF, 200,  ibhMeta(), mrcEthMeta(), mrcCtrlBthMeta(MRC_NACK,      0xABCD, 0xFFFFFF, 0xFFFFFF, true,  true),  true, true, 14},

        // CURRENT BEHAVIOUR: any opcode that is not MRC control (here RC RDMA WRITE ONLY 0x0A) goes to the data path
        {"IBH 15 RC opcode",      0x0A,                    0xFFFF, 0x000005, 1,0,0,0, 0x0A0B04, 64,   ibhMeta((mrcOpCode)0x0A,         0xFFFF, 0x000005, 0x0A0B04, true, true,  false, false), mrcEthMeta((mrcOpCode)0x0A,         false), mrcCtrlBthMeta(), false, false, 15},

        // back to a request after control packets: the parser restarted cleanly at every last
        {"IBH 16 WRITE ONLY",     MRC_RDMA_WRITE_ONLY,     0xFFFF, 0x000007, 0,0,1,1, 0x000002, 128,  ibhMeta(MRC_RDMA_WRITE_ONLY,     0xFFFF, 0x000007, 0x000002, true, false, true,  true),  mrcEthMeta(MRC_RDMA_WRITE_ONLY,     true),  mrcCtrlBthMeta(), false, false, 16},
    };

}

int runRxProcessIbhTest() 
{
    RxProcessIbhTestBench tb;

    int failures = 0;
    const size_t testCaseCount = sizeof(bthTestCases) / sizeof(bthTestCases[0]);
    for (size_t tcIndex = 0; tcIndex < testCaseCount; ++tcIndex) {
        const BthTestCase& tc = bthTestCases[tcIndex];
        std::cout << "\n" << tc.name << "\n" << std::endl;

        std::vector<uint8_t> packet;
        // BTH
        packet = buildBthHeader(tc);

        // payload
        for (size_t i = 0; i < tc.payload_len; ++i) packet.push_back(static_cast<uint8_t>(tcIndex * 16 + i));

        std::vector<net_axis<DATA_WIDTH> > words;
        const size_t bytesPerWord = DATA_WIDTH / 8;
        for (size_t byteIndex = 0; byteIndex < packet.size(); byteIndex += bytesPerWord) {
            net_axis<DATA_WIDTH> wordToSend;
            wordToSend.data = 0;
            wordToSend.keep = 0;

            const size_t howManyBytes = std::min(bytesPerWord, packet.size() - byteIndex);
            for (size_t byte = 0; byte < howManyBytes; byte++) {
                wordToSend.data(byte * 8 + 7, byte * 8) = packet[byteIndex + byte];
                wordToSend.keep[byte] = 1;
            }
            wordToSend.last = byteIndex + bytesPerWord >= packet.size();

            words.push_back(wordToSend);
            tb.input.write(wordToSend);
        }

        for (size_t c = 0; c < words.size() + 1; ++c) tb.tickOneCycle(); // one word per call
        failures += !checkBthProcessed(tb, tc, words);
    }

    return failures;
}