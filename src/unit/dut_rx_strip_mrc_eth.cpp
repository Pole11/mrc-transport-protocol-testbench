#include <cstdint>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "rocev2_config.hpp"
#include "util.hpp"

#include "axi_utils.hpp"
#include "ib_transport_protocol/ib_transport_protocol_impl.hpp"

// it receives data packets and strips the mrc headers (can be variable in size and their presence optional)
// they can either be 4 or 8 bytes

namespace 
{
    #define RX_STRIP_MRC_ETH_INSTID 2 // it is a static module!

    struct StripMrcEthTestCase
    {
        // misc
        const char* name;

        // METH
        uint16_t rqmsn;
        uint16_t msn;

        // [TSETH]
        uint16_t tx_timestamp;
        uint8_t tsr;    // 1 bit
        uint8_t ftype;  // 4 bits

        // payload
        std::vector<uint8_t> payload;

        // mrcEthMeta metaIn
        mrcOpCode op_code;
        bool     ts;

        // expected
        mrcOpCode               exp_op_code;
        bool                    exp_hasMeth; // if it is a request
        uint16_t                exp_rqmsn;
        uint16_t                exp_msn;
        bool                    exp_hasTseth;
        uint16_t                exp_txTimestamp;
        bool                    exp_tsr;
        bool                    exp_hasImmDt;  
        uint32_t                exp_immDt;
        std::vector<uint8_t>    exp_payload;
    };

    struct RxStripMrcEthTestBench
    {
        hls::stream<net_axis<DATA_WIDTH> > input;
        hls::stream<mrcEthMeta>            metaIn;

        hls::stream<exhOpMeta>             metaOut;
        hls::stream<net_axis<DATA_WIDTH> > output;

        void tickOneCycle()
        {
            rx_strip_mrc_eth<DATA_WIDTH, RX_STRIP_MRC_ETH_INSTID>(
                input,
                metaIn,
                metaOut,
                output
            );
        }
    };

    std::vector<uint8_t> buildMrcEthPacket(const StripMrcEthTestCase& testCase)
    {
        std::vector<uint8_t> packet;

        // METH and TSETH only exist on requests, TSETH only if BTH.ts
        if (checkIfMrcRequest(testCase.op_code)) {
            // METH
            appendNetworkOrder16(packet, testCase.rqmsn);
            appendNetworkOrder16(packet, testCase.msn);

            // [TSETH]
            if (testCase.ts) {
                appendNetworkOrder16(packet, testCase.tx_timestamp);
                appendNetworkOrder16(packet, (testCase.tsr << 15) | (testCase.ftype & 0xF));
            }
        }

        // payload
        packet.insert(packet.end(), testCase.payload.begin(), testCase.payload.end());
        return packet;
    }

    bool checkIfCorrectlyStripped(RxStripMrcEthTestBench& tb, const StripMrcEthTestCase& tc) 
    {
        bool returnValue = true;
        auto fail = [&](const std::string& msg) {
            std::cerr << "[FAIL " << tc.name << "] " << msg << std::endl;
            returnValue = false;
        };
        auto expect = [&](const std::string& field, uint64_t got, uint64_t exp) {
            if (got != exp) fail(field + " 0x" + [&]{ std::ostringstream s; s << std::hex << got << " != 0x" << exp; return s.str(); }());
        };

        // exactly one exhOpMeta per packet, with the parsed METH / TSETH
        if (tb.metaOut.size() != 1) fail("metaOut: expected 1 meta, got " + std::to_string(tb.metaOut.size()));
        if (!tb.metaOut.empty()) {
            const exhOpMeta m = tb.metaOut.read();
            expect("op_code",         m.op_code,         tc.exp_op_code);
            expect("mrc.hasMeth",     m.mrc.hasMeth,     tc.exp_hasMeth);
            expect("mrc.rqmsn",       m.mrc.rqmsn,       tc.exp_rqmsn);
            expect("mrc.msn",         m.mrc.msn,         tc.exp_msn);
            expect("mrc.hasTseth",    m.mrc.hasTseth,    tc.exp_hasTseth);
            expect("mrc.txTimestamp", m.mrc.txTimestamp, tc.exp_txTimestamp);
            expect("mrc.tsr",         m.mrc.tsr,         tc.exp_tsr);
            expect("mrc.hasImmDt",    m.mrc.hasImmDt,    tc.exp_hasImmDt);
            expect("mrc.immDt",       m.mrc.immDt,       tc.exp_immDt);
        }

        // output: the packet without METH / TSETH, packed from byte 0, last on the final word only.
        // A packet that is only METH (+ TSETH) still produces one word, with keep = 0
        std::vector<uint8_t> bytes;
        size_t words = 0, lasts = 0;
        bool wordAfterLast = false;
        while (!tb.output.empty()) {
            const net_axis<DATA_WIDTH> word = tb.output.read();
            if (lasts) wordAfterLast = true;
            ++words;
            for (size_t bit = 0; bit < DATA_WIDTH / 8; ++bit)
                if (word.keep[bit]) bytes.push_back(static_cast<uint8_t>(word.data(bit * 8 + 7, bit * 8).to_uint()));
            lasts += word.last;
        }
        const size_t expWords = std::max<size_t>(1, (tc.exp_payload.size() + DATA_WIDTH / 8 - 1) / (DATA_WIDTH / 8));
        if (words != expWords) fail("output: expected " + std::to_string(expWords) + " word(s), got " + std::to_string(words));
        if (lasts != 1)        fail("output: last asserted " + std::to_string(lasts) + " time(s), expected once");
        if (wordAfterLast)     fail("output: words after last");
        if (bytes != tc.exp_payload) {
            fail("output: got " + std::to_string(bytes.size()) + " byte(s), expected " + std::to_string(tc.exp_payload.size()));
            for (size_t i = 0; i < std::min(bytes.size(), tc.exp_payload.size()); ++i)
                if (bytes[i] != tc.exp_payload[i]) {
                    fail("first differing byte at offset " + std::to_string(i) + ": got " + std::to_string(bytes[i]) + ", expected " + std::to_string(tc.exp_payload[i]));
                    break;
                }
        }

        // both inputs consumed
        if (!tb.input.empty())  fail("input words not consumed");
        if (!tb.metaIn.empty()) fail("metaIn not consumed");

        // whatever is left must not leak into the next test case
        while (!tb.metaOut.empty()) tb.metaOut.read();
        while (!tb.input.empty())   tb.input.read();
        while (!tb.metaIn.empty())  tb.metaIn.read();

        return returnValue;
    }

