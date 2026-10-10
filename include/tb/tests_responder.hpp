#pragma once
/*
 * Running a scenario
 * -------------------------------------------------------------------------------------------------
 * A scenario is a list of packets (tests_config.hpp), writes and MRC control packets mixed. The
 * opcode decides the kind: checkIfMrcCtrl(opcode) -> control packet, otherwise RDMA WRITE.
 *
 * Each packet says what happens after it (TestCase::after):
 *     BACK_TO_BACK  the next packet's first beat follows on the next cycle
 *     N > 0         N idle cycles, then the next packet
 *     DRAIN         end of a GROUP: run CYCLES_PER_TEST cycles, then check everything in the group
 *
 * Feeding is realistic: one beat per cycle at most, only when the DUT input has room
 * (RX_INPUT_DEPTH, i.e. TREADY backpressure), ipUdpMeta arrives RX_META_LAG cycles after the first
 * beat. Several packets of a group can therefore be inside the module at the same time.
 *
 * Checks, per group: the module processes packets in order, so outputs are matched in order:
 *     each write expecting MEM_WRITE takes the next memory command + the next payload,
 *     each write expecting a reply takes the next TX packet,
 *     each control packet must have reached rx_ctrl_fsm (checked through the module's prints),
 *     anything left over is unexpected. The drop counter is checked over the whole group.
 * -------------------------------------------------------------------------------------------------
 */

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <cstdint>

#include "ib_transport_protocol/ib_transport_protocol.hpp"
#include "tb_config.hpp"
#include "utils.hpp"
#include "pkt_data_builder.hpp"
#include "pkt_ctrl_builder.hpp"
#include "dut_ib_transport_protocol.hpp"

// What must happen to the payload
enum MemExpect  { MEM_WRITE,  MEM_NONE };                 // payload reaches memory / nothing is written
// What the module must send back
enum ReplyExpect{ REPLY_ACK,  REPLY_NAK, REPLY_NONE };    // ACK / NAK (seq. error) / no packet at all
// What must happen to regInvalidPsnDropCount
enum DropExpect { DROP_INC,   DROP_SAME, DROP_ANY };      // +1 / unchanged / not checked
// Flags inside BTH
enum : uint8_t { F_NONE = 0, F_AR = 0x1, F_RTX = 0x2, F_TS = 0x4 };  // request BTH flags
enum RelExpect { REL_NONE, REL_SACK, REL_NACK };                   // reliability-layer reply
// What happens after a packet (TestCase::after); any N > 0 means N idle cycles
enum : int { DRAIN = -1, BACK_TO_BACK = 0 };

struct Expected
{
	MemExpect   mem;
	// memory side (only checked if mem == MEM_WRITE)
	uint64_t    memAddr;
	uint32_t    memLen;
	bool        memLast;      // memCmd.lst: 1 = last chunk of the message
	// network side: the reply the module sends back (only checked if reply != REPLY_NONE)
	ReplyExpect reply;
	uint32_t    ackPsn;
	uint32_t    ackMsn;
	DropExpect  drop;
};

// One packet of the scenario, fully resolved (PSNs computed, bytes built)
struct Step
{
	std::string          name;
	std::string          note;    // printed with the packet, e.g. the PSN state before it
	bool                 isCtrl;
	WritePacket          write;   // writes only
	Expected             exp;     // writes only
	CtrlPacket           ctrl;    // control packets only
	std::vector<uint8_t> bytes;   // what goes on the wire, starting with the BTH
	uint16_t             udpLength;
	int                  after;   // DRAIN / BACK_TO_BACK / N idle cycles
};

static Step makeWriteStep(const std::string& name, const WritePacket& p, const Expected& e, int after)
{
	Step s;
	s.name      = name;
	s.isCtrl    = false;
	s.write     = p;
	s.exp       = e;
	s.bytes     = buildMrcPacket(p);
	s.udpLength = 8 + s.bytes.size() + 4;   // UDP header + IB packet + ICRC (see main.cpp header)
	s.after     = after;
	return s;
}

static Step makeCtrlStep(const std::string& name, const CtrlPacket& c, int after)
{
	Step s;
	s.name      = name;
	s.isCtrl    = true;
	s.ctrl      = c;
	s.bytes     = buildCtrlPacket(c);
	s.udpLength = 8 + s.bytes.size() + 4;
	s.after     = after;
	return s;
}

static std::string afterName(int after)
{
	if (after == DRAIN)        return "then drain and check";
	if (after == BACK_TO_BACK) return "next packet back to back";
	return "then " + std::to_string(after) + " idle cycle(s)";
}

// Everything the module puts out, in the order and at the cycle it comes out
struct Outputs
{
	struct Cmd    { memCmd cmd; int cycle; };
	struct Packet { std::vector<uint8_t> bytes; int cycle; bool last; };

