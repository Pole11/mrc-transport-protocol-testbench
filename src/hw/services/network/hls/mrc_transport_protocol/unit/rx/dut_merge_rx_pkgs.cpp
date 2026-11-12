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
#include "mrc_transport_protocol/rx/merge_rx_pkgs.hpp"

// it receives:
// 1. rx_pkgShiftTypeFifo           one pkgShift per packet, its type selects the input to forward (qpn is not used)
// 2. rx_rethSift2mergerFifo        packets after the RETH shift (SHIFT_RETH)
// 3. rx_rethImmSift2mergerFifo     packets after the RETH + ImmDt shift (SHIFT_RETH_IMM)
// 4. rx_NoSift2mergerFifo          packets that need no shift (SHIFT_NONE)
// it outputs:
// 1. m_axis_mem_write_data         every word of the selected input, unchanged, until last. Then the next pkgShift
// one call reads the pkgShift, then one word per call

namespace
{
    #define MERGE_RX_PKGS_INSTID 2 // it is a static module!

    struct MergeRxPkgsTestCase
    {
        // misc
        const char* name;

        // input: written before the calls of this row
        int shift;              // pkgShiftType written to rx_pkgShiftTypeFifo, -1 for none (0/1/2 are SHIFT_RETH/SHIFT_NONE/SHIFT_RETH_IMM; 3 is the malformed value)
        pkgShiftType fifo;      // the words go to the input this type selects (Which input gets this row’s words: SHIFT_RETH -> rx_rethSift2mergerFifo, SHIFT_RETH_IMM -> rx_rethImmSift2mergerFifo, SHIFT_NONE -> rx_NoSift2mergerFifo.)
        uint16_t length;        // how many bytes written to fifo, 0 for no words
        uint8_t seed;           // byte i = seed + i so that every packet is different
        bool last;              // last on the final word
        size_t calls;           // how many times to tick the module

        // expected: what m_axis_mem_write_data carries after the calls
        uint16_t exp_length;
        uint8_t exp_seed;
        bool exp_last;
    };

    struct MergeRxPkgsTestBench
    {
        hls::stream<pkgShift>              rx_pkgShiftTypeFifo;
        hls::stream<net_axis<DATA_WIDTH> > rx_rethSift2mergerFifo;
        hls::stream<net_axis<DATA_WIDTH> > rx_rethImmSift2mergerFifo;
        hls::stream<net_axis<DATA_WIDTH> > rx_NoSift2mergerFifo;

        hls::stream<net_axis<DATA_WIDTH> > m_axis_mem_write_data;

        void tickOneCycle()
        {
            merge_rx_pkgs<DATA_WIDTH, MERGE_RX_PKGS_INSTID>(
                rx_pkgShiftTypeFifo,
                rx_rethSift2mergerFifo,
                rx_rethImmSift2mergerFifo,
                rx_NoSift2mergerFifo,
                m_axis_mem_write_data
            );
        }
    };

    bool checkMergeRxPkgs(MergeRxPkgsTestBench& tb, const MergeRxPkgsTestCase& tc)
    {
        bool returnValue = true;
        auto fail = [&](const std::string& msg) {
            std::cerr << "[FAIL " << tc.name << "] " << msg << std::endl;
            returnValue = false;
        };

        // exactly exp_length bytes in whole words, last once on the final word (never if !exp_last)
        std::vector<uint8_t> bytes;
        size_t words = 0, lasts = 0;
        bool wordAfterLast = false;
        while (!tb.m_axis_mem_write_data.empty()) {
            const net_axis<DATA_WIDTH> word = tb.m_axis_mem_write_data.read();
            if (lasts) wordAfterLast = true;
            ++words;
            for (size_t byte = 0; byte < DATA_WIDTH / 8; ++byte)
                if (word.keep[byte]) bytes.push_back(static_cast<uint8_t>(word.data(byte * 8 + 7, byte * 8).to_uint()));
            lasts += word.last;
        }

        const size_t expWords = (tc.exp_length + DATA_WIDTH / 8 - 1) / (DATA_WIDTH / 8);
        if (words != expWords)      fail("expected " + std::to_string(expWords) + " word(s), got " + std::to_string(words));
        if (lasts != tc.exp_last)   fail("last asserted " + std::to_string(lasts) + " time(s), expected " + std::to_string(tc.exp_last));
        if (wordAfterLast)          fail("words after last");
        if (bytes.size() != tc.exp_length) {
            fail("expected " + std::to_string(tc.exp_length) + " byte(s), got " + std::to_string(bytes.size()));
            return returnValue;
        }
        for (size_t i = 0; i < bytes.size(); ++i)
            if (bytes[i] != static_cast<uint8_t>(tc.exp_seed + i)) {
                fail("first differing byte at offset " + std::to_string(i) + ": got " + std::to_string(bytes[i]) + ", expected " + std::to_string(static_cast<uint8_t>(tc.exp_seed + i)));
                break;
            }

        return returnValue;
    }

