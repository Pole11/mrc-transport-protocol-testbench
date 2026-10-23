#include <cstdint>
#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "rocev2_config.hpp"
#include "util.hpp"
#include "tests.hpp"

#include "axi_utils.hpp"
#include "mrc_transport_protocol/rx/rx_data_res.hpp"

// it receives:
// 1. input         packets that starts with RETH
// 2. metaIn        op_code, mrcExt(hasMth, rqmsn, msn, hasTseth, txTimestamp, tsr, hasImmDt, immDt)
// it outputs       
// 1. exhMetaFifo   numPkg (do we really need it?)
// 2. metaOut       op_code, RETH fields, hasImmDt*, immDt*
// 3. output        the packet

namespace
{
    #define RX_PROCESS_EXH_INSTID 2 // it is a static module!

    struct ProcessExhTestCase
    {
        // misc
        const char* name;

        // input: exhOpMeta (hasImmDt / immDt are filled by this module, so they are not inputs)
        mrcOpCode op_code;
        bool hasMeth;
        uint16_t rqmsn;
        uint16_t msn;
        bool hasTseth;
        uint16_t txTimestamp;
        bool tsr;
        bool hasImmDt;
        uint32_t mrcExt_immDt;

        // input: packet = [RETH] [ImmDt] payload, RETH and ImmDt only for the opcodes that carry them
        uint64_t va;
        uint32_t r_key;
        uint32_t dmalen;
        uint32_t packet_immDt;
        std::vector<uint8_t> payload;

        // expected: metaOut (RETH fields only checked if exp_hasReth)
        bool exp_hasReth;
        uint64_t exp_va;
        uint32_t exp_r_key;
        uint32_t exp_dmalen;
        bool exp_hasMeth;
        uint16_t exp_rqmsn;
        uint16_t exp_msn;
        bool exp_hasTseth;
        uint16_t exp_txTimestamp;
        bool exp_tsr;
        bool exp_hasImmDt;
        uint32_t exp_immDt;
    };

    struct RxProcessExhTestBench 
    {
        hls::stream<net_axis<DATA_WIDTH> > input;
        hls::stream<exhOpMeta>             metaIn;

        hls::stream<exhMeta>               exhMetaFifo;
        hls::stream<ExHeader<DATA_WIDTH> > metaOut;
        hls::stream<net_axis<DATA_WIDTH> > output;

        void tickOneCycle()
        {
            rx_process_exh<DATA_WIDTH, RX_PROCESS_EXH_INSTID>(
                input,
                metaIn,
                exhMetaFifo,
                metaOut,
                output
            );
        }
    };

    std::vector<uint8_t> buildProcessExhPacket(const ProcessExhTestCase& testCase)
    {
        std::vector<uint8_t> packet;

        // RETH
        if (checkIfRethHeader(testCase.op_code)) {
            appendNetworkOrder64(packet, testCase.va);
            appendNetworkOrder32(packet, testCase.r_key);
            appendNetworkOrder32(packet, testCase.dmalen);
        }

        // ImmDt
        if (checkIfImmDt(testCase.op_code))
            appendNetworkOrder32(packet, testCase.packet_immDt);

        // payload
        packet.insert(packet.end(), testCase.payload.begin(), testCase.payload.end());
        return packet;
    }

    exhOpMeta buildExhOpMeta(const ProcessExhTestCase& testCase)
    {
        exhOpMeta meta;
        meta.op_code            = testCase.op_code;
        meta.mrc.hasMeth        = testCase.hasMeth;
        meta.mrc.rqmsn          = testCase.rqmsn;
        meta.mrc.msn            = testCase.msn;
        meta.mrc.hasTseth       = testCase.hasTseth;
        meta.mrc.txTimestamp    = testCase.txTimestamp;
        meta.mrc.tsr            = testCase.tsr;
        meta.mrc.hasImmDt       = testCase.hasImmDt;
        meta.mrc.immDt          = testCase.mrcExt_immDt;
        return meta;
    }