	std::vector<Cmd>       memCmds;
	std::vector<Packet>    memPayloads;   // one entry per TLAST on m_axis_mem_write_data
	std::vector<ipUdpMeta> txMetas;
	std::vector<Packet>    txPackets;     // one entry per TLAST on m_axis_tx_data
	size_t                 ackMetas = 0;

	std::vector<uint8_t>   memPartial, txPartial;

	void collect(Dut& dut)
	{
		while (!dut.m_axis_mem_write_cmd.empty())
			memCmds.push_back({ dut.m_axis_mem_write_cmd.read(), g_cycle });
		while (!dut.m_axis_mem_write_data.empty())
		{
			net_axis<DATA_WIDTH> w = dut.m_axis_mem_write_data.read();
			appendBeatBytes(w, memPartial);
			if (w.last) { memPayloads.push_back({ memPartial, g_cycle, true }); memPartial.clear(); }
		}
		while (!dut.m_axis_tx_meta.empty())
			txMetas.push_back(dut.m_axis_tx_meta.read());
		while (!dut.m_axis_tx_data.empty())
		{
			net_axis<DATA_WIDTH> w = dut.m_axis_tx_data.read();
			appendBeatBytes(w, txPartial);
			if (w.last) { txPackets.push_back({ txPartial, g_cycle, true }); txPartial.clear(); }
		}
		while (!dut.m_axis_rx_ack_meta.empty()) { dut.m_axis_rx_ack_meta.read(); ackMetas++; }
	}
};

// ---- checks of one write against its outputs (same checks as the original runWriteTest) ----
static void checkWriteMemory(const Step& s, const Outputs& out, size_t& iCmd, size_t& iPay)
{
	const WritePacket& pkt = s.write;
	const Expected&    exp = s.exp;
	if (exp.mem != MEM_WRITE)
	{
		std::cout << "[TB]   no memory write expected" << std::endl;
		return;
	}
	check(iCmd < out.memCmds.size(), "a memory write command was issued for it");
	if (iCmd < out.memCmds.size())
	{
		const memCmd& c = out.memCmds[iCmd].cmd;
		std::cout << "[TB]   memory command at cycle " << out.memCmds[iCmd].cycle << std::endl;
		checkEq("memCmd.op_code", c.op_code, pkt.opcode);
		checkEq("memCmd.qpn    ", c.qpn.to_uint(), LOCAL_QPN);
		checkEq("memCmd.addr   ", c.addr.to_uint64(), exp.memAddr);
		checkCount("memCmd.len    ", c.len.to_uint(), exp.memLen);
		checkEq("memCmd.lst    ", c.lst.to_uint(), exp.memLast);
		iCmd++;
	}
	check(iPay < out.memPayloads.size(), "payload data reached memory");
	if (iPay < out.memPayloads.size())
	{
		const std::vector<uint8_t>& memBytes = out.memPayloads[iPay].bytes;
		std::cout << "[TB]   payload in memory by cycle " << out.memPayloads[iPay].cycle << std::endl;
		checkCount("bytes written to memory", memBytes.size(), pkt.payload.size());
		check(memBytes == pkt.payload, "memory data is identical to the payload that was sent");
		iPay++;
	}
}

static void checkWriteReply(const Step& s, const Outputs& out, size_t& iTx)
{
	const Expected& exp = s.exp;
	if (exp.reply == REPLY_NONE)
	{
		std::cout << "[TB]   no reply expected" << std::endl;
		return;
	}
	check(iTx < out.txPackets.size() && iTx < out.txMetas.size(), "a reply (ACK/NAK) was sent for it");
	if (iTx >= out.txPackets.size() || iTx >= out.txMetas.size())
		return;

	const ipUdpMeta&            m = out.txMetas[iTx];
	const std::vector<uint8_t>& a = out.txPackets[iTx].bytes;
	std::cout << "[TB]   reply at cycle " << out.txPackets[iTx].cycle << std::endl;
	iTx++;

	checkEq("tx meta: destination IP", m.their_address.to_uint64(), REMOTE_IP);
	checkCount("tx meta: UDP length (BTH+AETH+ICRC)", m.length.to_uint(), 12 + 4 + 4);
	if (a.size() < 16)
	{
		check(false, "reply is shorter than BTH+AETH (16 B)");
		return;
	}
	const bool nak = exp.reply == REPLY_NAK;
	uint32_t syndrome = a[12];
	std::cout << "[TB]   parsed reply: " << opcodeName(a[0]) << "  DestQP=" << hex(get24(a, 5))
	          << "  PSN=" << hex(get24(a, 9)) << "  AETH.syndrome=" << hex(syndrome, 2)
	          << (syndrome == 0x1F ? " (ACK)" : syndrome == 0x60 ? " (NAK: PSN sequence error)" : "")
	          << "  AETH.MSN=" << hex(get24(a, 13)) << std::endl;
	checkCount("reply length in bytes", a.size(), 16);
	checkEq("reply BTH.opcode (MRC_ACK)", a[0], MRC_ACK);
	checkEq("reply BTH.DestQP (remote QPN)", get24(a, 5), REMOTE_QPN);
	checkEq("reply BTH.PSN", get24(a, 9), exp.ackPsn);
	checkEq(nak ? "reply AETH.syndrome (NAK seq err)" : "reply AETH.syndrome (ACK)", syndrome, nak ? 0x60 : 0x1F);
	checkEq("reply AETH.MSN", get24(a, 13), exp.ackMsn);
}