    // The merger keeps its state between rows, a row with 0 calls only queues its inputs for the next ones.
    // calls = 1 for the pkgShift + 1 per word (DATA_WIDTH 512, 64 bytes per word). Every packet has its own seed,
    // so words taken from the wrong input, or from the next packet, cannot pass as the expected ones
    const MergeRxPkgsTestCase mergeRxPkgsTestCases[] = {
        // nothing to do
        {.name = "MERGE 01 no input", .shift = -1, .fifo = SHIFT_RETH, .length = 0, .seed = 0x00, .last = true, .calls = 3,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},

        // every type forwards its own input (the cast to the state relies on the two enums being aligned)
        {.name = "MERGE 02 RETH 1 byte", .shift = SHIFT_RETH, .fifo = SHIFT_RETH, .length = 1, .seed = 0x10, .last = true, .calls = 2,
         .exp_length = 1, .exp_seed = 0x10, .exp_last = true},
        {.name = "MERGE 03 RETH_IMM 1 byte", .shift = SHIFT_RETH_IMM, .fifo = SHIFT_RETH_IMM, .length = 1, .seed = 0x11, .last = true, .calls = 2,
         .exp_length = 1, .exp_seed = 0x11, .exp_last = true},
        {.name = "MERGE 04 NONE 1 byte", .shift = SHIFT_NONE, .fifo = SHIFT_NONE, .length = 1, .seed = 0x12, .last = true, .calls = 2,
         .exp_length = 1, .exp_seed = 0x12, .exp_last = true},

        // sizes around the word boundary and long packets: the input is held for every word until last
        {.name = "MERGE 05 RETH 64 bytes", .shift = SHIFT_RETH, .fifo = SHIFT_RETH, .length = 64, .seed = 0x20, .last = true, .calls = 2,
         .exp_length = 64, .exp_seed = 0x20, .exp_last = true},
        {.name = "MERGE 06 RETH_IMM 65 bytes", .shift = SHIFT_RETH_IMM, .fifo = SHIFT_RETH_IMM, .length = 65, .seed = 0x21, .last = true, .calls = 3,
         .exp_length = 65, .exp_seed = 0x21, .exp_last = true},
        {.name = "MERGE 07 NONE 128 bytes", .shift = SHIFT_NONE, .fifo = SHIFT_NONE, .length = 128, .seed = 0x22, .last = true, .calls = 3,
         .exp_length = 128, .exp_seed = 0x22, .exp_last = true},
        {.name = "MERGE 08 RETH 256 bytes (WRITE ONLY 256)", .shift = SHIFT_RETH, .fifo = SHIFT_RETH, .length = 256, .seed = 0x30, .last = true, .calls = 5,
         .exp_length = 256, .exp_seed = 0x30, .exp_last = true},
        {.name = "MERGE 09 RETH_IMM PMTU", .shift = SHIFT_RETH_IMM, .fifo = SHIFT_RETH_IMM, .length = 4096, .seed = 0x40, .last = true, .calls = 65,
         .exp_length = 4096, .exp_seed = 0x40, .exp_last = true},
        {.name = "MERGE 10 NONE 1000 bytes", .shift = SHIFT_NONE, .fifo = SHIFT_NONE, .length = 1000, .seed = 0x41, .last = true, .calls = 17,
         .exp_length = 1000, .exp_seed = 0x41, .exp_last = true},

        // one word per call: after 2 calls only the first word is out, the rest follows without a new pkgShift
        {.name = "MERGE 11 RETH 192 bytes, 2 calls", .shift = SHIFT_RETH, .fifo = SHIFT_RETH, .length = 192, .seed = 0x50, .last = true, .calls = 2,
         .exp_length = 64, .exp_seed = 0x50, .exp_last = false},
        {.name = "MERGE 12 RETH remaining 2 words", .shift = -1, .fifo = SHIFT_RETH, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_length = 128, .exp_seed = 0x90, .exp_last = true},

        // words before their pkgShift: they wait, nothing is forwarded without a pkgShift
        {.name = "MERGE 13 RETH words, no pkgShift", .shift = -1, .fifo = SHIFT_RETH, .length = 128, .seed = 0x60, .last = true, .calls = 4,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "MERGE 14 RETH pkgShift arrives", .shift = SHIFT_RETH, .fifo = SHIFT_RETH, .length = 0, .seed = 0x00, .last = true, .calls = 3,
         .exp_length = 128, .exp_seed = 0x60, .exp_last = true},

        // pkgShift before its words: the ready packets on the other inputs are not taken while waiting
        {.name = "MERGE 15 RETH_IMM pkgShift, NONE words", .shift = SHIFT_RETH_IMM, .fifo = SHIFT_NONE, .length = 64, .seed = 0x70, .last = true, .calls = 4,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "MERGE 16 RETH words too", .shift = -1, .fifo = SHIFT_RETH, .length = 64, .seed = 0x71, .last = true, .calls = 3,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        // and a packet in progress is not interleaved: its words arrive with gaps, the others keep waiting
        {.name = "MERGE 17 RETH_IMM first word", .shift = -1, .fifo = SHIFT_RETH_IMM, .length = 64, .seed = 0x72, .last = false, .calls = 1,
         .exp_length = 64, .exp_seed = 0x72, .exp_last = false},
        {.name = "MERGE 18 RETH_IMM gap", .shift = -1, .fifo = SHIFT_RETH_IMM, .length = 0, .seed = 0x00, .last = true, .calls = 3,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "MERGE 19 RETH_IMM last word", .shift = -1, .fifo = SHIFT_RETH_IMM, .length = 10, .seed = 0xB2, .last = true, .calls = 1,
         .exp_length = 10, .exp_seed = 0xB2, .exp_last = true},
        // the order of the pkgShifts decides, not the order of the words
        {.name = "MERGE 20 RETH pkgShift, older NONE waits", .shift = SHIFT_RETH, .fifo = SHIFT_RETH, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_length = 64, .exp_seed = 0x71, .exp_last = true},
        {.name = "MERGE 21 NONE pkgShift", .shift = SHIFT_NONE, .fifo = SHIFT_NONE, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_length = 64, .exp_seed = 0x70, .exp_last = true},

        // last ends the packet even if the next one is already queued on the same input, the next pkgShift decides
        {.name = "MERGE 22 queue: RETH packet A", .shift = -1, .fifo = SHIFT_RETH, .length = 128, .seed = 0x80, .last = true, .calls = 0,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "MERGE 23 queue: RETH packet B", .shift = -1, .fifo = SHIFT_RETH, .length = 64, .seed = 0x05, .last = true, .calls = 0,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "MERGE 24 queue: RETH pkgShift, NONE packet", .shift = SHIFT_RETH, .fifo = SHIFT_NONE, .length = 64, .seed = 0xA0, .last = true, .calls = 0,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "MERGE 25 queue: NONE pkgShift, A out", .shift = SHIFT_NONE, .fifo = SHIFT_NONE, .length = 0, .seed = 0x00, .last = true, .calls = 3,
         .exp_length = 128, .exp_seed = 0x80, .exp_last = true},
        {.name = "MERGE 26 queue: NONE before B", .shift = -1, .fifo = SHIFT_NONE, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_length = 64, .exp_seed = 0xA0, .exp_last = true},
        {.name = "MERGE 27 queue: RETH pkgShift, B out", .shift = SHIFT_RETH, .fifo = SHIFT_RETH, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_length = 64, .exp_seed = 0x05, .exp_last = true},

        // a packet without pkgShift (rx_exh_payload routes it, rx_exh_fsm never writes its pkgShift) blocks no other input
        {.name = "MERGE 28 orphan NONE packet", .shift = -1, .fifo = SHIFT_NONE, .length = 64, .seed = 0xD0, .last = true, .calls = 2,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "MERGE 29 RETH still flows", .shift = SHIFT_RETH, .fifo = SHIFT_RETH, .length = 64, .seed = 0xD1, .last = true, .calls = 2,
         .exp_length = 64, .exp_seed = 0xD1, .exp_last = true},
        {.name = "MERGE 30 RETH_IMM still flows", .shift = SHIFT_RETH_IMM, .fifo = SHIFT_RETH_IMM, .length = 100, .seed = 0xD2, .last = true, .calls = 3,
         .exp_length = 100, .exp_seed = 0xD2, .exp_last = true},
        // CURRENT BEHAVIOUR: the next SHIFT_NONE forwards the orphan, every later NONE packet is one pkgShift late
        {.name = "MERGE 31 NONE pkgShift takes the orphan", .shift = SHIFT_NONE, .fifo = SHIFT_NONE, .length = 64, .seed = 0xE0, .last = true, .calls = 2,
         .exp_length = 64, .exp_seed = 0xD0, .exp_last = true},
        {.name = "MERGE 32 NONE pkgShift takes the new one", .shift = SHIFT_NONE, .fifo = SHIFT_NONE, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_length = 64, .exp_seed = 0xE0, .exp_last = true},

        // malformed: type 3 is outside pkgShiftType (it fits its 2 bits). CURRENT BEHAVIOUR: it is cast to IDLE,
        // the pkgShift is consumed and nothing is forwarded, the words wait for the next pkgShift
        {.name = "MERGE 33 pkgShift type 3", .shift = 3, .fifo = SHIFT_RETH, .length = 64, .seed = 0xF0, .last = true, .calls = 3,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "MERGE 34 RETH pkgShift after type 3", .shift = SHIFT_RETH, .fifo = SHIFT_RETH, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_length = 64, .exp_seed = 0xF0, .exp_last = true},
    };
} // namespace

int runMergeRxPkgsTest()
{
    MergeRxPkgsTestBench tb;

    int failures = 0;
    const size_t testCaseCount = sizeof(mergeRxPkgsTestCases) / sizeof(mergeRxPkgsTestCases[0]);
    for (size_t tcIndex = 0; tcIndex < testCaseCount; ++tcIndex) {
        const MergeRxPkgsTestCase& tc = mergeRxPkgsTestCases[tcIndex];
        std::cout << "\n" << tc.name << "\n" << std::endl;

        if (tc.shift >= 0) tb.rx_pkgShiftTypeFifo.write(pkgShift((pkgShiftType) tc.shift, tcIndex));

        hls::stream<net_axis<DATA_WIDTH> >& input = tc.fifo == SHIFT_RETH     ? tb.rx_rethSift2mergerFifo :
                                                    tc.fifo == SHIFT_RETH_IMM ? tb.rx_rethImmSift2mergerFifo :
                                                                                tb.rx_NoSift2mergerFifo;
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

            input.write(wordToSend);
        }

        for (size_t c = 0; c < tc.calls; ++c) tb.tickOneCycle();
        failures += !checkMergeRxPkgs(tb, tc);
    }

    // every pkgShift and every word has been used by the end
    if (!tb.rx_pkgShiftTypeFifo.empty() || !tb.rx_rethSift2mergerFifo.empty() || !tb.rx_rethImmSift2mergerFifo.empty() || !tb.rx_NoSift2mergerFifo.empty()) {
        std::cerr << "[FAIL MERGE] inputs not consumed at the end" << std::endl;
        ++failures;
    }

    return failures;
}