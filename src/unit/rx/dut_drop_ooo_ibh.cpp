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
#include "mrc_transport_protocol/rx/drop_ooo_ibh.hpp"

// it receives:
// 1. metaIn    one drop flag per data packet, from rx_ibh_fsm: false = accepted, true = duplicate or out of window
// 2. input     the data packets, starting with the RETH, from rx_process_exh
// it outputs:
// 1. output    every word of the packets with drop = false, unchanged. The words of the packets with drop = true are read and discarded
// one call reads the flag, then one word per call, forwarded or dropped, until last.
// rx_ibh_fsm can decide only after the state table answers, so in the pipeline the words usually arrive before their flag

namespace
{
    #define DROP_OOO_IBH_INSTID 2 // it is a static module (fsm state, counters)!

    struct DropOooIbhTestCase
    {
        // misc
        const char* name;

        // input: written before the calls of this row
        int drop;               // flag written to metaIn: 0 = forward, 1 = drop, -1 = no flag written in this row
        uint16_t length;        // how many bytes are written to input in this row (split in 64-byte words), 0 for no words
        uint8_t seed;           // byte i = seed + i, so that every packet is different
        bool last;              // last on the final word written in this row, false = the packet continues in a later row
        size_t calls;           // how many times to tick the module after the inputs are written, 0 = only queue the inputs

        // expected: what output carries after the calls of this row (it can be a packet written in an earlier row)
        uint16_t exp_length;    // bytes on output, 0 = no word at all
        uint8_t exp_seed;       // first byte on output, byte i must be exp_seed + i
        bool exp_last;          // true = last exactly once, on the final word. false = no word with last
    };

    struct DropOooIbhTestBench
    {
        hls::stream<net_axis<DATA_WIDTH> > input;
        hls::stream<bool>                  metaIn;

        hls::stream<net_axis<DATA_WIDTH> > output;

        void tickOneCycle()
        {
            drop_ooo_ibh<DATA_WIDTH, DROP_OOO_IBH_INSTID>(
                input,
                metaIn,
                output
            );
        }
    };