    // payload = everything after METH [+ TSETH]: RETH 16 [+ ImmDt 4] + data. Every row has its own seed.
    // ImmDt is parsed later by rx_process_exh, so exp_hasImmDt / exp_immDt are always false / 0 here.
    // Non-request packets carry no METH / TSETH: they pass through unchanged and their meta is all zero.
    // Word size 64 bytes (DATA_WIDTH 512): sizes below are input sizes = 4 or 8 + payload
    const StripMrcEthTestCase stripMrcEthTestCases[] = {
        // every request opcode, with and without TSETH
        {"STRIP 01 WRITE ONLY",              0x0001, 0x0002, 0,      0, 0,   buildWritePayload(272,  0x10), MRC_RDMA_WRITE_ONLY,     false, MRC_RDMA_WRITE_ONLY,     true, 0x0001, 0x0002, false, 0,      false, false, 0, buildWritePayload(272,  0x10)},
        {"STRIP 02 WRITE FIRST ts",          0x1234, 0x5678, 0xABCD, 1, 0x1, buildWritePayload(1040, 0x20), MRC_RDMA_WRITE_FIRST,    true,  MRC_RDMA_WRITE_FIRST,    true, 0x1234, 0x5678, true,  0xABCD, true,  false, 0, buildWritePayload(1040, 0x20)},
        {"STRIP 03 WRITE MIDDLE",            0x1234, 0x5678, 0,      0, 0,   buildWritePayload(1040, 0x30), MRC_RDMA_WRITE_MIDDLE,   false, MRC_RDMA_WRITE_MIDDLE,   true, 0x1234, 0x5678, false, 0,      false, false, 0, buildWritePayload(1040, 0x30)},
        {"STRIP 04 WRITE LAST ts",           0x1234, 0x5679, 0x0102, 0, 0x1, buildWritePayload(116,  0x40), MRC_RDMA_WRITE_LAST,     true,  MRC_RDMA_WRITE_LAST,     true, 0x1234, 0x5679, true,  0x0102, false, false, 0, buildWritePayload(116,  0x40)},
        {"STRIP 05 WRITE LAST IMM ts",       0x0010, 0x0020, 0x0304, 1, 0x1, buildWritePayload(120,  0x50), MRC_RDMA_WRITE_LAST_IMM, true,  MRC_RDMA_WRITE_LAST_IMM, true, 0x0010, 0x0020, true,  0x0304, true,  false, 0, buildWritePayload(120,  0x50)},
        {"STRIP 06 WRITE ONLY IMM",          0x0011, 0x0021, 0,      0, 0,   buildWritePayload(20,   0x60), MRC_RDMA_WRITE_ONLY_IMM, false, MRC_RDMA_WRITE_ONLY_IMM, true, 0x0011, 0x0021, false, 0,      false, false, 0, buildWritePayload(20,   0x60)},

        // METH / TSETH field values: extremes, and tsr kept apart from ftype (ftype is not exported)
        {"STRIP 07 METH all ones",           0xFFFF, 0xFFFF, 0,      0, 0,   buildWritePayload(100,  0x70), MRC_RDMA_WRITE_ONLY,     false, MRC_RDMA_WRITE_ONLY,     true, 0xFFFF, 0xFFFF, false, 0,      false, false, 0, buildWritePayload(100,  0x70)},
        {"STRIP 08 METH zero",               0x0000, 0x0000, 0,      0, 0,   buildWritePayload(100,  0x71), MRC_RDMA_WRITE_ONLY,     false, MRC_RDMA_WRITE_ONLY,     true, 0x0000, 0x0000, false, 0,      false, false, 0, buildWritePayload(100,  0x71)},
        {"STRIP 09 TSETH tsr 0 ftype F",     0xA1A2, 0xB1B2, 0xFFFF, 0, 0xF, buildWritePayload(100,  0x72), MRC_RDMA_WRITE_ONLY,     true,  MRC_RDMA_WRITE_ONLY,     true, 0xA1A2, 0xB1B2, true,  0xFFFF, false, false, 0, buildWritePayload(100,  0x72)},
        {"STRIP 10 TSETH tsr 1 ftype 0",     0xA1A2, 0xB1B3, 0x0000, 1, 0x0, buildWritePayload(100,  0x73), MRC_RDMA_WRITE_ONLY,     true,  MRC_RDMA_WRITE_ONLY,     true, 0xA1A2, 0xB1B3, true,  0x0000, true,  false, 0, buildWritePayload(100,  0x73)},

        // METH only (4 bytes stripped): input of 60, 64, 65, 68, 69 bytes
        {"STRIP 11 METH 60 in",              0x0101, 0x0102, 0,      0, 0,   buildWritePayload(56,   0x80), MRC_RDMA_WRITE_ONLY,     false, MRC_RDMA_WRITE_ONLY,     true, 0x0101, 0x0102, false, 0,      false, false, 0, buildWritePayload(56,   0x80)},
        {"STRIP 12 METH 64 in",              0x0103, 0x0104, 0,      0, 0,   buildWritePayload(60,   0x81), MRC_RDMA_WRITE_ONLY,     false, MRC_RDMA_WRITE_ONLY,     true, 0x0103, 0x0104, false, 0,      false, false, 0, buildWritePayload(60,   0x81)},
        {"STRIP 13 METH 65 in",              0x0105, 0x0106, 0,      0, 0,   buildWritePayload(61,   0x82), MRC_RDMA_WRITE_ONLY,     false, MRC_RDMA_WRITE_ONLY,     true, 0x0105, 0x0106, false, 0,      false, false, 0, buildWritePayload(61,   0x82)},
        {"STRIP 14 METH 68 in",              0x0107, 0x0108, 0,      0, 0,   buildWritePayload(64,   0x83), MRC_RDMA_WRITE_ONLY,     false, MRC_RDMA_WRITE_ONLY,     true, 0x0107, 0x0108, false, 0,      false, false, 0, buildWritePayload(64,   0x83)},
        {"STRIP 15 METH 69 in",              0x0109, 0x010A, 0,      0, 0,   buildWritePayload(65,   0x84), MRC_RDMA_WRITE_ONLY,     false, MRC_RDMA_WRITE_ONLY,     true, 0x0109, 0x010A, false, 0,      false, false, 0, buildWritePayload(65,   0x84)},

        // METH + TSETH (8 bytes stripped): input of 64, 65, 72, 73 bytes
        {"STRIP 16 TSETH 64 in",             0x0201, 0x0202, 0x0203, 1, 0x1, buildWritePayload(56,   0x90), MRC_RDMA_WRITE_ONLY,     true,  MRC_RDMA_WRITE_ONLY,     true, 0x0201, 0x0202, true,  0x0203, true,  false, 0, buildWritePayload(56,   0x90)},
        {"STRIP 17 TSETH 65 in",             0x0204, 0x0205, 0x0206, 0, 0x1, buildWritePayload(57,   0x91), MRC_RDMA_WRITE_ONLY,     true,  MRC_RDMA_WRITE_ONLY,     true, 0x0204, 0x0205, true,  0x0206, false, false, 0, buildWritePayload(57,   0x91)},
        {"STRIP 18 TSETH 72 in",             0x0207, 0x0208, 0x0209, 1, 0x1, buildWritePayload(64,   0x92), MRC_RDMA_WRITE_ONLY,     true,  MRC_RDMA_WRITE_ONLY,     true, 0x0207, 0x0208, true,  0x0209, true,  false, 0, buildWritePayload(64,   0x92)},
        {"STRIP 19 TSETH 73 in",             0x020A, 0x020B, 0x020C, 0, 0x1, buildWritePayload(65,   0x93), MRC_RDMA_WRITE_ONLY,     true,  MRC_RDMA_WRITE_ONLY,     true, 0x020A, 0x020B, true,  0x020C, false, false, 0, buildWritePayload(65,   0x93)},

        // nothing after the stripped headers: one empty word with last
        {"STRIP 20 METH only",               0x0301, 0x0302, 0,      0, 0,   buildWritePayload(0,    0x00), MRC_RDMA_WRITE_MIDDLE,   false, MRC_RDMA_WRITE_MIDDLE,   true, 0x0301, 0x0302, false, 0,      false, false, 0, buildWritePayload(0,    0x00)},
        {"STRIP 21 METH + TSETH only",       0x0303, 0x0304, 0x0305, 1, 0x1, buildWritePayload(0,    0x00), MRC_RDMA_WRITE_MIDDLE,   true,  MRC_RDMA_WRITE_MIDDLE,   true, 0x0303, 0x0304, true,  0x0305, true,  false, 0, buildWritePayload(0,    0x00)},

        // not a request: passed through unchanged, meta all zero, ts ignored
        {"STRIP 22 ACK ts",                  0,      0,      0,      0, 0,   buildWritePayload(4,    0xA0), MRC_ACK,                 true,  MRC_ACK,                 false, 0,     0,      false, 0,      false, false, 0, buildWritePayload(4,    0xA0)},
        {"STRIP 23 SACK",                    0,      0,      0,      0, 0,   buildWritePayload(36,   0xA1), MRC_SACK,                false, MRC_SACK,                false, 0,     0,      false, 0,      false, false, 0, buildWritePayload(36,   0xA1)},
        {"STRIP 24 RC opcode 300 bytes",     0,      0,      0,      0, 0,   buildWritePayload(300,  0xA2), (mrcOpCode)0x0A,         true,  (mrcOpCode)0x0A,         false, 0,     0,      false, 0,      false, false, 0, buildWritePayload(300,  0xA2)},
        {"STRIP 25 ACK 64 bytes",            0,      0,      0,      0, 0,   buildWritePayload(64,   0xA3), MRC_ACK,                 false, MRC_ACK,                 false, 0,     0,      false, 0,      false, false, 0, buildWritePayload(64,   0xA3)},

        // back to requests: offsets switch 0 -> 8 -> 4 -> 8 between packets
        {"STRIP 26 TSETH after passthrough", 0x0401, 0x0402, 0x0403, 1, 0x1, buildWritePayload(200,  0xB0), MRC_RDMA_WRITE_FIRST,    true,  MRC_RDMA_WRITE_FIRST,    true, 0x0401, 0x0402, true,  0x0403, true,  false, 0, buildWritePayload(200,  0xB0)},
        {"STRIP 27 METH after TSETH",        0x0404, 0x0405, 0,      0, 0,   buildWritePayload(200,  0xB1), MRC_RDMA_WRITE_MIDDLE,   false, MRC_RDMA_WRITE_MIDDLE,   true, 0x0404, 0x0405, false, 0,      false, false, 0, buildWritePayload(200,  0xB1)},
        {"STRIP 28 TSETH after METH",        0x0406, 0x0407, 0x0408, 0, 0x2, buildWritePayload(201,  0xB2), MRC_RDMA_WRITE_LAST,     true,  MRC_RDMA_WRITE_LAST,     true, 0x0406, 0x0407, true,  0x0408, false, false, 0, buildWritePayload(201,  0xB2)},
    };
} // namespace

int runStripMrcEthTest() 
{
    RxStripMrcEthTestBench tb;

    int failures = 0;
    const size_t testCaseCount = sizeof(stripMrcEthTestCases) / sizeof(stripMrcEthTestCases[0]);
    for (size_t tcIndex = 0; tcIndex < testCaseCount; ++tcIndex) {
        const StripMrcEthTestCase& tc = stripMrcEthTestCases[tcIndex];
        std::cout << "\n" << tc.name << "\n" << std::endl;

        const std::vector<uint8_t> packet = buildMrcEthPacket(tc);
        tb.metaIn.write(mrcEthMeta(tc.op_code, tc.ts));

        size_t wordCount = 0;
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

            tb.input.write(wordToSend);
            ++wordCount;
        }

        for (size_t c = 0; c < wordCount + 3; ++c) tb.tickOneCycle(); // meta, one word per call, then a possible remainder word
        failures += !checkIfCorrectlyStripped(tb, tc);
    }

    return failures;
}