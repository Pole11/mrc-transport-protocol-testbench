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
#include "mrc_transport_protocol/rx/rx_split_ctrl.hpp"

// this module must output every word of the input packet (the bytes after the BTH), unchanged
// 1. to dataOut if splitIn = False
// 2. to ctrlOut if splitIn = True
// one splitIn per packet, read before its first word

namespace 
{
    #define RX_SPLIT_CTRL_INSTID 2 // it is a static module!

    struct SplitCtrlTestCase 
    {
        // misc
        const char* name;

        // packet after the BTH, simply data!
        uint16_t length;    // bytes
        uint8_t seed;       // byte i = seed + i
        bool splitIn;

        // expected
        uint16_t exp_data_length;
        uint8_t exp_data_seed;
        uint16_t exp_ctrl_length;
        uint8_t exp_ctrl_seed;
    };

    struct RxSplitCtrlTestBench
    {
        hls::stream<net_axis<DATA_WIDTH> > input;
        hls::stream<bool>                  splitIn;

        hls::stream<net_axis<DATA_WIDTH> > dataOut;
        hls::stream<net_axis<DATA_WIDTH> > ctrlOut;

        void tickOneCycle() 
        {
            rx_split_ctrl<DATA_WIDTH, RX_SPLIT_CTRL_INSTID>(
                input,
                splitIn,
                dataOut,
                ctrlOut
            );
        }
    };

    std::vector<uint8_t> buildSplitCtrlPacket(const SplitCtrlTestCase& testCase)
    {
        std::vector<uint8_t> packet(testCase.length);
        for (size_t i = 0; i < packet.size(); ++i) packet[i] = static_cast<uint8_t>(testCase.seed + i);
        return packet;
    }

    bool checkWhichOutputIsUsed(RxSplitCtrlTestBench& tb, const SplitCtrlTestCase& tc) 
    {
        bool returnValue = true;
        auto fail = [&](const std::string& msg) {
            std::cerr << "[FAIL " << tc.name << "] " << msg << std::endl;
            returnValue = false;
        };

        // each output must carry exactly expLength bytes (none for the unused one), in whole words, last on the final word only
        auto checkOutput = [&](hls::stream<net_axis<DATA_WIDTH> >& out, const std::string& outName, uint16_t expLength, uint8_t expSeed) {
            std::vector<uint8_t> bytes;
            size_t words = 0, lasts = 0;
            bool wordAfterLast = false;
            while (!out.empty()) {
                const net_axis<DATA_WIDTH> word = out.read();
                if (lasts) wordAfterLast = true;
                ++words;
                for (size_t bit = 0; bit < DATA_WIDTH / 8; ++bit)
                    if (word.keep[bit]) bytes.push_back(static_cast<uint8_t>(word.data(bit * 8 + 7, bit * 8).to_uint()));
                lasts += word.last;
            }

            const size_t expWords = (expLength + DATA_WIDTH / 8 - 1) / (DATA_WIDTH / 8);
            if (words != expWords)          fail(outName + ": expected " + std::to_string(expWords) + " word(s), got " + std::to_string(words));
            if (expWords && lasts != 1)     fail(outName + ": last asserted " + std::to_string(lasts) + " time(s), expected once");
            if (wordAfterLast)              fail(outName + ": words after last");
            if (bytes.size() != expLength) {
                fail(outName + ": expected " + std::to_string(expLength) + " byte(s), got " + std::to_string(bytes.size()));
                return;
            }
            for (size_t i = 0; i < bytes.size(); ++i)
                if (bytes[i] != static_cast<uint8_t>(expSeed + i)) {
                    fail(outName + ": first differing byte at offset " + std::to_string(i) + ": got " + std::to_string(bytes[i]) + ", expected " + std::to_string(static_cast<uint8_t>(expSeed + i)));
                    break;
                }
        };
        checkOutput(tb.dataOut, "dataOut", tc.exp_data_length, tc.exp_data_seed);
        checkOutput(tb.ctrlOut, "ctrlOut", tc.exp_ctrl_length, tc.exp_ctrl_seed);

        // both inputs consumed
        if (!tb.input.empty())   fail("input words not consumed");
        if (!tb.splitIn.empty()) fail("splitIn flag not consumed");

        // whatever is left must not leak into the next test case
        while (!tb.input.empty())   tb.input.read();
        while (!tb.splitIn.empty()) tb.splitIn.read();

        return returnValue;
    }

