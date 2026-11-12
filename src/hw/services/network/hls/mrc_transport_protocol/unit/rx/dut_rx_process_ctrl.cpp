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
#include "mrc_transport_protocol/rx/rx_process_ctrl.hpp"

// it receives:
// 1. input         control packets after the BTH (AETH, SETH + CC_STATE, NETH, PETH, ERTH, EETH)
// 2. bthMetaIn     op_code, partition_key, dest_qp, psn, ackReq, rtx
// 3. udpMetaIn     their_address, their_port, my_port, length
// it outputs
// 1. metaOut       BTH fields, udpLength, srcAddress, srcPort, lengthOk, raw (first MRC_CTRL_HDR_SIZE bits of the packet)

namespace
{
    #define RX_PROCESS_CTRL_INSTID 2 // it is a static module!

    struct ProcessCtrlTestCase
    {
        // misc
        const char* name;

        // input: mrcCtrlBthMeta
        mrcOpCode op_code;
        uint16_t partition_key;
        uint32_t dest_qp;   // 24 bits
        uint32_t psn;       // 24 bits
        bool ackReq;
        bool rtx;

        // input: ipUdpMeta
        ap_uint<128> their_address;
        uint16_t their_port;
        uint16_t udp_length;

        // input: packet after the BTH
        std::vector<uint8_t> header;

        // expected (the other metaOut fields are the inputs, raw is the first 36 bytes of the packet)
        bool exp_lengthOk;
    };

    struct RxProcessCtrlTestBench
    {
        hls::stream<net_axis<DATA_WIDTH> >  input;
        hls::stream<mrcCtrlBthMeta>         bthMetaIn;
        hls::stream<ipUdpMeta>              udpMetaIn;

        hls::stream<mrcCtrlMeta<DATA_WIDTH> > metaOut;

        void tickOneCycle()
        {
            rx_process_ctrl<DATA_WIDTH, RX_PROCESS_CTRL_INSTID>(
                input,
                bthMetaIn,
                udpMetaIn,
                metaOut
            );
        }
    };

    bool checkCtrlProcessed(RxProcessCtrlTestBench& tb, const ProcessCtrlTestCase& tc)
    {
        bool returnValue = true;
        auto fail = [&](const std::string& msg) {
            std::cerr << "[FAIL " << tc.name << "] " << msg << std::endl;
            returnValue = false;
        };
        auto expect = [&](const std::string& field, const ap_uint<MRC_CTRL_HDR_SIZE>& got, const ap_uint<MRC_CTRL_HDR_SIZE>& exp) {
            if (got != exp) fail(field + " " + got.to_string(16) + " != " + exp.to_string(16));
        };

        // raw: byte b is header[b], bytes beyond the packet are zero
        ap_uint<MRC_CTRL_HDR_SIZE> expRaw = 0;
        for (size_t b = 0; b < std::min<size_t>(tc.header.size(), MRC_CTRL_HDR_SIZE / 8); ++b)
            expRaw(b * 8 + 7, b * 8) = tc.header[b];

        // metaOut: exactly one per packet, however long the packet is
        expect("metaOut count", tb.metaOut.size(), 1);
        if (!tb.metaOut.empty()) {
            mrcCtrlMeta<DATA_WIDTH> m = tb.metaOut.read();
            expect("op_code",       m.op_code,       tc.op_code);
            expect("partition_key", m.partition_key, tc.partition_key);
            expect("dest_qp",       m.dest_qp,       tc.dest_qp);
            expect("psn",           m.psn,           tc.psn);
            expect("ackReq",        m.ackReq,        tc.ackReq);
            expect("rtx",           m.rtx,           tc.rtx);
            expect("udpLength",     m.udpLength,     tc.udp_length);
            expect("srcAddress",    m.srcAddress,    tc.their_address);
            expect("srcPort",       m.srcPort,       tc.their_port);
            expect("lengthOk",      m.lengthOk,      tc.exp_lengthOk);
            expect("raw",           m.raw,           expRaw);
        }

        // all inputs consumed: the words after the header are drained, not left for the next packet
        if (!tb.input.empty())     fail("input words not consumed");
        if (!tb.bthMetaIn.empty()) fail("bthMetaIn not consumed");
        if (!tb.udpMetaIn.empty()) fail("udpMetaIn not consumed");

        // whatever is left must not leak into the next test case
        while (!tb.metaOut.empty())   tb.metaOut.read();
        while (!tb.input.empty())     tb.input.read();
        while (!tb.bthMetaIn.empty()) tb.bthMetaIn.read();
        while (!tb.udpMetaIn.empty()) tb.udpMetaIn.read();

        return returnValue;
    }