    bool checkExhProcessed(RxProcessExhTestBench& tb, const ProcessExhTestCase& tc, const std::vector<net_axis<DATA_WIDTH> >& sentWords)
    {
        bool returnValue = true;
        auto fail = [&](const std::string& msg) {
            std::cerr << "[FAIL " << tc.name << "] " << msg << std::endl;
            returnValue = false;
        };
        auto expect = [&](const std::string& field, uint64_t got, uint64_t exp) {
            if (got != exp) fail(field + " 0x" + [&]{ std::ostringstream s; s << std::hex << got << " != 0x" << exp; return s.str(); }());
        };

        // exhMetaFifo: one per packet
        expect("exhMetaFifo count", tb.exhMetaFifo.size(), 1);
        if (!tb.exhMetaFifo.empty()) expect("exhMeta.numPkg", tb.exhMetaFifo.read().numPkg, 1);

        // metaOut: one per packet, RETH fields + MRC extension headers (+ ImmDt parsed here)
        expect("metaOut count", tb.metaOut.size(), 1);
        if (!tb.metaOut.empty()) {
            ExHeader<DATA_WIDTH> m = tb.metaOut.read();
            if (tc.exp_hasReth) {
                RdmaExHeader<DATA_WIDTH> reth = m.getRdmaHeader();
                expect("RETH.va",     reth.getVirtualAddress(), tc.exp_va);
                expect("RETH.r_key",  reth.getRemoteKey(),      tc.exp_r_key);
                expect("RETH.dmalen", reth.getLength(),         tc.exp_dmalen);
            }
            expect("mrc.hasMeth",     m.mrc.hasMeth,     tc.exp_hasMeth);
            expect("mrc.rqmsn",       m.mrc.rqmsn,       tc.exp_rqmsn);
            expect("mrc.msn",         m.mrc.msn,         tc.exp_msn);
            expect("mrc.hasTseth",    m.mrc.hasTseth,    tc.exp_hasTseth);
            expect("mrc.txTimestamp", m.mrc.txTimestamp, tc.exp_txTimestamp);
            expect("mrc.tsr",         m.mrc.tsr,         tc.exp_tsr);
            expect("mrc.hasImmDt",    m.mrc.hasImmDt,    tc.exp_hasImmDt);
            expect("mrc.immDt",       m.mrc.immDt,       tc.exp_immDt);
        }

        // output: every word, unchanged (RETH and ImmDt are removed later, by the shifters)
        expect("output word count", tb.output.size(), sentWords.size());
        for (size_t i = 0; i < sentWords.size() && !tb.output.empty(); ++i) {
            const net_axis<DATA_WIDTH> word = tb.output.read();
            if (word.data != sentWords[i].data) fail("output word " + std::to_string(i) + ": data changed");
            if (word.keep != sentWords[i].keep) fail("output word " + std::to_string(i) + ": keep changed");
            if (word.last != sentWords[i].last) fail("output word " + std::to_string(i) + ": last " + std::to_string(word.last.to_uint()));
        }

        // both inputs consumed
        if (!tb.input.empty())  fail("input words not consumed");
        if (!tb.metaIn.empty()) fail("metaIn not consumed");

        // whatever is left must not leak into the next test case
        while (!tb.exhMetaFifo.empty()) tb.exhMetaFifo.read();
        while (!tb.metaOut.empty())     tb.metaOut.read();
        while (!tb.output.empty())      tb.output.read();
        while (!tb.input.empty())       tb.input.read();
        while (!tb.metaIn.empty())      tb.metaIn.read();

        return returnValue;
    }

