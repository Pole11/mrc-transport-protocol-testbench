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
        {"MERGE 01 no input",                           -1,             SHIFT_RETH,     0,    0x00, true,  3,  0,    0x00, false},

        // every type forwards its own input (the cast to the state relies on the two enums being aligned)
        {"MERGE 02 RETH 1 byte",                        SHIFT_RETH,     SHIFT_RETH,     1,    0x10, true,  2,  1,    0x10, true},
        {"MERGE 03 RETH_IMM 1 byte",                    SHIFT_RETH_IMM, SHIFT_RETH_IMM, 1,    0x11, true,  2,  1,    0x11, true},
        {"MERGE 04 NONE 1 byte",                        SHIFT_NONE,     SHIFT_NONE,     1,    0x12, true,  2,  1,    0x12, true},

        // sizes around the word boundary and long packets: the input is held for every word until last
        {"MERGE 05 RETH 64 bytes",                      SHIFT_RETH,     SHIFT_RETH,     64,   0x20, true,  2,  64,   0x20, true},
        {"MERGE 06 RETH_IMM 65 bytes",                  SHIFT_RETH_IMM, SHIFT_RETH_IMM, 65,   0x21, true,  3,  65,   0x21, true},
        {"MERGE 07 NONE 128 bytes",                     SHIFT_NONE,     SHIFT_NONE,     128,  0x22, true,  3,  128,  0x22, true},
        {"MERGE 08 RETH 256 bytes (WRITE ONLY 256)",    SHIFT_RETH,     SHIFT_RETH,     256,  0x30, true,  5,  256,  0x30, true},
        {"MERGE 09 RETH_IMM PMTU",                      SHIFT_RETH_IMM, SHIFT_RETH_IMM, 4096, 0x40, true,  65, 4096, 0x40, true},
        {"MERGE 10 NONE 1000 bytes",                    SHIFT_NONE,     SHIFT_NONE,     1000, 0x41, true,  17, 1000, 0x41, true},

        // one word per call: after 2 calls only the first word is out, the rest follows without a new pkgShift
        {"MERGE 11 RETH 192 bytes, 2 calls",            SHIFT_RETH,     SHIFT_RETH,     192,  0x50, true,  2,  64,   0x50, false},
        {"MERGE 12 RETH remaining 2 words",             -1,             SHIFT_RETH,     0,    0x00, true,  2,  128,  0x90, true},

        // words before their pkgShift: they wait, nothing is forwarded without a pkgShift
        {"MERGE 13 RETH words, no pkgShift",            -1,             SHIFT_RETH,     128,  0x60, true,  4,  0,    0x00, false},
        {"MERGE 14 RETH pkgShift arrives",              SHIFT_RETH,     SHIFT_RETH,     0,    0x00, true,  3,  128,  0x60, true},

        // pkgShift before its words: the ready packets on the other inputs are not taken while waiting
        {"MERGE 15 RETH_IMM pkgShift, NONE words",      SHIFT_RETH_IMM, SHIFT_NONE,     64,   0x70, true,  4,  0,    0x00, false},
        {"MERGE 16 RETH words too",                     -1,             SHIFT_RETH,     64,   0x71, true,  3,  0,    0x00, false},
        // and a packet in progress is not interleaved: its words arrive with gaps, the others keep waiting
        {"MERGE 17 RETH_IMM first word",                -1,             SHIFT_RETH_IMM, 64,   0x72, false, 1,  64,   0x72, false},
        {"MERGE 18 RETH_IMM gap",                       -1,             SHIFT_RETH_IMM, 0,    0x00, true,  3,  0,    0x00, false},
        {"MERGE 19 RETH_IMM last word",                 -1,             SHIFT_RETH_IMM, 10,   0xB2, true,  1,  10,   0xB2, true},
        // the order of the pkgShifts decides, not the order of the words
        {"MERGE 20 RETH pkgShift, older NONE waits",    SHIFT_RETH,     SHIFT_RETH,     0,    0x00, true,  2,  64,   0x71, true},
        {"MERGE 21 NONE pkgShift",                      SHIFT_NONE,     SHIFT_NONE,     0,    0x00, true,  2,  64,   0x70, true},

        // last ends the packet even if the next one is already queued on the same input, the next pkgShift decides
        {"MERGE 22 queue: RETH packet A",               -1,             SHIFT_RETH,     128,  0x80, true,  0,  0,    0x00, false},
        {"MERGE 23 queue: RETH packet B",               -1,             SHIFT_RETH,     64,   0x05, true,  0,  0,    0x00, false},
        {"MERGE 24 queue: RETH pkgShift, NONE packet",  SHIFT_RETH,     SHIFT_NONE,     64,   0xA0, true,  0,  0,    0x00, false},
        {"MERGE 25 queue: NONE pkgShift, A out",        SHIFT_NONE,     SHIFT_NONE,     0,    0x00, true,  3,  128,  0x80, true},
        {"MERGE 26 queue: NONE before B",               -1,             SHIFT_NONE,     0,    0x00, true,  2,  64,   0xA0, true},
        {"MERGE 27 queue: RETH pkgShift, B out",        SHIFT_RETH,     SHIFT_RETH,     0,    0x00, true,  2,  64,   0x05, true},

        // a packet without pkgShift (rx_exh_payload routes it, rx_exh_fsm never writes its pkgShift) blocks no other input
        {"MERGE 28 orphan NONE packet",                 -1,             SHIFT_NONE,     64,   0xD0, true,  2,  0,    0x00, false},
        {"MERGE 29 RETH still flows",                   SHIFT_RETH,     SHIFT_RETH,     64,   0xD1, true,  2,  64,   0xD1, true},
        {"MERGE 30 RETH_IMM still flows",               SHIFT_RETH_IMM, SHIFT_RETH_IMM, 100,  0xD2, true,  3,  100,  0xD2, true},
        // CURRENT BEHAVIOUR: the next SHIFT_NONE forwards the orphan, every later NONE packet is one pkgShift late
        {"MERGE 31 NONE pkgShift takes the orphan",     SHIFT_NONE,     SHIFT_NONE,     64,   0xE0, true,  2,  64,   0xD0, true},
        {"MERGE 32 NONE pkgShift takes the new one",    SHIFT_NONE,     SHIFT_NONE,     0,    0x00, true,  2,  64,   0xE0, true},

        // malformed: type 3 is outside pkgShiftType (it fits its 2 bits). CURRENT BEHAVIOUR: it is cast to IDLE,
        // the pkgShift is consumed and nothing is forwarded, the words wait for the next pkgShift
        {"MERGE 33 pkgShift type 3",                    3,              SHIFT_RETH,     64,   0xF0, true,  3,  0,    0x00, false},
        {"MERGE 34 RETH pkgShift after type 3",         SHIFT_RETH,     SHIFT_RETH,     0,    0x00, true,  2,  64,   0xF0, true},
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