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
#include "mrc_transport_protocol/rx/rx_exh_payload.hpp"

// it receives:
// 1. metaIn                    one pkgSplit (op_code) per packet, read before its first word
// 2. input                     the packet, starting with the RETH
// it outputs every word unchanged, the whole packet to the output its op_code selects:
// 1. rx_exh2rethImmShiftFifo   WRITE_LAST_IMM, WRITE_ONLY_IMM (RETH + ImmDt, 20 bytes to remove)
// 2. rx_exh2rethShiftFifo      WRITE_FIRST, WRITE_MIDDLE, WRITE_LAST, WRITE_ONLY (RETH, 16 bytes to remove)
// 3. rx_exhNoShiftFifo         any other op_code
// one call reads the pkgSplit, then one word per call

namespace
{
    #define RX_EXH_PAYLOAD_INSTID 2 // it is a static module!

    struct ExhPayloadTestCase
    {
        // misc
        const char* name;

        // input: written before the calls of this row
        int meta;               // op_code written to metaIn, -1 for none
        uint16_t length;        // bytes written to input, 0 for no words
        uint8_t seed;           // byte i = seed + i
        bool last;              // last on the final word
        size_t calls;

        // expected: what the outputs carry after the calls, all of it on exp_fifo, nothing on the other two
        pkgShiftType exp_fifo;  // SHIFT_RETH_IMM, SHIFT_RETH, SHIFT_NONE: the output feeding that shifter (ignored if exp_length = 0)
        uint16_t exp_length;
        uint8_t exp_seed;
        bool exp_last;
    };

    struct RxExhPayloadTestBench
    {
        hls::stream<pkgSplit>              metaIn;
        hls::stream<net_axis<DATA_WIDTH> > input;

        hls::stream<net_axis<DATA_WIDTH> > rx_exh2rethShiftFifo;
        hls::stream<net_axis<DATA_WIDTH> > rx_exh2rethImmShiftFifo;
        hls::stream<net_axis<DATA_WIDTH> > rx_exhNoShiftFifo;

        void tickOneCycle()
        {
            rx_exh_payload<DATA_WIDTH, RX_EXH_PAYLOAD_INSTID>(
                metaIn,
                input,
                rx_exh2rethShiftFifo,
                rx_exh2rethImmShiftFifo,
                rx_exhNoShiftFifo
            );
        }
    };

    bool checkExhPayload(RxExhPayloadTestBench& tb, const ExhPayloadTestCase& tc)
    {
        bool returnValue = true;
        auto fail = [&](const std::string& msg) {
            std::cerr << "[FAIL " << tc.name << "] " << msg << std::endl;
            returnValue = false;
        };

        // each output must carry exactly expLength bytes (none for the unused ones), in whole words, last once on the final word if expLast
        auto checkOutput = [&](hls::stream<net_axis<DATA_WIDTH> >& out, const std::string& outName, uint16_t expLength, bool expLast) {
            std::vector<uint8_t> bytes;
            size_t words = 0, lasts = 0;
            bool wordAfterLast = false;
            while (!out.empty()) {
                const net_axis<DATA_WIDTH> word = out.read();
                if (lasts) wordAfterLast = true;
                ++words;
                for (size_t byte = 0; byte < DATA_WIDTH / 8; ++byte)
                    if (word.keep[byte]) bytes.push_back(static_cast<uint8_t>(word.data(byte * 8 + 7, byte * 8).to_uint()));
                lasts += word.last;
            }

            const size_t expWords = (expLength + DATA_WIDTH / 8 - 1) / (DATA_WIDTH / 8);
            if (words != expWords)      fail(outName + ": expected " + std::to_string(expWords) + " word(s), got " + std::to_string(words));
            if (lasts != expLast)       fail(outName + ": last asserted " + std::to_string(lasts) + " time(s), expected " + std::to_string(expLast));
            if (wordAfterLast)          fail(outName + ": words after last");
            if (bytes.size() != expLength) {
                fail(outName + ": expected " + std::to_string(expLength) + " byte(s), got " + std::to_string(bytes.size()));
                return;
            }
            for (size_t i = 0; i < bytes.size(); ++i)
                if (bytes[i] != static_cast<uint8_t>(tc.exp_seed + i)) {
                    fail(outName + ": first differing byte at offset " + std::to_string(i) + ": got " + std::to_string(bytes[i]) + ", expected " + std::to_string(static_cast<uint8_t>(tc.exp_seed + i)));
                    break;
                }
        };
        checkOutput(tb.rx_exh2rethImmShiftFifo, "rethImmShift", tc.exp_fifo == SHIFT_RETH_IMM ? tc.exp_length : 0, tc.exp_fifo == SHIFT_RETH_IMM && tc.exp_last);
        checkOutput(tb.rx_exh2rethShiftFifo,    "rethShift",    tc.exp_fifo == SHIFT_RETH     ? tc.exp_length : 0, tc.exp_fifo == SHIFT_RETH     && tc.exp_last);
        checkOutput(tb.rx_exhNoShiftFifo,       "noShift",      tc.exp_fifo == SHIFT_NONE     ? tc.exp_length : 0, tc.exp_fifo == SHIFT_NONE     && tc.exp_last);

        return returnValue;
    }

