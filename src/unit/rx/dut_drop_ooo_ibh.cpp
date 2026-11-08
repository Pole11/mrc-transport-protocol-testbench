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
        {.name = "DROP 01 no input", .drop = -1, .length = 0, .seed = 0x00, .last = true, .calls = 3,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},

        // one packet at a time, forwarded and dropped alternately, every size: a dropped packet leaves nothing on
        // output, and the forwarded packet right after it proves the drop ended at last
        {.name = "DROP 02 forward WRITE ONLY 256", .drop = 0, .length = 272, .seed = 0x10, .last = true, .calls = 6,
         .exp_length = 272, .exp_seed = 0x10, .exp_last = true},
        {.name = "DROP 03 drop WRITE ONLY 256", .drop = 1, .length = 272, .seed = 0x11, .last = true, .calls = 6,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 04 forward RETH only", .drop = 0, .length = 16, .seed = 0x12, .last = true, .calls = 2,
         .exp_length = 16, .exp_seed = 0x12, .exp_last = true},
        {.name = "DROP 05 drop RETH + ImmDt only", .drop = 1, .length = 20, .seed = 0x13, .last = true, .calls = 2,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 06 forward 64 bytes (1 word)", .drop = 0, .length = 64, .seed = 0x14, .last = true, .calls = 2,
         .exp_length = 64, .exp_seed = 0x14, .exp_last = true},
        {.name = "DROP 07 drop 64 bytes (1 word)", .drop = 1, .length = 64, .seed = 0x15, .last = true, .calls = 2,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 08 forward 65 bytes (2 words)", .drop = 0, .length = 65, .seed = 0x16, .last = true, .calls = 3,
         .exp_length = 65, .exp_seed = 0x16, .exp_last = true},
        {.name = "DROP 09 drop 65 bytes (2 words)", .drop = 1, .length = 65, .seed = 0x17, .last = true, .calls = 3,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 10 forward WRITE FIRST PMTU", .drop = 0, .length = 4112, .seed = 0x18, .last = true, .calls = 66,
         .exp_length = 4112, .exp_seed = 0x18, .exp_last = true},
        {.name = "DROP 11 drop WRITE LAST IMM PMTU", .drop = 1, .length = 4116, .seed = 0x19, .last = true, .calls = 66,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},

        // one word per call when forwarding: 2 calls for a 3-word packet give only the first word,
        // the other two follow without a new flag (0x80 = 0x40 + 64, the same packet)
        {.name = "DROP 12 forward 192 bytes, 2 calls", .drop = 0, .length = 192, .seed = 0x40, .last = true, .calls = 2,
         .exp_length = 64, .exp_seed = 0x40, .exp_last = false},
        {.name = "DROP 13 forward remaining 2 words", .drop = -1, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_length = 128, .exp_seed = 0x80, .exp_last = true},
        // one word per call when dropping too: after 2 calls the dropped packet still has 2 words in input.
        // The next calls must drop them and must not read the forward flag of the next packet before last
        {.name = "DROP 14 drop 192 bytes, 2 calls", .drop = 1, .length = 192, .seed = 0x50, .last = true, .calls = 2,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 15 drop remaining 2 words", .drop = 0, .length = 64, .seed = 0x51, .last = true, .calls = 2,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 16 next packet forwarded", .drop = -1, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_length = 64, .exp_seed = 0x51, .exp_last = true},

        // words before their flag (the usual order in the pipeline): without a flag nothing is forwarded or dropped,
        // the words wait. Once dropped, the next packet waits for its own flag again
        {.name = "DROP 17 words, no flag", .drop = -1, .length = 128, .seed = 0x60, .last = true, .calls = 4,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 18 drop flag arrives", .drop = 1, .length = 0, .seed = 0x00, .last = true, .calls = 3,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 19 next words, no flag", .drop = -1, .length = 64, .seed = 0x61, .last = true, .calls = 3,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 20 forward flag arrives", .drop = 0, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_length = 64, .exp_seed = 0x61, .exp_last = true},

        // flag before its words and gaps inside a forwarded packet: the forward decision holds until last,
        // a drop flag queued in the middle of the packet is not read before last, then it drops the next packet.
        // 0x70 -> 0xB0 -> 0xF0 is one packet of 138 bytes written in three rows
        {.name = "DROP 21 forward flag, no words", .drop = 0, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 22 first word", .drop = -1, .length = 64, .seed = 0x70, .last = false, .calls = 1,
         .exp_length = 64, .exp_seed = 0x70, .exp_last = false},
        {.name = "DROP 23 gap", .drop = -1, .length = 0, .seed = 0x00, .last = true, .calls = 3,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 24 drop flag queued, second word", .drop = 1, .length = 64, .seed = 0xB0, .last = false, .calls = 1,
         .exp_length = 64, .exp_seed = 0xB0, .exp_last = false},
        {.name = "DROP 25 last word", .drop = -1, .length = 10, .seed = 0xF0, .last = true, .calls = 1,
         .exp_length = 10, .exp_seed = 0xF0, .exp_last = true},
        {.name = "DROP 26 queued drop flag, next packet", .drop = -1, .length = 100, .seed = 0x71, .last = true, .calls = 3,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        // the same with a dropped packet: the drop decision holds across the gap and the queued forward flag
        {.name = "DROP 27 drop flag, no words", .drop = 1, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 28 first word dropped", .drop = -1, .length = 64, .seed = 0x72, .last = false, .calls = 1,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 29 gap", .drop = -1, .length = 0, .seed = 0x00, .last = true, .calls = 3,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 30 forward flag queued, second word", .drop = 0, .length = 64, .seed = 0xB2, .last = false, .calls = 1,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 31 last word dropped", .drop = -1, .length = 10, .seed = 0xF2, .last = true, .calls = 1,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 32 queued forward flag, next packet", .drop = -1, .length = 100, .seed = 0x73, .last = true, .calls = 3,
         .exp_length = 100, .exp_seed = 0x73, .exp_last = true},

        // back to back packets, all queued, every pair of decisions (forward -> drop -> drop -> forward -> drop -> forward):
        // each flag applies to its packet only, the words of a dropped packet never reach output
        {.name = "DROP 33 queue: forward A", .drop = 0, .length = 128, .seed = 0x80, .last = true, .calls = 0,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 34 queue: drop B", .drop = 1, .length = 64, .seed = 0x05, .last = true, .calls = 0,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 35 queue: drop C", .drop = 1, .length = 192, .seed = 0x90, .last = true, .calls = 0,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 36 queue: forward D", .drop = 0, .length = 64, .seed = 0x95, .last = true, .calls = 0,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 37 queue: drop E", .drop = 1, .length = 16, .seed = 0xA5, .last = true, .calls = 0,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 38 queue: forward F", .drop = 0, .length = 65, .seed = 0xAA, .last = true, .calls = 0,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 39 queue: A out", .drop = -1, .length = 0, .seed = 0x00, .last = true, .calls = 3,
         .exp_length = 128, .exp_seed = 0x80, .exp_last = true},
        {.name = "DROP 40 queue: B dropped", .drop = -1, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 41 queue: C dropped", .drop = -1, .length = 0, .seed = 0x00, .last = true, .calls = 4,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 42 queue: D out", .drop = -1, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_length = 64, .exp_seed = 0x95, .exp_last = true},
        {.name = "DROP 43 queue: E dropped", .drop = -1, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 44 queue: F out", .drop = -1, .length = 0, .seed = 0x00, .last = true, .calls = 3,
         .exp_length = 65, .exp_seed = 0xAA, .exp_last = true},

        // CURRENT BEHAVIOUR: flags and packets are paired only by order. rx_ibh_fsm writes one flag per data packet,
        // so this cannot happen today, but if they ever get out of step every later packet gets the decision
        // of another one: duplicates reach memory and accepted packets are lost. The third row of each case
        // puts the pairing back for the next rows.
        // a flag without packet: the next packet (a duplicate) is forwarded, and its drop flag drops the packet after
        {.name = "DROP 45 orphan forward flag", .drop = 0, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 46 duplicate forwarded", .drop = 1, .length = 64, .seed = 0xC0, .last = true, .calls = 2,
         .exp_length = 64, .exp_seed = 0xC0, .exp_last = true},
        {.name = "DROP 47 next packet dropped", .drop = -1, .length = 64, .seed = 0xC1, .last = true, .calls = 2,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        // a packet without flag: it waits, then takes the drop flag of the next packet, which takes the one after
        {.name = "DROP 48 orphan packet waits", .drop = -1, .length = 64, .seed = 0xD0, .last = true, .calls = 2,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 49 orphan dropped with next flag", .drop = 1, .length = 64, .seed = 0xD1, .last = true, .calls = 2,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 50 duplicate forwarded one flag late", .drop = 0, .length = 0, .seed = 0x00, .last = true, .calls = 2,
         .exp_length = 64, .exp_seed = 0xD1, .exp_last = true},
        // a forwarded packet without last: the next packet (a duplicate) is forwarded as its tail
        {.name = "DROP 51 forward, no last", .drop = 0, .length = 64, .seed = 0xE0, .last = false, .calls = 2,
         .exp_length = 64, .exp_seed = 0xE0, .exp_last = false},
        {.name = "DROP 52 duplicate forwarded as the tail", .drop = 1, .length = 64, .seed = 0xE1, .last = true, .calls = 1,
         .exp_length = 64, .exp_seed = 0xE1, .exp_last = true},
        {.name = "DROP 53 next packet dropped", .drop = -1, .length = 64, .seed = 0xE2, .last = true, .calls = 2,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        // a dropped packet without last: the next packet (accepted) is dropped as its tail
        {.name = "DROP 54 drop, no last", .drop = 1, .length = 64, .seed = 0xE3, .last = false, .calls = 2,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 55 accepted dropped as the tail", .drop = 0, .length = 64, .seed = 0xE4, .last = true, .calls = 1,
         .exp_length = 0, .exp_seed = 0x00, .exp_last = false},
        {.name = "DROP 56 next packet forwarded", .drop = -1, .length = 64, .seed = 0xE5, .last = true, .calls = 2,
         .exp_length = 64, .exp_seed = 0xE5, .exp_last = true},
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