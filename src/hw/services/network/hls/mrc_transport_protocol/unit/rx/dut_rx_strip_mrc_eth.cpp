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
#include "mrc_transport_protocol/rx/rx_strip_mrc_eth.hpp"

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
        {.name = "STRIP 01 WRITE ONLY", .rqmsn = 0x0001, .msn = 0x0002,
         .tx_timestamp = 0, .tsr = 0, .ftype = 0, .payload = buildWritePayload(272, 0x10), .op_code = MRC_RDMA_WRITE_ONLY, .ts = false,
         .exp_op_code = MRC_RDMA_WRITE_ONLY, .exp_hasMeth = true, .exp_rqmsn = 0x0001, .exp_msn = 0x0002, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(272, 0x10)},
        {.name = "STRIP 02 WRITE FIRST ts", .rqmsn = 0x1234, .msn = 0x5678,
         .tx_timestamp = 0xABCD, .tsr = 1, .ftype = 0x1, .payload = buildWritePayload(1040, 0x20), .op_code = MRC_RDMA_WRITE_FIRST, .ts = true,
         .exp_op_code = MRC_RDMA_WRITE_FIRST, .exp_hasMeth = true, .exp_rqmsn = 0x1234, .exp_msn = 0x5678, .exp_hasTseth = true, .exp_txTimestamp = 0xABCD, .exp_tsr = true, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(1040, 0x20)},
        {.name = "STRIP 03 WRITE MIDDLE", .rqmsn = 0x1234, .msn = 0x5678,
         .tx_timestamp = 0, .tsr = 0, .ftype = 0, .payload = buildWritePayload(1040, 0x30), .op_code = MRC_RDMA_WRITE_MIDDLE, .ts = false,
         .exp_op_code = MRC_RDMA_WRITE_MIDDLE, .exp_hasMeth = true, .exp_rqmsn = 0x1234, .exp_msn = 0x5678, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(1040, 0x30)},
        {.name = "STRIP 04 WRITE LAST ts", .rqmsn = 0x1234, .msn = 0x5679,
         .tx_timestamp = 0x0102, .tsr = 0, .ftype = 0x1, .payload = buildWritePayload(116, 0x40), .op_code = MRC_RDMA_WRITE_LAST, .ts = true,
         .exp_op_code = MRC_RDMA_WRITE_LAST, .exp_hasMeth = true, .exp_rqmsn = 0x1234, .exp_msn = 0x5679, .exp_hasTseth = true, .exp_txTimestamp = 0x0102, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(116, 0x40)},
        {.name = "STRIP 05 WRITE LAST IMM ts", .rqmsn = 0x0010, .msn = 0x0020,
         .tx_timestamp = 0x0304, .tsr = 1, .ftype = 0x1, .payload = buildWritePayload(120, 0x50), .op_code = MRC_RDMA_WRITE_LAST_IMM, .ts = true,
         .exp_op_code = MRC_RDMA_WRITE_LAST_IMM, .exp_hasMeth = true, .exp_rqmsn = 0x0010, .exp_msn = 0x0020, .exp_hasTseth = true, .exp_txTimestamp = 0x0304, .exp_tsr = true, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(120, 0x50)},
        {.name = "STRIP 06 WRITE ONLY IMM", .rqmsn = 0x0011, .msn = 0x0021,
         .tx_timestamp = 0, .tsr = 0, .ftype = 0, .payload = buildWritePayload(20, 0x60), .op_code = MRC_RDMA_WRITE_ONLY_IMM, .ts = false,
         .exp_op_code = MRC_RDMA_WRITE_ONLY_IMM, .exp_hasMeth = true, .exp_rqmsn = 0x0011, .exp_msn = 0x0021, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(20, 0x60)},

        // METH / TSETH field values: extremes, and tsr kept apart from ftype (ftype is not exported)
        {.name = "STRIP 07 METH all ones", .rqmsn = 0xFFFF, .msn = 0xFFFF,
         .tx_timestamp = 0, .tsr = 0, .ftype = 0, .payload = buildWritePayload(100, 0x70), .op_code = MRC_RDMA_WRITE_ONLY, .ts = false,
         .exp_op_code = MRC_RDMA_WRITE_ONLY, .exp_hasMeth = true, .exp_rqmsn = 0xFFFF, .exp_msn = 0xFFFF, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(100, 0x70)},
        {.name = "STRIP 08 METH zero", .rqmsn = 0x0000, .msn = 0x0000,
         .tx_timestamp = 0, .tsr = 0, .ftype = 0, .payload = buildWritePayload(100, 0x71), .op_code = MRC_RDMA_WRITE_ONLY, .ts = false,
         .exp_op_code = MRC_RDMA_WRITE_ONLY, .exp_hasMeth = true, .exp_rqmsn = 0x0000, .exp_msn = 0x0000, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(100, 0x71)},
        {.name = "STRIP 09 TSETH tsr 0 ftype F", .rqmsn = 0xA1A2, .msn = 0xB1B2,
         .tx_timestamp = 0xFFFF, .tsr = 0, .ftype = 0xF, .payload = buildWritePayload(100, 0x72), .op_code = MRC_RDMA_WRITE_ONLY, .ts = true,
         .exp_op_code = MRC_RDMA_WRITE_ONLY, .exp_hasMeth = true, .exp_rqmsn = 0xA1A2, .exp_msn = 0xB1B2, .exp_hasTseth = true, .exp_txTimestamp = 0xFFFF, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(100, 0x72)},
        {.name = "STRIP 10 TSETH tsr 1 ftype 0", .rqmsn = 0xA1A2, .msn = 0xB1B3,
         .tx_timestamp = 0x0000, .tsr = 1, .ftype = 0x0, .payload = buildWritePayload(100, 0x73), .op_code = MRC_RDMA_WRITE_ONLY, .ts = true,
         .exp_op_code = MRC_RDMA_WRITE_ONLY, .exp_hasMeth = true, .exp_rqmsn = 0xA1A2, .exp_msn = 0xB1B3, .exp_hasTseth = true, .exp_txTimestamp = 0x0000, .exp_tsr = true, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(100, 0x73)},

        // METH only (4 bytes stripped): input of 60, 64, 65, 68, 69 bytes
        {.name = "STRIP 11 METH 60 in", .rqmsn = 0x0101, .msn = 0x0102,
         .tx_timestamp = 0, .tsr = 0, .ftype = 0, .payload = buildWritePayload(56, 0x80), .op_code = MRC_RDMA_WRITE_ONLY, .ts = false,
         .exp_op_code = MRC_RDMA_WRITE_ONLY, .exp_hasMeth = true, .exp_rqmsn = 0x0101, .exp_msn = 0x0102, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(56, 0x80)},
        {.name = "STRIP 12 METH 64 in", .rqmsn = 0x0103, .msn = 0x0104,
         .tx_timestamp = 0, .tsr = 0, .ftype = 0, .payload = buildWritePayload(60, 0x81), .op_code = MRC_RDMA_WRITE_ONLY, .ts = false,
         .exp_op_code = MRC_RDMA_WRITE_ONLY, .exp_hasMeth = true, .exp_rqmsn = 0x0103, .exp_msn = 0x0104, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(60, 0x81)},
        {.name = "STRIP 13 METH 65 in", .rqmsn = 0x0105, .msn = 0x0106,
         .tx_timestamp = 0, .tsr = 0, .ftype = 0, .payload = buildWritePayload(61, 0x82), .op_code = MRC_RDMA_WRITE_ONLY, .ts = false,
         .exp_op_code = MRC_RDMA_WRITE_ONLY, .exp_hasMeth = true, .exp_rqmsn = 0x0105, .exp_msn = 0x0106, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(61, 0x82)},
        {.name = "STRIP 14 METH 68 in", .rqmsn = 0x0107, .msn = 0x0108,
         .tx_timestamp = 0, .tsr = 0, .ftype = 0, .payload = buildWritePayload(64, 0x83), .op_code = MRC_RDMA_WRITE_ONLY, .ts = false,
         .exp_op_code = MRC_RDMA_WRITE_ONLY, .exp_hasMeth = true, .exp_rqmsn = 0x0107, .exp_msn = 0x0108, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(64, 0x83)},
        {.name = "STRIP 15 METH 69 in", .rqmsn = 0x0109, .msn = 0x010A,
         .tx_timestamp = 0, .tsr = 0, .ftype = 0, .payload = buildWritePayload(65, 0x84), .op_code = MRC_RDMA_WRITE_ONLY, .ts = false,
         .exp_op_code = MRC_RDMA_WRITE_ONLY, .exp_hasMeth = true, .exp_rqmsn = 0x0109, .exp_msn = 0x010A, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(65, 0x84)},

        // METH + TSETH (8 bytes stripped): input of 64, 65, 72, 73 bytes
        {.name = "STRIP 16 TSETH 64 in", .rqmsn = 0x0201, .msn = 0x0202,
         .tx_timestamp = 0x0203, .tsr = 1, .ftype = 0x1, .payload = buildWritePayload(56, 0x90), .op_code = MRC_RDMA_WRITE_ONLY, .ts = true,
         .exp_op_code = MRC_RDMA_WRITE_ONLY, .exp_hasMeth = true, .exp_rqmsn = 0x0201, .exp_msn = 0x0202, .exp_hasTseth = true, .exp_txTimestamp = 0x0203, .exp_tsr = true, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(56, 0x90)},
        {.name = "STRIP 17 TSETH 65 in", .rqmsn = 0x0204, .msn = 0x0205,
         .tx_timestamp = 0x0206, .tsr = 0, .ftype = 0x1, .payload = buildWritePayload(57, 0x91), .op_code = MRC_RDMA_WRITE_ONLY, .ts = true,
         .exp_op_code = MRC_RDMA_WRITE_ONLY, .exp_hasMeth = true, .exp_rqmsn = 0x0204, .exp_msn = 0x0205, .exp_hasTseth = true, .exp_txTimestamp = 0x0206, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(57, 0x91)},
        {.name = "STRIP 18 TSETH 72 in", .rqmsn = 0x0207, .msn = 0x0208,
         .tx_timestamp = 0x0209, .tsr = 1, .ftype = 0x1, .payload = buildWritePayload(64, 0x92), .op_code = MRC_RDMA_WRITE_ONLY, .ts = true,
         .exp_op_code = MRC_RDMA_WRITE_ONLY, .exp_hasMeth = true, .exp_rqmsn = 0x0207, .exp_msn = 0x0208, .exp_hasTseth = true, .exp_txTimestamp = 0x0209, .exp_tsr = true, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(64, 0x92)},
        {.name = "STRIP 19 TSETH 73 in", .rqmsn = 0x020A, .msn = 0x020B,
         .tx_timestamp = 0x020C, .tsr = 0, .ftype = 0x1, .payload = buildWritePayload(65, 0x93), .op_code = MRC_RDMA_WRITE_ONLY, .ts = true,
         .exp_op_code = MRC_RDMA_WRITE_ONLY, .exp_hasMeth = true, .exp_rqmsn = 0x020A, .exp_msn = 0x020B, .exp_hasTseth = true, .exp_txTimestamp = 0x020C, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(65, 0x93)},

        // nothing after the stripped headers: one empty word with last
        {.name = "STRIP 20 METH only", .rqmsn = 0x0301, .msn = 0x0302,
         .tx_timestamp = 0, .tsr = 0, .ftype = 0, .payload = buildWritePayload(0, 0x00), .op_code = MRC_RDMA_WRITE_MIDDLE, .ts = false,
         .exp_op_code = MRC_RDMA_WRITE_MIDDLE, .exp_hasMeth = true, .exp_rqmsn = 0x0301, .exp_msn = 0x0302, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(0, 0x00)},
        {.name = "STRIP 21 METH + TSETH only", .rqmsn = 0x0303, .msn = 0x0304,
         .tx_timestamp = 0x0305, .tsr = 1, .ftype = 0x1, .payload = buildWritePayload(0, 0x00), .op_code = MRC_RDMA_WRITE_MIDDLE, .ts = true,
         .exp_op_code = MRC_RDMA_WRITE_MIDDLE, .exp_hasMeth = true, .exp_rqmsn = 0x0303, .exp_msn = 0x0304, .exp_hasTseth = true, .exp_txTimestamp = 0x0305, .exp_tsr = true, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(0, 0x00)},

        // not a request: passed through unchanged, meta all zero, ts ignored
        {.name = "STRIP 22 ACK ts", .rqmsn = 0, .msn = 0,
         .tx_timestamp = 0, .tsr = 0, .ftype = 0, .payload = buildWritePayload(4, 0xA0), .op_code = MRC_ACK, .ts = true,
         .exp_op_code = MRC_ACK, .exp_hasMeth = false, .exp_rqmsn = 0, .exp_msn = 0, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(4, 0xA0)},
        {.name = "STRIP 23 SACK", .rqmsn = 0, .msn = 0,
         .tx_timestamp = 0, .tsr = 0, .ftype = 0, .payload = buildWritePayload(36, 0xA1), .op_code = MRC_SACK, .ts = false,
         .exp_op_code = MRC_SACK, .exp_hasMeth = false, .exp_rqmsn = 0, .exp_msn = 0, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(36, 0xA1)},
        {.name = "STRIP 24 RC opcode 300 bytes", .rqmsn = 0, .msn = 0,
         .tx_timestamp = 0, .tsr = 0, .ftype = 0, .payload = buildWritePayload(300, 0xA2), .op_code = (mrcOpCode)0x0A, .ts = true,
         .exp_op_code = (mrcOpCode)0x0A, .exp_hasMeth = false, .exp_rqmsn = 0, .exp_msn = 0, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(300, 0xA2)},
        {.name = "STRIP 25 ACK 64 bytes", .rqmsn = 0, .msn = 0,
         .tx_timestamp = 0, .tsr = 0, .ftype = 0, .payload = buildWritePayload(64, 0xA3), .op_code = MRC_ACK, .ts = false,
         .exp_op_code = MRC_ACK, .exp_hasMeth = false, .exp_rqmsn = 0, .exp_msn = 0, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(64, 0xA3)},

        // back to requests: offsets switch 0 -> 8 -> 4 -> 8 between packets
        {.name = "STRIP 26 TSETH after passthrough", .rqmsn = 0x0401, .msn = 0x0402,
         .tx_timestamp = 0x0403, .tsr = 1, .ftype = 0x1, .payload = buildWritePayload(200, 0xB0), .op_code = MRC_RDMA_WRITE_FIRST, .ts = true,
         .exp_op_code = MRC_RDMA_WRITE_FIRST, .exp_hasMeth = true, .exp_rqmsn = 0x0401, .exp_msn = 0x0402, .exp_hasTseth = true, .exp_txTimestamp = 0x0403, .exp_tsr = true, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(200, 0xB0)},
        {.name = "STRIP 27 METH after TSETH", .rqmsn = 0x0404, .msn = 0x0405,
         .tx_timestamp = 0, .tsr = 0, .ftype = 0, .payload = buildWritePayload(200, 0xB1), .op_code = MRC_RDMA_WRITE_MIDDLE, .ts = false,
         .exp_op_code = MRC_RDMA_WRITE_MIDDLE, .exp_hasMeth = true, .exp_rqmsn = 0x0404, .exp_msn = 0x0405, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(200, 0xB1)},
        {.name = "STRIP 28 TSETH after METH", .rqmsn = 0x0406, .msn = 0x0407,
         .tx_timestamp = 0x0408, .tsr = 0, .ftype = 0x2, .payload = buildWritePayload(201, 0xB2), .op_code = MRC_RDMA_WRITE_LAST, .ts = true,
         .exp_op_code = MRC_RDMA_WRITE_LAST, .exp_hasMeth = true, .exp_rqmsn = 0x0406, .exp_msn = 0x0407, .exp_hasTseth = true, .exp_txTimestamp = 0x0408, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0, .exp_payload = buildWritePayload(201, 0xB2)},
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