    // header sizes: AETH 4, SETH + CC_STATE 36, NETH 20, PETH 16, ERTH 16, EETH 36 bytes.
    // lengthOk = udp_length >= UDP 8 + BTH 12 + header + ICRC 4. Every row has its own seed, so raw bytes
    // left over from a previous row are caught. Word size 64 bytes (DATA_WIDTH 512)
    const ProcessCtrlTestCase processCtrlTestCases[] = {
        // every control opcode, header of its own size, exact UDP length
        {.name = "CTRL 01 ACK", .op_code = MRC_ACK, .partition_key = 0xFFFF, .dest_qp = 0x000008, .psn = 0x000202, .ackReq = false, .rtx = false,
         .their_address = ap_uint<128>("0x0A000002"), .their_port = 0xC000, .udp_length = 28, .header = buildWritePayload(4, 0x10), .exp_lengthOk = true},
        {.name = "CTRL 02 SACK", .op_code = MRC_SACK, .partition_key = 0xFFFF, .dest_qp = 0x000008, .psn = 0x00ABCD, .ackReq = false, .rtx = false,
         .their_address = ap_uint<128>("0x0A000002"), .their_port = 0xC001, .udp_length = 60, .header = buildWritePayload(36, 0x20), .exp_lengthOk = true},
        {.name = "CTRL 03 NACK", .op_code = MRC_NACK, .partition_key = 0xFFFF, .dest_qp = 0x000009, .psn = 0x000200, .ackReq = false, .rtx = false,
         .their_address = ap_uint<128>("0x0A000003"), .their_port = 0xC002, .udp_length = 44, .header = buildWritePayload(20, 0x30), .exp_lengthOk = true},
        {.name = "CTRL 04 PROBE", .op_code = MRC_PROBE_REQ, .partition_key = 0xFFFF, .dest_qp = 0x000005, .psn = 0x000000, .ackReq = false, .rtx = false,
         .their_address = ap_uint<128>("0x0A000004"), .their_port = 0xC003, .udp_length = 40, .header = buildWritePayload(16, 0x40), .exp_lengthOk = true},
        {.name = "CTRL 05 EP REQ", .op_code = MRC_EP_REQ, .partition_key = 0xFFFF, .dest_qp = 0x000002, .psn = 0x003002, .ackReq = false, .rtx = false,
         .their_address = ap_uint<128>("0x0A000005"), .their_port = 0xC004, .udp_length = 40, .header = buildWritePayload(16, 0x50), .exp_lengthOk = true},
        {.name = "CTRL 06 EP RES", .op_code = MRC_EP_RSP, .partition_key = 0xFFFF, .dest_qp = 0x000002, .psn = 0x003002, .ackReq = false, .rtx = false,
         .their_address = ap_uint<128>("0x0A000006"), .their_port = 0xC005, .udp_length = 60, .header = buildWritePayload(36, 0x60), .exp_lengthOk = true},

        // UDP length one byte short of the header: lengthOk false (the meta is still produced)
        {.name = "CTRL 07 ACK udp 27", .op_code = MRC_ACK, .partition_key = 0xFFFF, .dest_qp = 0x000008, .psn = 0x000203, .ackReq = false, .rtx = false,
         .their_address = ap_uint<128>("0x0A000002"), .their_port = 0xC000, .udp_length = 27, .header = buildWritePayload(4, 0x11), .exp_lengthOk = false},
        {.name = "CTRL 08 SACK udp 59", .op_code = MRC_SACK, .partition_key = 0xFFFF, .dest_qp = 0x000008, .psn = 0x00ABCE, .ackReq = false, .rtx = false,
         .their_address = ap_uint<128>("0x0A000002"), .their_port = 0xC001, .udp_length = 59, .header = buildWritePayload(36, 0x21), .exp_lengthOk = false},
        {.name = "CTRL 09 NACK udp 43", .op_code = MRC_NACK, .partition_key = 0xFFFF, .dest_qp = 0x000009, .psn = 0x000201, .ackReq = false, .rtx = false,
         .their_address = ap_uint<128>("0x0A000003"), .their_port = 0xC002, .udp_length = 43, .header = buildWritePayload(20, 0x31), .exp_lengthOk = false},
        {.name = "CTRL 10 EP RES udp 59", .op_code = MRC_EP_RSP, .partition_key = 0xFFFF, .dest_qp = 0x000002, .psn = 0x003003, .ackReq = false, .rtx = false,
         .their_address = ap_uint<128>("0x0A000006"), .their_port = 0xC005, .udp_length = 59, .header = buildWritePayload(36, 0x61), .exp_lengthOk = false},

        // NACK carries the length of the trimmed packet in its UDP header (7.5.5.6): much larger, still ok
        {.name = "CTRL 11 NACK trimmed length", .op_code = MRC_NACK, .partition_key = 0xFFFF, .dest_qp = 0x000009, .psn = 0x000202, .ackReq = false, .rtx = false,
         .their_address = ap_uint<128>("0x0A000003"), .their_port = 0xC002, .udp_length = 1100, .header = buildWritePayload(20, 0x32), .exp_lengthOk = true},

        // packet shorter than its header. CURRENT BEHAVIOUR: lengthOk only looks at the UDP length, not at the
        // bytes that arrived; the missing bytes of raw are zero
        {.name = "CTRL 12 SACK 1 byte udp 25", .op_code = MRC_SACK, .partition_key = 0xFFFF, .dest_qp = 0x000008, .psn = 0x00ABCF, .ackReq = false, .rtx = false,
         .their_address = ap_uint<128>("0x0A000002"), .their_port = 0xC001, .udp_length = 25, .header = buildWritePayload(1, 0x22), .exp_lengthOk = false},
        {.name = "CTRL 13 NACK 8 bytes udp 44", .op_code = MRC_NACK, .partition_key = 0xFFFF, .dest_qp = 0x000009, .psn = 0x000203, .ackReq = false, .rtx = false,
         .their_address = ap_uint<128>("0x0A000003"), .their_port = 0xC002, .udp_length = 44, .header = buildWritePayload(8, 0x33), .exp_lengthOk = true},

        // longer than the 36-byte buffer: raw keeps the first 36 bytes, the rest (and the next words) is drained.
        // 64 bytes (1 word), 65 bytes (2 words), 200 bytes (4 words)
        {.name = "CTRL 14 PROBE 64 bytes", .op_code = MRC_PROBE_REQ, .partition_key = 0xFFFF, .dest_qp = 0x000005, .psn = 0x000000, .ackReq = false, .rtx = false,
         .their_address = ap_uint<128>("0x0A000004"), .their_port = 0xC003, .udp_length = 88, .header = buildWritePayload(64, 0x41), .exp_lengthOk = true},
        {.name = "CTRL 15 PROBE 65 bytes", .op_code = MRC_PROBE_REQ, .partition_key = 0xFFFF, .dest_qp = 0x000005, .psn = 0x000000, .ackReq = false, .rtx = false,
         .their_address = ap_uint<128>("0x0A000004"), .their_port = 0xC003, .udp_length = 89, .header = buildWritePayload(65, 0x42), .exp_lengthOk = true},
        {.name = "CTRL 16 SACK 200 bytes", .op_code = MRC_SACK, .partition_key = 0xFFFF, .dest_qp = 0x000008, .psn = 0x00ABD0, .ackReq = false, .rtx = false,
         .their_address = ap_uint<128>("0x0A000002"), .their_port = 0xC001, .udp_length = 224, .header = buildWritePayload(200, 0x23), .exp_lengthOk = true},

        // right after the drained packets: a 4-byte header, bytes 4..35 of raw must be zero, not the previous SACK
        {.name = "CTRL 17 ACK after long SACK", .op_code = MRC_ACK, .partition_key = 0xFFFF, .dest_qp = 0x000008, .psn = 0x000204, .ackReq = false, .rtx = false,
         .their_address = ap_uint<128>("0x0A000002"), .their_port = 0xC000, .udp_length = 28, .header = buildWritePayload(4, 0x12), .exp_lengthOk = true},

        // BTH and UDP fields passed through: extremes, every field different, full 128-bit address
        {.name = "CTRL 18 BTH all ones", .op_code = MRC_NACK, .partition_key = 0xFFFF, .dest_qp = 0xFFFFFF, .psn = 0xFFFFFF, .ackReq = true, .rtx = true,
         .their_address = ap_uint<128>("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"), .their_port = 0xFFFF, .udp_length = 0xFFFF, .header = buildWritePayload(20, 0x34), .exp_lengthOk = true},
        {.name = "CTRL 19 BTH zero", .op_code = MRC_ACK, .partition_key = 0x0000, .dest_qp = 0x000000, .psn = 0x000000, .ackReq = false, .rtx = false,
         .their_address = ap_uint<128>("0x0"), .their_port = 0x0000, .udp_length = 28, .header = buildWritePayload(4, 0x13), .exp_lengthOk = true},
        {.name = "CTRL 20 distinct fields IPv6", .op_code = MRC_SACK, .partition_key = 0xABCD, .dest_qp = 0x123456, .psn = 0x654321, .ackReq = true, .rtx = false,
         .their_address = ap_uint<128>("0x20010DB8000000000000000000000001"), .their_port = 0x4321, .udp_length = 60, .header = buildWritePayload(36, 0x24), .exp_lengthOk = true},
        {.name = "CTRL 21 ackReq only", .op_code = MRC_PROBE_REQ, .partition_key = 0x1234, .dest_qp = 0x000005, .psn = 0x000001, .ackReq = true, .rtx = false,
         .their_address = ap_uint<128>("0x0A000004"), .their_port = 0xC003, .udp_length = 40, .header = buildWritePayload(16, 0x43), .exp_lengthOk = true},
        {.name = "CTRL 22 rtx only", .op_code = MRC_PROBE_REQ, .partition_key = 0x1234, .dest_qp = 0x000005, .psn = 0x000002, .ackReq = false, .rtx = true,
         .their_address = ap_uint<128>("0x0A000004"), .their_port = 0xC003, .udp_length = 40, .header = buildWritePayload(16, 0x44), .exp_lengthOk = true},

        // not a control opcode (should never get here): header size 0, so lengthOk only needs UDP + BTH + ICRC = 24
        {.name = "CTRL 23 data opcode udp 24", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 0x000005, .psn = 0x0A0B00, .ackReq = true, .rtx = false,
         .their_address = ap_uint<128>("0x0A000002"), .their_port = 0xC000, .udp_length = 24, .header = buildWritePayload(20, 0x70), .exp_lengthOk = true},
        {.name = "CTRL 24 data opcode udp 23", .op_code = MRC_RDMA_WRITE_ONLY, .partition_key = 0xFFFF, .dest_qp = 0x000005, .psn = 0x0A0B01, .ackReq = true, .rtx = false,
         .their_address = ap_uint<128>("0x0A000002"), .their_port = 0xC000, .udp_length = 23, .header = buildWritePayload(20, 0x71), .exp_lengthOk = false},
    };
} // namespace

