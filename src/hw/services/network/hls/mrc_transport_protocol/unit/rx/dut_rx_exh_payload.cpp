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
        {.name = "EXH PAYLOAD 01 no input", .meta = -1, .length = 0, .seed = 0x00, .last = true, .calls = 3,
         .exp_fifo = SHIFT_NONE, .exp_length = 0, .exp_seed = 0x00, .exp_last = false},

        // every write opcode: the two IMM ones to the RETH + ImmDt shifter, the others to the RETH shifter
        {.name = "EXH PAYLOAD 02 WRITE ONLY 256", .meta = MRC_RDMA_WRITE_ONLY, .length = 272, .seed = 0x10, .last = true, .calls = 6,
         .exp_fifo = SHIFT_RETH, .exp_length = 272, .exp_seed = 0x10, .exp_last = true},
        {.name = "EXH PAYLOAD 03 WRITE FIRST PMTU", .meta = MRC_RDMA_WRITE_FIRST, .length = 4112, .seed = 0x11, .last = true, .calls = 66,
         .exp_fifo = SHIFT_RETH, .exp_length = 4112, .exp_seed = 0x11, .exp_last = true},
        {.name = "EXH PAYLOAD 04 WRITE MIDDLE PMTU", .meta = MRC_RDMA_WRITE_MIDDLE, .length = 4112, .seed = 0x12, .last = true, .calls = 66,
         .exp_fifo = SHIFT_RETH, .exp_length = 4112, .exp_seed = 0x12, .exp_last = true},
        {.name = "EXH PAYLOAD 05 WRITE LAST 48", .meta = MRC_RDMA_WRITE_LAST, .length = 64, .seed = 0x13, .last = true, .calls = 2,
         .exp_fifo = SHIFT_RETH, .exp_length = 64, .exp_seed = 0x13, .exp_last = true},
        {.name = "EXH PAYLOAD 06 WRITE ONLY IMM 300", .meta = MRC_RDMA_WRITE_ONLY_IMM, .length = 320, .seed = 0x14, .last = true, .calls = 6,
         .exp_fifo = SHIFT_RETH_IMM, .exp_length = 320, .exp_seed = 0x14, .exp_last = true},
        {.name = "EXH PAYLOAD 07 WRITE LAST IMM PMTU", .meta = MRC_RDMA_WRITE_LAST_IMM, .length = 4116, .seed = 0x15, .last = true, .calls = 66,
         .exp_fifo = SHIFT_RETH_IMM, .exp_length = 4116, .exp_seed = 0x15, .exp_last = true},

        // any other op_code goes to the no shift output (rx_exh_fsm never sends them, control packets leave at rx_split_ctrl)
        {.name = "EXH PAYLOAD 08 ACK", .meta = MRC_ACK, .length = 4, .seed = 0x20, .last = true, .calls = 2,
         .exp_fifo = SHIFT_NONE, .exp_length = 4, .exp_seed = 0x20, .exp_last = true},
        {.name = "EXH PAYLOAD 09 SACK", .meta = MRC_SACK, .length = 36, .seed = 0x21, .last = true, .calls = 2,
         .exp_fifo = SHIFT_NONE, .exp_length = 36, .exp_seed = 0x21, .exp_last = true},
        {.name = "EXH PAYLOAD 10 NACK", .meta = MRC_NACK, .length = 20, .seed = 0x22, .last = true, .calls = 2,
         .exp_fifo = SHIFT_NONE, .exp_length = 20, .exp_seed = 0x22, .exp_last = true},
        {.name = "EXH PAYLOAD 11 PROBE REQ", .meta = MRC_PROBE_REQ, .length = 16, .seed = 0x23, .last = true, .calls = 2,
         .exp_fifo = SHIFT_NONE, .exp_length = 16, .exp_seed = 0x23, .exp_last = true},
        {.name = "EXH PAYLOAD 12 EP REQ", .meta = MRC_EP_REQ, .length = 16, .seed = 0x24, .last = true, .calls = 2,
         .exp_fifo = SHIFT_NONE, .exp_length = 16, .exp_seed = 0x24, .exp_last = true},
        {.name = "EXH PAYLOAD 13 EP RSP", .meta = MRC_EP_RSP, .length = 36, .seed = 0x25, .last = true, .calls = 2,
         .exp_fifo = SHIFT_NONE, .exp_length = 36, .exp_seed = 0x25, .exp_last = true},
        {.name = "EXH PAYLOAD 14 RC WRITE ONLY opcode 0x0A", .meta = 0x0A, .length = 300, .seed = 0x26, .last = true, .calls = 6,
         .exp_fifo = SHIFT_NONE, .exp_length = 300, .exp_seed = 0x26, .exp_last = true},
        {.name = "EXH PAYLOAD 15 opcode 0xFF", .meta = 0xFF, .length = 65, .seed = 0x27, .last = true, .calls = 3,
         .exp_fifo = SHIFT_NONE, .exp_length = 65, .exp_seed = 0x27, .exp_last = true},

        // smallest packets (headers only) and sizes around the word boundary
        {.name = "EXH PAYLOAD 16 WRITE ONLY RETH only", .meta = MRC_RDMA_WRITE_ONLY, .length = 16, .seed = 0x30, .last = true, .calls = 2,
         .exp_fifo = SHIFT_RETH, .exp_length = 16, .exp_seed = 0x30, .exp_last = true},
        {.name = "EXH PAYLOAD 17 WRITE ONLY IMM RETH+ImmDt only", .meta = MRC_RDMA_WRITE_ONLY_IMM, .length = 20, .seed = 0x31, .last = true, .calls = 2,
         .exp_fifo = SHIFT_RETH_IMM, .exp_length = 20, .exp_seed = 0x31, .exp_last = true},
        {.name = "EXH PAYLOAD 18 WRITE LAST 65 bytes", .meta = MRC_RDMA_WRITE_LAST, .length = 65, .seed = 0x32, .last = true, .calls = 3,
         .exp_fifo = SHIFT_RETH, .exp_length = 65, .exp_seed = 0x32, .exp_last = true},
        {.name = "EXH PAYLOAD 19 WRITE LAST IMM 128 bytes", .meta = MRC_RDMA_WRITE_LAST_IMM, .length = 128, .seed = 0x33, .last = true, .calls = 3,
         .exp_fifo = SHIFT_RETH_IMM, .exp_length = 128, .exp_seed = 0x33, .exp_last = true},

        // one word per call: after 2 calls only the first word is out, the rest follows without a new pkgSplit
        {.name = "EXH PAYLOAD 20 WRITE FIRST 192 bytes, 2 calls", .meta = MRC_RDMA_WRITE_FIRST, .length = 192, .seed = 0x40, .last = true, .calls = 2,
         .exp_fifo = SHIFT_RETH, .exp_length = 64, .exp_seed = 0x40, .exp_last = false},
        {.name = "EXH PAYLOAD 21 remaining 2 words", .meta = -1, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_fifo = SHIFT_RETH, .exp_length = 128, .exp_seed = 0x80, .exp_last = true},

        // words before their pkgSplit: they wait, nothing is forwarded without a pkgSplit
        {.name = "EXH PAYLOAD 22 words, no pkgSplit", .meta = -1, .length = 128, .seed = 0x50, .last = true, .calls = 4,
         .exp_fifo = SHIFT_NONE, .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "EXH PAYLOAD 23 ONLY IMM pkgSplit arrives", .meta = MRC_RDMA_WRITE_ONLY_IMM, .length = 0, .seed = 0x00, .last = true, .calls = 3,
         .exp_fifo = SHIFT_RETH_IMM, .exp_length = 128, .exp_seed = 0x50, .exp_last = true},

        // pkgSplit before its words, gaps inside the packet: the pkgSplit read at the start holds until last,
        // a pkgSplit queued in the middle of the packet is not read before last
        {.name = "EXH PAYLOAD 24 MIDDLE pkgSplit, no words", .meta = MRC_RDMA_WRITE_MIDDLE, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_fifo = SHIFT_NONE, .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "EXH PAYLOAD 25 first word", .meta = -1, .length = 64, .seed = 0x60, .last = false, .calls = 1,
         .exp_fifo = SHIFT_RETH, .exp_length = 64, .exp_seed = 0x60, .exp_last = false},
        {.name = "EXH PAYLOAD 26 gap", .meta = -1, .length = 0, .seed = 0x00, .last = true, .calls = 3,
         .exp_fifo = SHIFT_NONE, .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "EXH PAYLOAD 27 ONLY IMM pkgSplit queued", .meta = MRC_RDMA_WRITE_ONLY_IMM, .length = 64, .seed = 0xA0, .last = false, .calls = 1,
         .exp_fifo = SHIFT_RETH, .exp_length = 64, .exp_seed = 0xA0, .exp_last = false},
        {.name = "EXH PAYLOAD 28 last word", .meta = -1, .length = 10, .seed = 0xE0, .last = true, .calls = 1,
         .exp_fifo = SHIFT_RETH, .exp_length = 10, .exp_seed = 0xE0, .exp_last = true},
        {.name = "EXH PAYLOAD 29 ONLY IMM packet", .meta = -1, .length = 100, .seed = 0x61, .last = true, .calls = 3,
         .exp_fifo = SHIFT_RETH_IMM, .exp_length = 100, .exp_seed = 0x61, .exp_last = true},

        // back to back packets, all queued: last ends each packet, the next pkgSplit routes the next one
        {.name = "EXH PAYLOAD 30 queue: LAST IMM packet A", .meta = MRC_RDMA_WRITE_LAST_IMM, .length = 128, .seed = 0x80, .last = true, .calls = 0,
         .exp_fifo = SHIFT_NONE, .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "EXH PAYLOAD 31 queue: LAST packet B", .meta = MRC_RDMA_WRITE_LAST, .length = 64, .seed = 0x05, .last = true, .calls = 0,
         .exp_fifo = SHIFT_NONE, .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "EXH PAYLOAD 32 queue: SACK packet C", .meta = MRC_SACK, .length = 36, .seed = 0x90, .last = true, .calls = 0,
         .exp_fifo = SHIFT_NONE, .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "EXH PAYLOAD 33 queue: ONLY IMM packet D", .meta = MRC_RDMA_WRITE_ONLY_IMM, .length = 64, .seed = 0x95, .last = true, .calls = 0,
         .exp_fifo = SHIFT_NONE, .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "EXH PAYLOAD 34 queue: A out", .meta = -1, .length = 0, .seed = 0x00, .last = true, .calls = 3,
         .exp_fifo = SHIFT_RETH_IMM, .exp_length = 128, .exp_seed = 0x80, .exp_last = true},
        {.name = "EXH PAYLOAD 35 queue: B out", .meta = -1, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_fifo = SHIFT_RETH, .exp_length = 64, .exp_seed = 0x05, .exp_last = true},
        {.name = "EXH PAYLOAD 36 queue: C out", .meta = -1, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_fifo = SHIFT_NONE, .exp_length = 36, .exp_seed = 0x90, .exp_last = true},
        {.name = "EXH PAYLOAD 37 queue: D out", .meta = -1, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_fifo = SHIFT_RETH_IMM, .exp_length = 64, .exp_seed = 0x95, .exp_last = true},

        // CURRENT BEHAVIOUR: pkgSplits and packets are paired only by order. A pkgSplit without packet
        // (rx_exh_fsm writes it, drop_ooo_ibh drops the packet) routes the next packet with the wrong op_code,
        // and so on for every later packet. The third row of each case puts the pairing back for the next rows
        {.name = "EXH PAYLOAD 38 orphan ONLY IMM pkgSplit", .meta = MRC_RDMA_WRITE_ONLY_IMM, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_fifo = SHIFT_NONE, .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "EXH PAYLOAD 39 ONLY packet, IMM output", .meta = MRC_RDMA_WRITE_ONLY, .length = 64, .seed = 0xB0, .last = true, .calls = 2,
         .exp_fifo = SHIFT_RETH_IMM, .exp_length = 64, .exp_seed = 0xB0, .exp_last = true},
        {.name = "EXH PAYLOAD 40 next packet takes ONLY", .meta = -1, .length = 64, .seed = 0xB1, .last = true, .calls = 2,
         .exp_fifo = SHIFT_RETH, .exp_length = 64, .exp_seed = 0xB1, .exp_last = true},
        // a packet without pkgSplit waits, then takes the pkgSplit of the next packet
        {.name = "EXH PAYLOAD 41 orphan packet", .meta = -1, .length = 64, .seed = 0xC0, .last = true, .calls = 2,
         .exp_fifo = SHIFT_NONE, .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "EXH PAYLOAD 42 LAST IMM pkgSplit, orphan out", .meta = MRC_RDMA_WRITE_LAST_IMM, .length = 64, .seed = 0xC1, .last = true, .calls = 2,
         .exp_fifo = SHIFT_RETH_IMM, .exp_length = 64, .exp_seed = 0xC0, .exp_last = true},
        {.name = "EXH PAYLOAD 43 LAST pkgSplit, previous out", .meta = MRC_RDMA_WRITE_LAST, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_fifo = SHIFT_RETH, .exp_length = 64, .exp_seed = 0xC1, .exp_last = true},
        // a packet without last swallows the next packet into the old output
        {.name = "EXH PAYLOAD 44 FIRST packet without last", .meta = MRC_RDMA_WRITE_FIRST, .length = 64, .seed = 0xD0, .last = false, .calls = 2,
         .exp_fifo = SHIFT_RETH, .exp_length = 64, .exp_seed = 0xD0, .exp_last = false},
        {.name = "EXH PAYLOAD 45 ONLY IMM packet, RETH output", .meta = MRC_RDMA_WRITE_ONLY_IMM, .length = 64, .seed = 0xD1, .last = true, .calls = 1,
         .exp_fifo = SHIFT_RETH, .exp_length = 64, .exp_seed = 0xD1, .exp_last = true},
        {.name = "EXH PAYLOAD 46 next packet takes ONLY IMM", .meta = -1, .length = 64, .seed = 0xD2, .last = true, .calls = 2,
         .exp_fifo = SHIFT_RETH_IMM, .exp_length = 64, .exp_seed = 0xD2, .exp_last = true},
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