/**
 * Feeds one group of packets into the DUT cycle by cycle, drains, then checks.
 * The last packet of the group is the one with after == DRAIN (or the last of the scenario).
 */
static void runGroup(Dut& dut, const std::vector<Step>& group)
{
	std::string title = "GROUP:";
	for (size_t i = 0; i < group.size(); i++) title += (i ? "  ->  " : " ") + group[i].name;
	banner(title);

	// ---------------- (a) show the packets ----------------
	for (size_t i = 0; i < group.size(); i++)
	{
		const Step& s = group[i];
		std::cout << "[TB] Packet " << i << ": " << s.name << std::endl;
		if (!s.note.empty()) std::cout << "[TB]   " << s.note << std::endl;
		std::cout << "[TB]   " << opcodeName(s.bytes[0]);
		if (s.isCtrl)
			std::cout << "  DestQP=" << hex(s.ctrl.dqp) << "  PSN=" << hex(s.ctrl.psn)
			          << "  header=" << std::dec << s.ctrl.hdr.size() << " B";
		else
		{
			std::cout << "  DestQP=" << hex(LOCAL_QPN) << "  PSN=" << hex(s.write.psn) << ", " << std::dec << s.write.psn;
			if (hasReth(s.write.opcode)) std::cout << "  RETH.vaddr=" << hex(s.write.vaddr) << "  RETH.len=" << std::dec << s.write.dmaLen;
			std::cout << "  payload=" << std::dec << s.write.payload.size() << " B";
		}
		std::cout << "  UDP length=" << std::dec << s.udpLength << "  (" << afterName(s.after) << ")" << std::endl;
		hexDump(s.bytes);
	}

	// ---------------- (b) feed the packets, one beat per cycle at most ----------------
	std::vector<std::vector<net_axis<DATA_WIDTH> > > beats(group.size());
	for (size_t i = 0; i < group.size(); i++) beats[i] = bytesToBeats(group[i].bytes);

	const uint32_t dropsBefore = dut.regInvalidPsnDropCount.to_uint();
	Outputs out;
	std::deque<std::pair<int, ipUdpMeta> > metaQueue;   // (cycle it is due, meta)
	size_t k = 0, b = 0;                                // packet, beat
	int    nextStart = g_cycle;
	int    stalled   = 0;
	bool   aborted   = false;

	dut.clearLog();
	std::cout << std::endl << "[TB] Feeding " << group.size() << " packet(s)... (module prints follow)" << std::endl;
	while (k < group.size())
	{
		if (g_cycle >= nextStart)
		{
			if (dut.s_axis_rx_data.size() < RX_INPUT_DEPTH)
			{
				if (b == 0)
				{
					metaQueue.push_back(std::make_pair(g_cycle + RX_META_LAG,
						ipUdpMeta(REMOTE_IP, REMOTE_UDP_PORT, RDMA_DEFAULT_PORT, group[k].udpLength)));
					std::cout << std::dec << std::setfill(' ') << "[cyc " << std::setw(4) << g_cycle << "] [TB] first beat of packet "
					          << k << " (" << group[k].name << ")" << std::endl;
				}
				dut.s_axis_rx_data.write(beats[k][b++]);
				stalled = 0;
				if (b == beats[k].size())
				{
					std::cout << std::dec << std::setfill(' ') << "[cyc " << std::setw(4) << g_cycle << "] [TB] last beat of packet " << k
					          << " (" << b << " beat(s))" << std::endl;
					nextStart = g_cycle + 1 + (group[k].after > 0 ? group[k].after : 0);
					k++;
					b = 0;
				}
			}
			else if (++stalled > CYCLES_PER_TEST)
			{
				check(false, "RX input stalled for " + std::to_string(CYCLES_PER_TEST) +
				             " cycles: the module stopped consuming packets");
				aborted = true;
				break;
			}
		}
		while (!metaQueue.empty() && metaQueue.front().first <= g_cycle)
		{
			dut.s_axis_rx_meta.write(metaQueue.front().second);
			metaQueue.pop_front();
		}
		dut.tick();
		out.collect(dut);
	}

	// ---------------- (c) drain ----------------
	std::cout << "[TB] All packets fed, running " << CYCLES_PER_TEST << " more cycles..." << std::endl;
	for (int i = 0; i < CYCLES_PER_TEST; i++)
	{
		while (!metaQueue.empty() && metaQueue.front().first <= g_cycle)
		{
			dut.s_axis_rx_meta.write(metaQueue.front().second);
			metaQueue.pop_front();
		}
		dut.tick();
		out.collect(dut);
	}

	// ---------------- (d) outputs ----------------
	std::cout << std::endl << "[TB] ---- Outputs (cycle they left the module) ----" << std::endl;
	for (size_t i = 0; i < out.memCmds.size(); i++)
	{
		const memCmd& c = out.memCmds[i].cmd;
		std::cout << "[TB] cyc " << std::dec << out.memCmds[i].cycle << " m_axis_mem_write_cmd: op=" << opcodeName(c.op_code)
		          << " qpn=" << hex(c.qpn.to_uint()) << " addr=" << hex(c.addr.to_uint64())
		          << " len=" << std::dec << c.len.to_uint() << " lst=" << c.lst.to_uint()
		          << " host=" << c.host.to_uint() << std::endl;
	}
	for (size_t i = 0; i < out.memPayloads.size(); i++)
	{
		std::cout << "[TB] cyc " << std::dec << out.memPayloads[i].cycle << " m_axis_mem_write_data: "
		          << out.memPayloads[i].bytes.size() << " B" << std::endl;
		hexDump(out.memPayloads[i].bytes);
	}
	for (size_t i = 0; i < out.txPackets.size(); i++)
	{
		std::cout << "[TB] cyc " << std::dec << out.txPackets[i].cycle << " m_axis_tx_data packet ("
		          << out.txPackets[i].bytes.size() << " B)";
		if (i < out.txMetas.size())
			std::cout << ", meta: dst ip=" << hex(out.txMetas[i].their_address.to_uint64())
			          << " length=" << std::dec << out.txMetas[i].length.to_uint();
		std::cout << std::endl;
		hexDump(out.txPackets[i].bytes);
	}

	// ---------------- (e) checks ----------------
	std::cout << std::endl << "[TB] ---- Checks ----" << std::endl;
	if (aborted)
		std::cout << "[TB] (group aborted, the checks below are expected to fail)" << std::endl;

	size_t iCmd = 0, iPay = 0, iTx = 0;
	size_t dropsExpected = 0;
	bool   dropsChecked  = true;
	std::map<std::string, size_t> ctrlExpected;   // expected rx_ctrl_fsm print -> number of packets

	for (size_t i = 0; i < group.size(); i++)
	{
		const Step& s = group[i];
		std::cout << "[TB] Packet " << i << ": " << s.name << std::endl;
		if (s.isCtrl)
		{
			ctrlExpected[s.ctrl.expectLog]++;
			std::cout << "[TB]   control packet: no memory write, no reply expected (checked below)" << std::endl;
			continue;
		}
		checkWriteMemory(s, out, iCmd, iPay);
		checkWriteReply(s, out, iTx);
		if (s.exp.drop == DROP_INC) dropsExpected++;
		if (s.exp.drop == DROP_ANY) dropsChecked = false;
	}

	std::cout << "[TB] Whole group" << std::endl;
	checkCount("unexpected memory write commands", out.memCmds.size() - iCmd, 0);
	checkCount("unexpected memory payloads", out.memPayloads.size() - iPay, 0);
	check(out.memPartial.empty(), "no memory data beat left without TLAST");
	checkCount("unexpected packets sent back", out.txPackets.size() - iTx, 0);
	checkCount("tx meta entries = tx packets", out.txMetas.size(), out.txPackets.size());

	for (std::map<std::string, size_t>::const_iterator it = ctrlExpected.begin(); it != ctrlExpected.end(); ++it)
		checkCount("control packets that reached rx_ctrl_fsm (printed \"" + it->first + "\")",
		           dut.logCount(it->first), it->second);

	const uint32_t dropsNow = dut.regInvalidPsnDropCount.to_uint();
	if (dropsChecked) checkCount("invalid-PSN drop counter increase over the group", dropsNow - dropsBefore, dropsExpected);
	else std::cout << "[TB] invalid-PSN drop counter " << dropsBefore << " -> " << dropsNow << " (not checked)" << std::endl;

	checkCount("ackMeta to the host (only used for our own requests)", out.ackMetas, 0);
	check(dut.s_axis_rx_data.empty() && dut.s_axis_rx_meta.empty(), "module consumed the whole input");
	check(dut.m_axis_mem_read_cmd.empty(), "no memory READ command issued");
}