int runRxProcessCtrlTest()
{
    RxProcessCtrlTestBench tb;

    int failures = 0;
    const size_t testCaseCount = sizeof(processCtrlTestCases) / sizeof(processCtrlTestCases[0]);
    for (size_t tcIndex = 0; tcIndex < testCaseCount; ++tcIndex) {
        const ProcessCtrlTestCase& tc = processCtrlTestCases[tcIndex];
        std::cout << "\n" << tc.name << "\n" << std::endl;

        tb.bthMetaIn.write(mrcCtrlBthMeta(tc.op_code, tc.partition_key, tc.dest_qp, tc.psn, tc.ackReq, tc.rtx));
        tb.udpMetaIn.write(ipUdpMeta(tc.their_address, tc.their_port, RDMA_DEFAULT_PORT, tc.udp_length));

        size_t wordCount = 0;
        const size_t bytesPerWord = DATA_WIDTH / 8;
        for (size_t byteIndex = 0; byteIndex < tc.header.size(); byteIndex += bytesPerWord) {
            net_axis<DATA_WIDTH> wordToSend;
            wordToSend.data = 0;
            wordToSend.keep = 0;

            const size_t howManyBytes = std::min(bytesPerWord, tc.header.size() - byteIndex);
            for (size_t byte = 0; byte < howManyBytes; byte++) {
                wordToSend.data(byte * 8 + 7, byte * 8) = tc.header[byteIndex + byte];
                wordToSend.keep[byte] = 1;
            }
            wordToSend.last = byteIndex + bytesPerWord >= tc.header.size();

            tb.input.write(wordToSend);
            ++wordCount;
        }

        for (size_t c = 0; c < wordCount + 2; ++c) tb.tickOneCycle(); // META, then one word per call (HEADER or DRAIN)
        failures += !checkCtrlProcessed(tb, tc);
    }

    return failures;
}