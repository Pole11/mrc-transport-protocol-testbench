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
#include "mrc_transport_protocol/rx/rx_process_exh.hpp"

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
        {.name = "EXH 01 WRITE ONLY", .op_code = MRC_RDMA_WRITE_ONLY, .hasMeth = true, .rqmsn = 0x0001, .msn = 0x0002, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .mrcExt_immDt = 0,
         .va = 0x0000000000001000ULL, .r_key = 0x1234, .dmalen = 256, .packet_immDt = 0, .payload = buildWritePayload(256, 0x10),
         .exp_hasReth = true, .exp_va = 0x0000000000001000ULL, .exp_r_key = 0x1234, .exp_dmalen = 256, .exp_hasMeth = true, .exp_rqmsn = 0x0001, .exp_msn = 0x0002, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0},
        {.name = "EXH 02 WRITE FIRST ts", .op_code = MRC_RDMA_WRITE_FIRST, .hasMeth = true, .rqmsn = 0x0003, .msn = 0x0004, .hasTseth = true, .txTimestamp = 0xABCD, .tsr = true, .hasImmDt = false, .mrcExt_immDt = 0,
         .va = 0x0000000000020000ULL, .r_key = 0x1234, .dmalen = 2100, .packet_immDt = 0, .payload = buildWritePayload(1024, 0x20),
         .exp_hasReth = true, .exp_va = 0x0000000000020000ULL, .exp_r_key = 0x1234, .exp_dmalen = 2100, .exp_hasMeth = true, .exp_rqmsn = 0x0003, .exp_msn = 0x0004, .exp_hasTseth = true, .exp_txTimestamp = 0xABCD, .exp_tsr = true, .exp_hasImmDt = false, .exp_immDt = 0},
        {.name = "EXH 03 WRITE MIDDLE", .op_code = MRC_RDMA_WRITE_MIDDLE, .hasMeth = true, .rqmsn = 0x0003, .msn = 0x0004, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .mrcExt_immDt = 0,
         .va = 0x0000000000020400ULL, .r_key = 0x1234, .dmalen = 2100, .packet_immDt = 0, .payload = buildWritePayload(1024, 0x30),
         .exp_hasReth = true, .exp_va = 0x0000000000020400ULL, .exp_r_key = 0x1234, .exp_dmalen = 2100, .exp_hasMeth = true, .exp_rqmsn = 0x0003, .exp_msn = 0x0004, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0},
        {.name = "EXH 04 WRITE LAST ts", .op_code = MRC_RDMA_WRITE_LAST, .hasMeth = true, .rqmsn = 0x0003, .msn = 0x0005, .hasTseth = true, .txTimestamp = 0x0102, .tsr = false, .hasImmDt = false, .mrcExt_immDt = 0,
         .va = 0x0000000000020800ULL, .r_key = 0x1234, .dmalen = 2100, .packet_immDt = 0, .payload = buildWritePayload(52, 0x40),
         .exp_hasReth = true, .exp_va = 0x0000000000020800ULL, .exp_r_key = 0x1234, .exp_dmalen = 2100, .exp_hasMeth = true, .exp_rqmsn = 0x0003, .exp_msn = 0x0005, .exp_hasTseth = true, .exp_txTimestamp = 0x0102, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0},
        {.name = "EXH 05 WRITE ONLY IMM", .op_code = MRC_RDMA_WRITE_ONLY_IMM, .hasMeth = true, .rqmsn = 0x0006, .msn = 0x0007, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .mrcExt_immDt = 0,
         .va = 0x0000000000030000ULL, .r_key = 0x5678, .dmalen = 300, .packet_immDt = 0xDEADBEEF, .payload = buildWritePayload(300, 0x50),
         .exp_hasReth = true, .exp_va = 0x0000000000030000ULL, .exp_r_key = 0x5678, .exp_dmalen = 300, .exp_hasMeth = true, .exp_rqmsn = 0x0006, .exp_msn = 0x0007, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = true, .exp_immDt = 0xDEADBEEF},
        {.name = "EXH 06 WRITE LAST IMM ts", .op_code = MRC_RDMA_WRITE_LAST_IMM, .hasMeth = true, .rqmsn = 0x0008, .msn = 0x0009, .hasTseth = true, .txTimestamp = 0x0304, .tsr = true, .hasImmDt = false, .mrcExt_immDt = 0,
         .va = 0x0000000000031000ULL, .r_key = 0x5678, .dmalen = 1124, .packet_immDt = 0x01020304, .payload = buildWritePayload(100, 0x60),
         .exp_hasReth = true, .exp_va = 0x0000000000031000ULL, .exp_r_key = 0x5678, .exp_dmalen = 1124, .exp_hasMeth = true, .exp_rqmsn = 0x0008, .exp_msn = 0x0009, .exp_hasTseth = true, .exp_txTimestamp = 0x0304, .exp_tsr = true, .exp_hasImmDt = true, .exp_immDt = 0x01020304},

        // RETH field extremes and byte order: every byte of va / r_key / dmalen is different
        {.name = "EXH 07 RETH all ones", .op_code = MRC_RDMA_WRITE_ONLY, .hasMeth = true, .rqmsn = 0xFFFF, .msn = 0xFFFF, .hasTseth = true, .txTimestamp = 0xFFFF, .tsr = true, .hasImmDt = false, .mrcExt_immDt = 0,
         .va = 0xFFFFFFFFFFFFFFFFULL, .r_key = 0xFFFFFFFF, .dmalen = 0xFFFFFFFF, .packet_immDt = 0, .payload = buildWritePayload(64, 0x70),
         .exp_hasReth = true, .exp_va = 0xFFFFFFFFFFFFFFFFULL, .exp_r_key = 0xFFFFFFFF, .exp_dmalen = 0xFFFFFFFF, .exp_hasMeth = true, .exp_rqmsn = 0xFFFF, .exp_msn = 0xFFFF, .exp_hasTseth = true, .exp_txTimestamp = 0xFFFF, .exp_tsr = true, .exp_hasImmDt = false, .exp_immDt = 0},
        {.name = "EXH 08 RETH distinct bytes", .op_code = MRC_RDMA_WRITE_ONLY_IMM, .hasMeth = true, .rqmsn = 0x1122, .msn = 0x3344, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .mrcExt_immDt = 0,
         .va = 0x0102030405060708ULL, .r_key = 0x090A0B0C, .dmalen = 0x0D0E0F10, .packet_immDt = 0x11121314, .payload = buildWritePayload(64, 0x71),
         .exp_hasReth = true, .exp_va = 0x0102030405060708ULL, .exp_r_key = 0x090A0B0C, .exp_dmalen = 0x0D0E0F10, .exp_hasMeth = true, .exp_rqmsn = 0x1122, .exp_msn = 0x3344, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = true, .exp_immDt = 0x11121314},

        // nothing after the headers: RETH only (16 bytes), RETH + ImmDt only (20 bytes)
        {.name = "EXH 09 RETH only", .op_code = MRC_RDMA_WRITE_ONLY, .hasMeth = true, .rqmsn = 0x000A, .msn = 0x000B, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .mrcExt_immDt = 0,
         .va = 0x0000000000040000ULL, .r_key = 0x9ABC, .dmalen = 0, .packet_immDt = 0, .payload = buildWritePayload(0, 0x00),
         .exp_hasReth = true, .exp_va = 0x0000000000040000ULL, .exp_r_key = 0x9ABC, .exp_dmalen = 0, .exp_hasMeth = true, .exp_rqmsn = 0x000A, .exp_msn = 0x000B, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0},
        {.name = "EXH 10 RETH + ImmDt only", .op_code = MRC_RDMA_WRITE_ONLY_IMM, .hasMeth = true, .rqmsn = 0x000C, .msn = 0x000D, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .mrcExt_immDt = 0,
         .va = 0x0000000000041000ULL, .r_key = 0x9ABC, .dmalen = 0, .packet_immDt = 0xCAFEF00D, .payload = buildWritePayload(0, 0x00),
         .exp_hasReth = true, .exp_va = 0x0000000000041000ULL, .exp_r_key = 0x9ABC, .exp_dmalen = 0, .exp_hasMeth = true, .exp_rqmsn = 0x000C, .exp_msn = 0x000D, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = true, .exp_immDt = 0xCAFEF00D},

        // sizes around the word boundary: 64 and 65 bytes, with and without ImmDt
        {.name = "EXH 11 RETH 64 bytes", .op_code = MRC_RDMA_WRITE_ONLY, .hasMeth = true, .rqmsn = 0x0010, .msn = 0x0011, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .mrcExt_immDt = 0,
         .va = 0x0000000000050000ULL, .r_key = 0x1111, .dmalen = 48, .packet_immDt = 0, .payload = buildWritePayload(48, 0x80),
         .exp_hasReth = true, .exp_va = 0x0000000000050000ULL, .exp_r_key = 0x1111, .exp_dmalen = 48, .exp_hasMeth = true, .exp_rqmsn = 0x0010, .exp_msn = 0x0011, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0},
        {.name = "EXH 12 RETH 65 bytes", .op_code = MRC_RDMA_WRITE_ONLY, .hasMeth = true, .rqmsn = 0x0012, .msn = 0x0013, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .mrcExt_immDt = 0,
         .va = 0x0000000000051000ULL, .r_key = 0x1111, .dmalen = 49, .packet_immDt = 0, .payload = buildWritePayload(49, 0x81),
         .exp_hasReth = true, .exp_va = 0x0000000000051000ULL, .exp_r_key = 0x1111, .exp_dmalen = 49, .exp_hasMeth = true, .exp_rqmsn = 0x0012, .exp_msn = 0x0013, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0},
        {.name = "EXH 13 IMM 64 bytes", .op_code = MRC_RDMA_WRITE_ONLY_IMM, .hasMeth = true, .rqmsn = 0x0014, .msn = 0x0015, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .mrcExt_immDt = 0,
         .va = 0x0000000000052000ULL, .r_key = 0x1111, .dmalen = 44, .packet_immDt = 0x0BADF00D, .payload = buildWritePayload(44, 0x82),
         .exp_hasReth = true, .exp_va = 0x0000000000052000ULL, .exp_r_key = 0x1111, .exp_dmalen = 44, .exp_hasMeth = true, .exp_rqmsn = 0x0014, .exp_msn = 0x0015, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = true, .exp_immDt = 0x0BADF00D},
        {.name = "EXH 14 IMM 65 bytes", .op_code = MRC_RDMA_WRITE_ONLY_IMM, .hasMeth = true, .rqmsn = 0x0016, .msn = 0x0017, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .mrcExt_immDt = 0,
         .va = 0x0000000000053000ULL, .r_key = 0x1111, .dmalen = 45, .packet_immDt = 0x0BADF00E, .payload = buildWritePayload(45, 0x83),
         .exp_hasReth = true, .exp_va = 0x0000000000053000ULL, .exp_r_key = 0x1111, .exp_dmalen = 45, .exp_hasMeth = true, .exp_rqmsn = 0x0016, .exp_msn = 0x0017, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = true, .exp_immDt = 0x0BADF00E},

        // no RETH: NO_HEADER, packet passed through unchanged, MRC fields of metaIn NOT passed through (all zero)
        {.name = "EXH 15 ACK", .op_code = MRC_ACK, .hasMeth = false, .rqmsn = 0x0020, .msn = 0x0021, .hasTseth = true, .txTimestamp = 0x0022, .tsr = true, .hasImmDt = true, .mrcExt_immDt = 0x23242526,
         .va = 0, .r_key = 0, .dmalen = 0, .packet_immDt = 0, .payload = buildWritePayload(4, 0x90),
         .exp_hasReth = false, .exp_va = 0, .exp_r_key = 0, .exp_dmalen = 0, .exp_hasMeth = false, .exp_rqmsn = 0, .exp_msn = 0, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0},
        {.name = "EXH 16 RC opcode 300 bytes", .op_code = (mrcOpCode)0x0A, .hasMeth = false, .rqmsn = 0, .msn = 0, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .mrcExt_immDt = 0,
         .va = 0, .r_key = 0, .dmalen = 0, .packet_immDt = 0, .payload = buildWritePayload(300, 0x91),
         .exp_hasReth = false, .exp_va = 0, .exp_r_key = 0, .exp_dmalen = 0, .exp_hasMeth = false, .exp_rqmsn = 0, .exp_msn = 0, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0},

        // back to RETH packets after NO_HEADER: the header parsers restart cleanly, ImmDt not carried over
        {.name = "EXH 17 IMM after no header", .op_code = MRC_RDMA_WRITE_LAST_IMM, .hasMeth = true, .rqmsn = 0x0030, .msn = 0x0031, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .mrcExt_immDt = 0,
         .va = 0x0000000000060000ULL, .r_key = 0x2222, .dmalen = 2148, .packet_immDt = 0x55667788, .payload = buildWritePayload(100, 0xA0),
         .exp_hasReth = true, .exp_va = 0x0000000000060000ULL, .exp_r_key = 0x2222, .exp_dmalen = 2148, .exp_hasMeth = true, .exp_rqmsn = 0x0030, .exp_msn = 0x0031, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = true, .exp_immDt = 0x55667788},
        {.name = "EXH 18 ONLY after IMM", .op_code = MRC_RDMA_WRITE_ONLY, .hasMeth = true, .rqmsn = 0x0032, .msn = 0x0033, .hasTseth = true, .txTimestamp = 0x0034, .tsr = false, .hasImmDt = false, .mrcExt_immDt = 0,
         .va = 0x0000000000061000ULL, .r_key = 0x2222, .dmalen = 200, .packet_immDt = 0, .payload = buildWritePayload(200, 0xA1),
         .exp_hasReth = true, .exp_va = 0x0000000000061000ULL, .exp_r_key = 0x2222, .exp_dmalen = 200, .exp_hasMeth = true, .exp_rqmsn = 0x0032, .exp_msn = 0x0033, .exp_hasTseth = true, .exp_txTimestamp = 0x0034, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0},

        // ImmDt fields already set in metaIn: an *_IMM opcode overwrites them with the ImmDt of the packet,
        // CURRENT BEHAVIOUR: any other opcode passes them through unchanged
        {.name = "EXH 19 IMM overwrites meta", .op_code = MRC_RDMA_WRITE_ONLY_IMM, .hasMeth = true, .rqmsn = 0x0040, .msn = 0x0041, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = true, .mrcExt_immDt = 0xFFFFFFFF,
         .va = 0x0000000000070000ULL, .r_key = 0x3333, .dmalen = 64, .packet_immDt = 0x0A0B0C0D, .payload = buildWritePayload(64, 0xB0),
         .exp_hasReth = true, .exp_va = 0x0000000000070000ULL, .exp_r_key = 0x3333, .exp_dmalen = 64, .exp_hasMeth = true, .exp_rqmsn = 0x0040, .exp_msn = 0x0041, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = true, .exp_immDt = 0x0A0B0C0D},
        {.name = "EXH 20 IMM meta, plain WRITE", .op_code = MRC_RDMA_WRITE_ONLY, .hasMeth = true, .rqmsn = 0x0042, .msn = 0x0043, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = true, .mrcExt_immDt = 0x12345678,
         .va = 0x0000000000071000ULL, .r_key = 0x3333, .dmalen = 64, .packet_immDt = 0, .payload = buildWritePayload(64, 0xB1),
         .exp_hasReth = true, .exp_va = 0x0000000000071000ULL, .exp_r_key = 0x3333, .exp_dmalen = 64, .exp_hasMeth = true, .exp_rqmsn = 0x0042, .exp_msn = 0x0043, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = true, .exp_immDt = 0x12345678},
        // the state is chosen by hasMeth, not by the opcode: a write without METH goes through NO_HEADER
        {.name = "EXH 21 WRITE without METH", .op_code = MRC_RDMA_WRITE_ONLY, .hasMeth = false, .rqmsn = 0x0050, .msn = 0x0051, .hasTseth = false, .txTimestamp = 0, .tsr = false, .hasImmDt = false, .mrcExt_immDt = 0,
         .va = 0x0000000000072000ULL, .r_key = 0x3333, .dmalen = 48, .packet_immDt = 0, .payload = buildWritePayload(48, 0xB2),
         .exp_hasReth = false, .exp_va = 0, .exp_r_key = 0, .exp_dmalen = 0, .exp_hasMeth = false, .exp_rqmsn = 0, .exp_msn = 0, .exp_hasTseth = false, .exp_txTimestamp = 0, .exp_tsr = false, .exp_hasImmDt = false, .exp_immDt = 0},
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