    // the unused output expects 0 bytes, i.e. no word at all. Every row has its own seed,
    // so a word left over by a previous row cannot pass as this row's output.
    // Lengths are the bytes after the BTH: data = METH 4 [+ TSETH 4] + RETH 16 [+ ImmDt 4] + payload,
    // control = NETH 20, SETH + CC_STATE 36, PETH 16, ERTH 16, EETH 36 (with DATA_WIDTH 512, 64 bytes per word)
    const SplitCtrlTestCase splitCtrlTestCases[] = {
        // typical packets
        {.name = "SPLIT CTRL 01 data 276 bytes (WRITE ONLY 256)", .length = 276, .seed = 0x10, .splitIn = false,
         .exp_data_length = 276, .exp_data_seed = 0x10, .exp_ctrl_length = 0, .exp_ctrl_seed = 0x00},
        {.name = "SPLIT CTRL 02 ctrl 20 bytes (NACK size)", .length = 20, .seed = 0x20, .splitIn = true,
         .exp_data_length = 0, .exp_data_seed = 0x00, .exp_ctrl_length = 20, .exp_ctrl_seed = 0x20},
        {.name = "SPLIT CTRL 03 ctrl 36 bytes (SACK size)", .length = 36, .seed = 0x30, .splitIn = true,
         .exp_data_length = 0, .exp_data_seed = 0x00, .exp_ctrl_length = 36, .exp_ctrl_seed = 0x30},
        {.name = "SPLIT CTRL 04 ctrl 16 bytes (PROBE size)", .length = 16, .seed = 0x40, .splitIn = true,
         .exp_data_length = 0, .exp_data_seed = 0x00, .exp_ctrl_length = 16, .exp_ctrl_seed = 0x40},
        {.name = "SPLIT CTRL 05 ctrl 16 bytes (EP REQ size)", .length = 16, .seed = 0x50, .splitIn = true,
         .exp_data_length = 0, .exp_data_seed = 0x00, .exp_ctrl_length = 16, .exp_ctrl_seed = 0x50},
        {.name = "SPLIT CTRL 06 ctrl 36 bytes (EP RES size)", .length = 36, .seed = 0x60, .splitIn = true,
         .exp_data_length = 0, .exp_data_seed = 0x00, .exp_ctrl_length = 36, .exp_ctrl_seed = 0x60},
        
        // sizes around the word boundary: 1 byte, exactly one word, one byte into the second word, exactly two words
        {.name = "SPLIT CTRL 07 data 1 byte", .length = 1, .seed = 0x70, .splitIn = false,
         .exp_data_length = 1, .exp_data_seed = 0x70, .exp_ctrl_length = 0, .exp_ctrl_seed = 0x00},
        {.name = "SPLIT CTRL 08 ctrl 1 byte", .length = 1, .seed = 0x71, .splitIn = true,
         .exp_data_length = 0, .exp_data_seed = 0x00, .exp_ctrl_length = 1, .exp_ctrl_seed = 0x71},
        {.name = "SPLIT CTRL 09 data 64 bytes", .length = 64, .seed = 0x72, .splitIn = false,
         .exp_data_length = 64, .exp_data_seed = 0x72, .exp_ctrl_length = 0, .exp_ctrl_seed = 0x00},
        {.name = "SPLIT CTRL 10 ctrl 64 bytes", .length = 64, .seed = 0x73, .splitIn = true,
         .exp_data_length = 0, .exp_data_seed = 0x00, .exp_ctrl_length = 64, .exp_ctrl_seed = 0x73},
        {.name = "SPLIT CTRL 11 data 65 bytes", .length = 65, .seed = 0x74, .splitIn = false,
         .exp_data_length = 65, .exp_data_seed = 0x74, .exp_ctrl_length = 0, .exp_ctrl_seed = 0x00},
        {.name = "SPLIT CTRL 12 ctrl 65 bytes", .length = 65, .seed = 0x75, .splitIn = true,
         .exp_data_length = 0, .exp_data_seed = 0x00, .exp_ctrl_length = 65, .exp_ctrl_seed = 0x75},
        {.name = "SPLIT CTRL 13 data 128 bytes", .length = 128, .seed = 0x76, .splitIn = false,
         .exp_data_length = 128, .exp_data_seed = 0x76, .exp_ctrl_length = 0, .exp_ctrl_seed = 0x00},

        // long packets: the flag holds for every word until last
        {.name = "SPLIT CTRL 14 data FIRST TSETH", .length = 1048, .seed = 0x80, .splitIn = false,
         .exp_data_length = 1048, .exp_data_seed = 0x80, .exp_ctrl_length = 0, .exp_ctrl_seed = 0x00},
        {.name = "SPLIT CTRL 15 ctrl 200 bytes", .length = 200, .seed = 0x90, .splitIn = true,
         .exp_data_length = 0, .exp_data_seed = 0x00, .exp_ctrl_length = 200, .exp_ctrl_seed = 0x90},
        {.name = "SPLIT CTRL 16 data after ctrl", .length = 300, .seed = 0xA0, .splitIn = false,
         .exp_data_length = 300, .exp_data_seed = 0xA0, .exp_ctrl_length = 0, .exp_ctrl_seed = 0x00},

        // the routing only depends on the current packet's flag: two in a row on each side, then alternating
        {.name = "SPLIT CTRL 17 ctrl twice (1)", .length = 20, .seed = 0xB0, .splitIn = true,
         .exp_data_length = 0, .exp_data_seed = 0x00, .exp_ctrl_length = 20, .exp_ctrl_seed = 0xB0},
        {.name = "SPLIT CTRL 18 ctrl twice (2)", .length = 36, .seed = 0xB1, .splitIn = true,
         .exp_data_length = 0, .exp_data_seed = 0x00, .exp_ctrl_length = 36, .exp_ctrl_seed = 0xB1},
        {.name = "SPLIT CTRL 19 data twice (1)", .length = 1044, .seed = 0xC0, .splitIn = false,
         .exp_data_length = 1044, .exp_data_seed = 0xC0, .exp_ctrl_length = 0, .exp_ctrl_seed = 0x00},
        {.name = "SPLIT CTRL 20 data twice (2)", .length = 24, .seed = 0xC1, .splitIn = false,
         .exp_data_length = 24, .exp_data_seed = 0xC1, .exp_ctrl_length = 0, .exp_ctrl_seed = 0x00},
        {.name = "SPLIT CTRL 21 ctrl alternate", .length = 16, .seed = 0xD0, .splitIn = true,
         .exp_data_length = 0, .exp_data_seed = 0x00, .exp_ctrl_length = 16, .exp_ctrl_seed = 0xD0},
        {.name = "SPLIT CTRL 22 data alternate", .length = 120, .seed = 0xD1, .splitIn = false,
         .exp_data_length = 120, .exp_data_seed = 0xD1, .exp_ctrl_length = 0, .exp_ctrl_seed = 0x00},
        {.name = "SPLIT CTRL 23 ctrl alternate", .length = 36, .seed = 0xD2, .splitIn = true,
         .exp_data_length = 0, .exp_data_seed = 0x00, .exp_ctrl_length = 36, .exp_ctrl_seed = 0xD2},
    };
} // namespace

int runSplitCtrlTest() 
{
    RxSplitCtrlTestBench tb;

    int failures = 0;
    const size_t testCaseCount = sizeof(splitCtrlTestCases) / sizeof(splitCtrlTestCases[0]);
    for (size_t tcIndex = 0; tcIndex < testCaseCount; ++tcIndex) {
        const SplitCtrlTestCase& tc = splitCtrlTestCases[tcIndex];
        std::cout << "\n" << tc.name << "\n" << std::endl;

        const std::vector<uint8_t> packet = buildSplitCtrlPacket(tc);
        tb.splitIn.write(tc.splitIn);

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

        for (size_t c = 0; c < wordCount + 2; ++c) tb.tickOneCycle(); // one call reads splitIn, then one word per call
        failures += !checkWhichOutputIsUsed(tb, tc);
    }

    return failures;
}