    // The module keeps its state between rows, a row with 0 calls only queues its inputs for the next ones.
    // calls = 1 for the pkgSplit + 1 per word (DATA_WIDTH 512, 64 bytes per word). Every packet has its own seed,
    // so words routed with the wrong pkgSplit, or taken from the next packet, cannot pass as the expected ones.
    // Lengths are the bytes after METH / TSETH: RETH 16 [+ ImmDt 4] + payload
    const ExhPayloadTestCase exhPayloadTestCases[] = {
        // nothing to do
        {"EXH PAYLOAD 01 no input",                     -1,                      0,    0x00, true,  3,  SHIFT_NONE,     0,    0x00, false},

        // every write opcode: the two IMM ones to the RETH + ImmDt shifter, the others to the RETH shifter
        {"EXH PAYLOAD 02 WRITE ONLY 256",               MRC_RDMA_WRITE_ONLY,     272,  0x10, true,  6,  SHIFT_RETH,     272,  0x10, true},
        {"EXH PAYLOAD 03 WRITE FIRST PMTU",             MRC_RDMA_WRITE_FIRST,    4112, 0x11, true,  66, SHIFT_RETH,     4112, 0x11, true},
        {"EXH PAYLOAD 04 WRITE MIDDLE PMTU",            MRC_RDMA_WRITE_MIDDLE,   4112, 0x12, true,  66, SHIFT_RETH,     4112, 0x12, true},
        {"EXH PAYLOAD 05 WRITE LAST 48",                MRC_RDMA_WRITE_LAST,     64,   0x13, true,  2,  SHIFT_RETH,     64,   0x13, true},
        {"EXH PAYLOAD 06 WRITE ONLY IMM 300",           MRC_RDMA_WRITE_ONLY_IMM, 320,  0x14, true,  6,  SHIFT_RETH_IMM, 320,  0x14, true},
        {"EXH PAYLOAD 07 WRITE LAST IMM PMTU",          MRC_RDMA_WRITE_LAST_IMM, 4116, 0x15, true,  66, SHIFT_RETH_IMM, 4116, 0x15, true},

        // any other op_code goes to the no shift output (rx_exh_fsm never sends them, control packets leave at rx_split_ctrl)
        {"EXH PAYLOAD 08 ACK",                          MRC_ACK,                 4,    0x20, true,  2,  SHIFT_NONE,     4,    0x20, true},
        {"EXH PAYLOAD 09 SACK",                         MRC_SACK,                36,   0x21, true,  2,  SHIFT_NONE,     36,   0x21, true},
        {"EXH PAYLOAD 10 NACK",                         MRC_NACK,                20,   0x22, true,  2,  SHIFT_NONE,     20,   0x22, true},
        {"EXH PAYLOAD 11 PROBE REQ",                    MRC_PROBE_REQ,           16,   0x23, true,  2,  SHIFT_NONE,     16,   0x23, true},
        {"EXH PAYLOAD 12 EP REQ",                       MRC_EP_REQ,              16,   0x24, true,  2,  SHIFT_NONE,     16,   0x24, true},
        {"EXH PAYLOAD 13 EP RSP",                       MRC_EP_RSP,              36,   0x25, true,  2,  SHIFT_NONE,     36,   0x25, true},
        {"EXH PAYLOAD 14 RC WRITE ONLY opcode 0x0A",    0x0A,                    300,  0x26, true,  6,  SHIFT_NONE,     300,  0x26, true},
        {"EXH PAYLOAD 15 opcode 0xFF",                  0xFF,                    65,   0x27, true,  3,  SHIFT_NONE,     65,   0x27, true},

        // smallest packets (headers only) and sizes around the word boundary
        {"EXH PAYLOAD 16 WRITE ONLY RETH only",         MRC_RDMA_WRITE_ONLY,     16,   0x30, true,  2,  SHIFT_RETH,     16,   0x30, true},
        {"EXH PAYLOAD 17 WRITE ONLY IMM RETH+ImmDt only", MRC_RDMA_WRITE_ONLY_IMM, 20, 0x31, true,  2,  SHIFT_RETH_IMM, 20,   0x31, true},
        {"EXH PAYLOAD 18 WRITE LAST 65 bytes",          MRC_RDMA_WRITE_LAST,     65,   0x32, true,  3,  SHIFT_RETH,     65,   0x32, true},
        {"EXH PAYLOAD 19 WRITE LAST IMM 128 bytes",     MRC_RDMA_WRITE_LAST_IMM, 128,  0x33, true,  3,  SHIFT_RETH_IMM, 128,  0x33, true},

        // one word per call: after 2 calls only the first word is out, the rest follows without a new pkgSplit
        {"EXH PAYLOAD 20 WRITE FIRST 192 bytes, 2 calls", MRC_RDMA_WRITE_FIRST,  192,  0x40, true,  2,  SHIFT_RETH,     64,   0x40, false},
        {"EXH PAYLOAD 21 remaining 2 words",            -1,                      0,    0x00, true,  2,  SHIFT_RETH,     128,  0x80, true},

        // words before their pkgSplit: they wait, nothing is forwarded without a pkgSplit
        {"EXH PAYLOAD 22 words, no pkgSplit",           -1,                      128,  0x50, true,  4,  SHIFT_NONE,     0,    0x00, false},
        {"EXH PAYLOAD 23 ONLY IMM pkgSplit arrives",    MRC_RDMA_WRITE_ONLY_IMM, 0,    0x00, true,  3,  SHIFT_RETH_IMM, 128,  0x50, true},

        // pkgSplit before its words, gaps inside the packet: the pkgSplit read at the start holds until last,
        // a pkgSplit queued in the middle of the packet is not read before last
        {"EXH PAYLOAD 24 MIDDLE pkgSplit, no words",    MRC_RDMA_WRITE_MIDDLE,   0,    0x00, true,  2,  SHIFT_NONE,     0,    0x00, false},
        {"EXH PAYLOAD 25 first word",                   -1,                      64,   0x60, false, 1,  SHIFT_RETH,     64,   0x60, false},
        {"EXH PAYLOAD 26 gap",                          -1,                      0,    0x00, true,  3,  SHIFT_NONE,     0,    0x00, false},
        {"EXH PAYLOAD 27 ONLY IMM pkgSplit queued",     MRC_RDMA_WRITE_ONLY_IMM, 64,   0xA0, false, 1,  SHIFT_RETH,     64,   0xA0, false},
        {"EXH PAYLOAD 28 last word",                    -1,                      10,   0xE0, true,  1,  SHIFT_RETH,     10,   0xE0, true},
        {"EXH PAYLOAD 29 ONLY IMM packet",              -1,                      100,  0x61, true,  3,  SHIFT_RETH_IMM, 100,  0x61, true},

        // back to back packets, all queued: last ends each packet, the next pkgSplit routes the next one
        {"EXH PAYLOAD 30 queue: LAST IMM packet A",     MRC_RDMA_WRITE_LAST_IMM, 128,  0x80, true,  0,  SHIFT_NONE,     0,    0x00, false},
        {"EXH PAYLOAD 31 queue: LAST packet B",         MRC_RDMA_WRITE_LAST,     64,   0x05, true,  0,  SHIFT_NONE,     0,    0x00, false},
        {"EXH PAYLOAD 32 queue: SACK packet C",         MRC_SACK,                36,   0x90, true,  0,  SHIFT_NONE,     0,    0x00, false},
        {"EXH PAYLOAD 33 queue: ONLY IMM packet D",     MRC_RDMA_WRITE_ONLY_IMM, 64,   0x95, true,  0,  SHIFT_NONE,     0,    0x00, false},
        {"EXH PAYLOAD 34 queue: A out",                 -1,                      0,    0x00, true,  3,  SHIFT_RETH_IMM, 128,  0x80, true},
        {"EXH PAYLOAD 35 queue: B out",                 -1,                      0,    0x00, true,  2,  SHIFT_RETH,     64,   0x05, true},
        {"EXH PAYLOAD 36 queue: C out",                 -1,                      0,    0x00, true,  2,  SHIFT_NONE,     36,   0x90, true},
        {"EXH PAYLOAD 37 queue: D out",                 -1,                      0,    0x00, true,  2,  SHIFT_RETH_IMM, 64,   0x95, true},

        // CURRENT BEHAVIOUR: pkgSplits and packets are paired only by order. A pkgSplit without packet
        // (rx_exh_fsm writes it, drop_ooo_ibh drops the packet) routes the next packet with the wrong op_code,
        // and so on for every later packet. The third row of each case puts the pairing back for the next rows
        {"EXH PAYLOAD 38 orphan ONLY IMM pkgSplit",     MRC_RDMA_WRITE_ONLY_IMM, 0,    0x00, true,  2,  SHIFT_NONE,     0,    0x00, false},
        {"EXH PAYLOAD 39 ONLY packet, IMM output",      MRC_RDMA_WRITE_ONLY,     64,   0xB0, true,  2,  SHIFT_RETH_IMM, 64,   0xB0, true},
        {"EXH PAYLOAD 40 next packet takes ONLY",       -1,                      64,   0xB1, true,  2,  SHIFT_RETH,     64,   0xB1, true},
        // a packet without pkgSplit waits, then takes the pkgSplit of the next packet
        {"EXH PAYLOAD 41 orphan packet",                -1,                      64,   0xC0, true,  2,  SHIFT_NONE,     0,    0x00, false},
        {"EXH PAYLOAD 42 LAST IMM pkgSplit, orphan out",MRC_RDMA_WRITE_LAST_IMM, 64,   0xC1, true,  2,  SHIFT_RETH_IMM, 64,   0xC0, true},
        {"EXH PAYLOAD 43 LAST pkgSplit, previous out",  MRC_RDMA_WRITE_LAST,     0,    0x00, true,  2,  SHIFT_RETH,     64,   0xC1, true},
        // a packet without last swallows the next packet into the old output
        {"EXH PAYLOAD 44 FIRST packet without last",    MRC_RDMA_WRITE_FIRST,    64,   0xD0, false, 2,  SHIFT_RETH,     64,   0xD0, false},
        {"EXH PAYLOAD 45 ONLY IMM packet, RETH output", MRC_RDMA_WRITE_ONLY_IMM, 64,   0xD1, true,  1,  SHIFT_RETH,     64,   0xD1, true},
        {"EXH PAYLOAD 46 next packet takes ONLY IMM",   -1,                      64,   0xD2, true,  2,  SHIFT_RETH_IMM, 64,   0xD2, true},
    };
} // namespace

int runRxExhPayloadTest()
{
    RxExhPayloadTestBench tb;

    int failures = 0;
    const size_t testCaseCount = sizeof(exhPayloadTestCases) / sizeof(exhPayloadTestCases[0]);
    for (size_t tcIndex = 0; tcIndex < testCaseCount; ++tcIndex) {
        const ExhPayloadTestCase& tc = exhPayloadTestCases[tcIndex];
        std::cout << "\n" << tc.name << "\n" << std::endl;

        if (tc.meta >= 0) tb.metaIn.write(pkgSplit((mrcOpCode) tc.meta));

        const std::vector<uint8_t> packet = buildWritePayload(tc.length, tc.seed);
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
            wordToSend.last = tc.last && byteIndex + bytesPerWord >= packet.size();

            tb.input.write(wordToSend);
        }

        for (size_t c = 0; c < tc.calls; ++c) tb.tickOneCycle();
        failures += !checkExhPayload(tb, tc);
    }

    // every pkgSplit and every word has been used by the end
    if (!tb.metaIn.empty() || !tb.input.empty()) {
        std::cerr << "[FAIL EXH PAYLOAD] inputs not consumed at the end" << std::endl;
        ++failures;
    }

    return failures;
}