    bool checkDropOooIbh(DropOooIbhTestBench& tb, const DropOooIbhTestCase& tc)
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
        while (!tb.output.empty()) {
            const net_axis<DATA_WIDTH> word = tb.output.read();
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

    // The module keeps its state between rows, and so do metaIn and input: a row with 0 calls only queues its inputs
    // for the next ones. calls = 1 for the flag + 1 per word, for a forwarded and for a dropped packet alike
    // (DATA_WIDTH 512, 64 bytes per word). Every packet has its own seed, so a word of a dropped packet, or of the
    // next packet, cannot pass as the expected one. Lengths are the bytes after METH / TSETH: RETH 16 [+ ImmDt 4] + payload
    const DropOooIbhTestCase dropOooIbhTestCases[] = {
        // nothing to do: no flag, no words, nothing comes out
        {"DROP 01 no input",                            -1, 0,    0x00, true,  3,  0,    0x00, false},

        // one packet at a time, forwarded and dropped alternately, every size: a dropped packet leaves nothing on
        // output, and the forwarded packet right after it proves the drop ended at last
        {"DROP 02 forward WRITE ONLY 256",              0,  272,  0x10, true,  6,  272,  0x10, true},
        {"DROP 03 drop WRITE ONLY 256",                 1,  272,  0x11, true,  6,  0,    0x00, false},
        {"DROP 04 forward RETH only",                   0,  16,   0x12, true,  2,  16,   0x12, true},
        {"DROP 05 drop RETH + ImmDt only",              1,  20,   0x13, true,  2,  0,    0x00, false},
        {"DROP 06 forward 64 bytes (1 word)",           0,  64,   0x14, true,  2,  64,   0x14, true},
        {"DROP 07 drop 64 bytes (1 word)",              1,  64,   0x15, true,  2,  0,    0x00, false},
        {"DROP 08 forward 65 bytes (2 words)",          0,  65,   0x16, true,  3,  65,   0x16, true},
        {"DROP 09 drop 65 bytes (2 words)",             1,  65,   0x17, true,  3,  0,    0x00, false},
        {"DROP 10 forward WRITE FIRST PMTU",            0,  4112, 0x18, true,  66, 4112, 0x18, true},
        {"DROP 11 drop WRITE LAST IMM PMTU",            1,  4116, 0x19, true,  66, 0,    0x00, false},

        // one word per call when forwarding: 2 calls for a 3-word packet give only the first word,
        // the other two follow without a new flag (0x80 = 0x40 + 64, the same packet)
        {"DROP 12 forward 192 bytes, 2 calls",          0,  192,  0x40, true,  2,  64,   0x40, false},
        {"DROP 13 forward remaining 2 words",           -1, 0,    0x00, true,  2,  128,  0x80, true},
        // one word per call when dropping too: after 2 calls the dropped packet still has 2 words in input.
        // The next calls must drop them and must not read the forward flag of the next packet before last
        {"DROP 14 drop 192 bytes, 2 calls",             1,  192,  0x50, true,  2,  0,    0x00, false},
        {"DROP 15 drop remaining 2 words",              0,  64,   0x51, true,  2,  0,    0x00, false},
        {"DROP 16 next packet forwarded",               -1, 0,    0x00, true,  2,  64,   0x51, true},

        // words before their flag (the usual order in the pipeline): without a flag nothing is forwarded or dropped,
        // the words wait. Once dropped, the next packet waits for its own flag again
        {"DROP 17 words, no flag",                      -1, 128,  0x60, true,  4,  0,    0x00, false},
        {"DROP 18 drop flag arrives",                   1,  0,    0x00, true,  3,  0,    0x00, false},
        {"DROP 19 next words, no flag",                 -1, 64,   0x61, true,  3,  0,    0x00, false},
        {"DROP 20 forward flag arrives",                0,  0,    0x00, true,  2,  64,   0x61, true},

        // flag before its words and gaps inside a forwarded packet: the forward decision holds until last,
        // a drop flag queued in the middle of the packet is not read before last, then it drops the next packet.
        // 0x70 -> 0xB0 -> 0xF0 is one packet of 138 bytes written in three rows
        {"DROP 21 forward flag, no words",              0,  0,    0x00, true,  2,  0,    0x00, false},
        {"DROP 22 first word",                          -1, 64,   0x70, false, 1,  64,   0x70, false},
        {"DROP 23 gap",                                 -1, 0,    0x00, true,  3,  0,    0x00, false},
        {"DROP 24 drop flag queued, second word",       1,  64,   0xB0, false, 1,  64,   0xB0, false},
        {"DROP 25 last word",                           -1, 10,   0xF0, true,  1,  10,   0xF0, true},
        {"DROP 26 queued drop flag, next packet",       -1, 100,  0x71, true,  3,  0,    0x00, false},
        // the same with a dropped packet: the drop decision holds across the gap and the queued forward flag
        {"DROP 27 drop flag, no words",                 1,  0,    0x00, true,  2,  0,    0x00, false},
        {"DROP 28 first word dropped",                  -1, 64,   0x72, false, 1,  0,    0x00, false},
        {"DROP 29 gap",                                 -1, 0,    0x00, true,  3,  0,    0x00, false},
        {"DROP 30 forward flag queued, second word",    0,  64,   0xB2, false, 1,  0,    0x00, false},
        {"DROP 31 last word dropped",                   -1, 10,   0xF2, true,  1,  0,    0x00, false},
        {"DROP 32 queued forward flag, next packet",    -1, 100,  0x73, true,  3,  100,  0x73, true},

        // back to back packets, all queued, every pair of decisions (forward -> drop -> drop -> forward -> drop -> forward):
        // each flag applies to its packet only, the words of a dropped packet never reach output
        {"DROP 33 queue: forward A",                    0,  128,  0x80, true,  0,  0,    0x00, false},
        {"DROP 34 queue: drop B",                       1,  64,   0x05, true,  0,  0,    0x00, false},
        {"DROP 35 queue: drop C",                       1,  192,  0x90, true,  0,  0,    0x00, false},
        {"DROP 36 queue: forward D",                    0,  64,   0x95, true,  0,  0,    0x00, false},
        {"DROP 37 queue: drop E",                       1,  16,   0xA5, true,  0,  0,    0x00, false},
        {"DROP 38 queue: forward F",                    0,  65,   0xAA, true,  0,  0,    0x00, false},
        {"DROP 39 queue: A out",                        -1, 0,    0x00, true,  3,  128,  0x80, true},
        {"DROP 40 queue: B dropped",                    -1, 0,    0x00, true,  2,  0,    0x00, false},
        {"DROP 41 queue: C dropped",                    -1, 0,    0x00, true,  4,  0,    0x00, false},
        {"DROP 42 queue: D out",                        -1, 0,    0x00, true,  2,  64,   0x95, true},
        {"DROP 43 queue: E dropped",                    -1, 0,    0x00, true,  2,  0,    0x00, false},
        {"DROP 44 queue: F out",                        -1, 0,    0x00, true,  3,  65,   0xAA, true},

        // CURRENT BEHAVIOUR: flags and packets are paired only by order. rx_ibh_fsm writes one flag per data packet,
        // so this cannot happen today, but if they ever get out of step every later packet gets the decision
        // of another one: duplicates reach memory and accepted packets are lost. The third row of each case
        // puts the pairing back for the next rows.
        // a flag without packet: the next packet (a duplicate) is forwarded, and its drop flag drops the packet after
        {"DROP 45 orphan forward flag",                 0,  0,    0x00, true,  2,  0,    0x00, false},
        {"DROP 46 duplicate forwarded",                 1,  64,   0xC0, true,  2,  64,   0xC0, true},
        {"DROP 47 next packet dropped",                 -1, 64,   0xC1, true,  2,  0,    0x00, false},
        // a packet without flag: it waits, then takes the drop flag of the next packet, which takes the one after
        {"DROP 48 orphan packet waits",                 -1, 64,   0xD0, true,  2,  0,    0x00, false},
        {"DROP 49 orphan dropped with next flag",       1,  64,   0xD1, true,  2,  0,    0x00, false},
        {"DROP 50 duplicate forwarded one flag late",   0,  0,    0x00, true,  2,  64,   0xD1, true},
        // a forwarded packet without last: the next packet (a duplicate) is forwarded as its tail
        {"DROP 51 forward, no last",                    0,  64,   0xE0, false, 2,  64,   0xE0, false},
        {"DROP 52 duplicate forwarded as the tail",     1,  64,   0xE1, true,  1,  64,   0xE1, true},
        {"DROP 53 next packet dropped",                 -1, 64,   0xE2, true,  2,  0,    0x00, false},
        // a dropped packet without last: the next packet (accepted) is dropped as its tail
        {"DROP 54 drop, no last",                       1,  64,   0xE3, false, 2,  0,    0x00, false},
        {"DROP 55 accepted dropped as the tail",        0,  64,   0xE4, true,  1,  0,    0x00, false},
        {"DROP 56 next packet forwarded",               -1, 64,   0xE5, true,  2,  64,   0xE5, true},
    };
} // namespace

int runDropOooIbhTest()
{
    DropOooIbhTestBench tb;

    int failures = 0;
    const size_t testCaseCount = sizeof(dropOooIbhTestCases) / sizeof(dropOooIbhTestCases[0]);
    for (size_t tcIndex = 0; tcIndex < testCaseCount; ++tcIndex) {
        const DropOooIbhTestCase& tc = dropOooIbhTestCases[tcIndex];
        std::cout << "\n" << tc.name << "\n" << std::endl;

        if (tc.drop >= 0) tb.metaIn.write(tc.drop);

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
        failures += !checkDropOooIbh(tb, tc);
    }

    // every flag and every word has been used by the end
    if (!tb.metaIn.empty() || !tb.input.empty()) {
        std::cerr << "[FAIL DROP] inputs not consumed at the end" << std::endl;
        ++failures;
    }

    return failures;
}