    // Requests (every MRC write opcode carries a RETH): the MRC fields of metaIn are passed through, the *_IMM opcodes
    // overwrite hasImmDt / immDt with the ImmDt of the packet. Anything else goes through NO_HEADER: metaOut is an
    // empty ExHeader, MRC fields all zero. rx_strip_mrc_eth always sends hasImmDt false, immDt 0 (rows 01-18)
    const ProcessExhTestCase processExhTestCases[] = {
        // !!! probably I should also test what happens when you send non coherent information, something like MRC_ACK and hasMeth = true
        // every RETH opcode, METH only or METH + TSETH
        {"EXH 01 WRITE ONLY",            MRC_RDMA_WRITE_ONLY,     true, 0x0001, 0x0002, false, 0,      false, false, 0, 0x0000000000001000ULL, 0x1234, 256,  0,          buildWritePayload(256,  0x10), true, 0x0000000000001000ULL, 0x1234, 256,  true,  0x0001, 0x0002, false, 0,      false, false, 0},
        {"EXH 02 WRITE FIRST ts",        MRC_RDMA_WRITE_FIRST,    true, 0x0003, 0x0004, true,  0xABCD, true,  false, 0, 0x0000000000020000ULL, 0x1234, 2100, 0,          buildWritePayload(1024, 0x20), true, 0x0000000000020000ULL, 0x1234, 2100, true,  0x0003, 0x0004, true,  0xABCD, true,  false, 0},
        {"EXH 03 WRITE MIDDLE",          MRC_RDMA_WRITE_MIDDLE,   true, 0x0003, 0x0004, false, 0,      false, false, 0, 0x0000000000020400ULL, 0x1234, 2100, 0,          buildWritePayload(1024, 0x30), true, 0x0000000000020400ULL, 0x1234, 2100, true,  0x0003, 0x0004, false, 0,      false, false, 0},
        {"EXH 04 WRITE LAST ts",         MRC_RDMA_WRITE_LAST,     true, 0x0003, 0x0005, true,  0x0102, false, false, 0, 0x0000000000020800ULL, 0x1234, 2100, 0,          buildWritePayload(52,   0x40), true, 0x0000000000020800ULL, 0x1234, 2100, true,  0x0003, 0x0005, true,  0x0102, false, false, 0},
        {"EXH 05 WRITE ONLY IMM",        MRC_RDMA_WRITE_ONLY_IMM, true, 0x0006, 0x0007, false, 0,      false, false, 0, 0x0000000000030000ULL, 0x5678, 300,  0xDEADBEEF, buildWritePayload(300,  0x50), true, 0x0000000000030000ULL, 0x5678, 300,  true,  0x0006, 0x0007, false, 0,      false, true,  0xDEADBEEF},
        {"EXH 06 WRITE LAST IMM ts",     MRC_RDMA_WRITE_LAST_IMM, true, 0x0008, 0x0009, true,  0x0304, true,  false, 0, 0x0000000000031000ULL, 0x5678, 1124, 0x01020304, buildWritePayload(100,  0x60), true, 0x0000000000031000ULL, 0x5678, 1124, true,  0x0008, 0x0009, true,  0x0304, true,  true,  0x01020304},

        // RETH field extremes and byte order: every byte of va / r_key / dmalen is different
        {"EXH 07 RETH all ones",         MRC_RDMA_WRITE_ONLY,     true, 0xFFFF, 0xFFFF, true,  0xFFFF, true,  false, 0, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFF, 0xFFFFFFFF, 0, buildWritePayload(64,  0x70), true, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFF, 0xFFFFFFFF, true, 0xFFFF, 0xFFFF, true, 0xFFFF, true, false, 0},
        {"EXH 08 RETH distinct bytes",   MRC_RDMA_WRITE_ONLY_IMM, true, 0x1122, 0x3344, false, 0,      false, false, 0, 0x0102030405060708ULL, 0x090A0B0C, 0x0D0E0F10, 0x11121314, buildWritePayload(64, 0x71), true, 0x0102030405060708ULL, 0x090A0B0C, 0x0D0E0F10, true, 0x1122, 0x3344, false, 0, false, true, 0x11121314},

        // nothing after the headers: RETH only (16 bytes), RETH + ImmDt only (20 bytes)
        {"EXH 09 RETH only",             MRC_RDMA_WRITE_ONLY,     true, 0x000A, 0x000B, false, 0,      false, false, 0, 0x0000000000040000ULL, 0x9ABC, 0,    0,          buildWritePayload(0,    0x00), true, 0x0000000000040000ULL, 0x9ABC, 0,    true,  0x000A, 0x000B, false, 0,      false, false, 0},
        {"EXH 10 RETH + ImmDt only",     MRC_RDMA_WRITE_ONLY_IMM, true, 0x000C, 0x000D, false, 0,      false, false, 0, 0x0000000000041000ULL, 0x9ABC, 0,    0xCAFEF00D, buildWritePayload(0,    0x00), true, 0x0000000000041000ULL, 0x9ABC, 0,    true,  0x000C, 0x000D, false, 0,      false, true,  0xCAFEF00D},

        // sizes around the word boundary: 64 and 65 bytes, with and without ImmDt
        {"EXH 11 RETH 64 bytes",         MRC_RDMA_WRITE_ONLY,     true, 0x0010, 0x0011, false, 0,      false, false, 0, 0x0000000000050000ULL, 0x1111, 48,   0,          buildWritePayload(48,   0x80), true, 0x0000000000050000ULL, 0x1111, 48,   true,  0x0010, 0x0011, false, 0,      false, false, 0},
        {"EXH 12 RETH 65 bytes",         MRC_RDMA_WRITE_ONLY,     true, 0x0012, 0x0013, false, 0,      false, false, 0, 0x0000000000051000ULL, 0x1111, 49,   0,          buildWritePayload(49,   0x81), true, 0x0000000000051000ULL, 0x1111, 49,   true,  0x0012, 0x0013, false, 0,      false, false, 0},
        {"EXH 13 IMM 64 bytes",          MRC_RDMA_WRITE_ONLY_IMM, true, 0x0014, 0x0015, false, 0,      false, false, 0, 0x0000000000052000ULL, 0x1111, 44,   0x0BADF00D, buildWritePayload(44,   0x82), true, 0x0000000000052000ULL, 0x1111, 44,   true,  0x0014, 0x0015, false, 0,      false, true,  0x0BADF00D},
        {"EXH 14 IMM 65 bytes",          MRC_RDMA_WRITE_ONLY_IMM, true, 0x0016, 0x0017, false, 0,      false, false, 0, 0x0000000000053000ULL, 0x1111, 45,   0x0BADF00E, buildWritePayload(45,   0x83), true, 0x0000000000053000ULL, 0x1111, 45,   true,  0x0016, 0x0017, false, 0,      false, true,  0x0BADF00E},

        // no RETH: NO_HEADER, packet passed through unchanged, MRC fields of metaIn NOT passed through (all zero)
        {"EXH 15 ACK",                   MRC_ACK,                 false, 0x0020, 0x0021, true,  0x0022, true,  true,  0x23242526, 0, 0, 0, 0,         buildWritePayload(4,    0x90), false, 0, 0, 0, false, 0, 0, false, 0, false, false, 0},
        {"EXH 16 RC opcode 300 bytes",   (mrcOpCode)0x0A,         false, 0,     0,      false, 0,      false, false, 0, 0, 0, 0, 0,                  buildWritePayload(300,  0x91), false, 0, 0, 0, false, 0, 0, false, 0, false, false, 0},

        // back to RETH packets after NO_HEADER: the header parsers restart cleanly, ImmDt not carried over
        {"EXH 17 IMM after no header",   MRC_RDMA_WRITE_LAST_IMM, true, 0x0030, 0x0031, false, 0,      false, false, 0, 0x0000000000060000ULL, 0x2222, 2148, 0x55667788, buildWritePayload(100,  0xA0), true, 0x0000000000060000ULL, 0x2222, 2148, true,  0x0030, 0x0031, false, 0,      false, true,  0x55667788},
        {"EXH 18 ONLY after IMM",        MRC_RDMA_WRITE_ONLY,     true, 0x0032, 0x0033, true,  0x0034, false, false, 0, 0x0000000000061000ULL, 0x2222, 200,  0,          buildWritePayload(200,  0xA1), true, 0x0000000000061000ULL, 0x2222, 200,  true,  0x0032, 0x0033, true,  0x0034, false, false, 0},

        // ImmDt fields already set in metaIn: an *_IMM opcode overwrites them with the ImmDt of the packet,
        // CURRENT BEHAVIOUR: any other opcode passes them through unchanged
        {"EXH 19 IMM overwrites meta",   MRC_RDMA_WRITE_ONLY_IMM, true, 0x0040, 0x0041, false, 0,      false, true,  0xFFFFFFFF, 0x0000000000070000ULL, 0x3333, 64,   0x0A0B0C0D, buildWritePayload(64,   0xB0), true, 0x0000000000070000ULL, 0x3333, 64,   true,  0x0040, 0x0041, false, 0,      false, true,  0x0A0B0C0D},
        {"EXH 20 IMM meta, plain WRITE", MRC_RDMA_WRITE_ONLY,     true, 0x0042, 0x0043, false, 0,      false, true,  0x12345678, 0x0000000000071000ULL, 0x3333, 64,   0,          buildWritePayload(64,   0xB1), true, 0x0000000000071000ULL, 0x3333, 64,   true,  0x0042, 0x0043, false, 0,      false, true,  0x12345678},
        // the state is chosen by hasMeth, not by the opcode: a write without METH goes through NO_HEADER
        {"EXH 21 WRITE without METH",    MRC_RDMA_WRITE_ONLY,     false, 0x0050, 0x0051, false, 0,     false, false, 0, 0x0000000000072000ULL, 0x3333, 48,   0,          buildWritePayload(48,   0xB2), false, 0, 0, 0, false, 0, 0, false, 0, false, false, 0},
    };
} // namespace

int runRxProcessExhTest()
{
    RxProcessExhTestBench tb;

    int failures = 0;
    const size_t testCaseCount = sizeof(processExhTestCases) / sizeof(processExhTestCases[0]);
    for (size_t tcIndex = 0; tcIndex < testCaseCount; ++tcIndex) {
        const ProcessExhTestCase& tc = processExhTestCases[tcIndex];
        std::cout << "\n" << tc.name << "\n" << std::endl;

        const std::vector<uint8_t> packet = buildProcessExhPacket(tc);
        tb.metaIn.write(buildExhOpMeta(tc));

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

        for (size_t c = 0; c < words.size() + 2; ++c) tb.tickOneCycle(); // META, then one word per call
        failures += !checkExhProcessed(tb, tc, words);
    }

    return failures;
}