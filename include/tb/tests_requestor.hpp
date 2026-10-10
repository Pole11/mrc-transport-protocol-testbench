#pragma once
/*
 * tests_requestor.hpp  --  runs the REQUESTOR scenario (step C: transport ACK / SACK / NACK)
 * -------------------------------------------------------------------------------------------------
 * In tests_responder.hpp the module is the RESPONDER. Here it is the REQUESTOR, and the testbench:
 *   - posts write requests on s_axis_sq_meta, one txMeta per packet (as the Coyote shell does),
 *   - plays host memory: every m_axis_mem_read_cmd is answered on s_axis_mem_read_data with a
 *     known byte pattern, so the payload of every TX packet (first send or resend) can be checked,
 *   - plays the remote RESPONDER: it feeds transport ACKs, SACKs and NACKs into the RX side,
 *   - checks what comes out: request packets on TX, completions on m_axis_rx_ack_meta, and the
 *     module's own prints (which branch of rx_req_fsm / the retransmitter ran).
 *
 * A scenario (test_requestor_pkts.hpp) is a list of rows. A group is every row up to the next
 * ReqCheck(...), which drains and checks the group as a whole, because outputs are not 1:1 with
 * inputs: one ACK can complete several packets, one NACK triggers one resend.
 *
 * The requestor QPs are separate from the responder scenario's QP (tb_config.hpp), so the two
 * scenarios do not disturb each other.
 * -------------------------------------------------------------------------------------------------
 */

#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <cstdint>

#include "tests_responder.hpp"

static const uint32_t REQ_QPN_A = 0x07, REQ_REMOTE_QPN_A = 0x0043, REQ_FIRST_PSN_A = 0xFFFFFD; // 4th packet wraps to 0
static const uint32_t REQ_QPN_B = 0x08, REQ_REMOTE_QPN_B = 0x0044, REQ_FIRST_PSN_B = 0x000300;
static const uint32_t REQ_QPN_C = 0x09, REQ_REMOTE_QPN_C = 0x0045, REQ_FIRST_PSN_C = 0x000400;

// =================================================================================================
// Requestor QPs (values in tb_config.hpp)
// =================================================================================================

struct ReqQp { uint32_t qpn; uint32_t remoteQpn; uint32_t firstPsn; };

static const ReqQp REQ_QPS[] = {
	{ REQ_QPN_A, REQ_REMOTE_QPN_A, REQ_FIRST_PSN_A },
	{ REQ_QPN_B, REQ_REMOTE_QPN_B, REQ_FIRST_PSN_B },
	{ REQ_QPN_C, REQ_REMOTE_QPN_C, REQ_FIRST_PSN_C },
};
static const size_t REQ_NUM_QPS = sizeof(REQ_QPS) / sizeof(REQ_QPS[0]);

static uint32_t reqRemoteQpn(uint32_t qpn)
{
	for (size_t i = 0; i < REQ_NUM_QPS; i++)
		if (REQ_QPS[i].qpn == qpn) return REQ_QPS[i].remoteQpn;
	return 0;
}

// PSN of the k-th packet (0-based) a requestor QP sends, with the 24-bit wrap
static uint32_t psnA(int k) { return (REQ_FIRST_PSN_A + (uint32_t) k) & 0xFFFFFF; }
static uint32_t psnB(int k) { return (REQ_FIRST_PSN_B + (uint32_t) k) & 0xFFFFFF; }
static uint32_t psnC(int k) { return (REQ_FIRST_PSN_C + (uint32_t) k) & 0xFFFFFF; }

// =================================================================================================
// Packets from the remote responder (dqp = OUR QP, Table 7-6)
// =================================================================================================

// Transport ACK, or AETH NAK. BTH.psn = cack_psn (Table 7-6).
//   syndrome 0x1F        ACK (credit field always 0x1F in MRC, 6.4)
//   syndrome 0x20        RNR NAK (not applicable in MRC, must be ignored)
//   syndrome 0x60 | code NAK, code = aethNakCode (e.g. 0x62 = Remote Access Error)
static CtrlPacket ReqAck(uint32_t qpn, uint32_t cackPsn, uint8_t syndrome = 0x1F, uint32_t msn = 0)
{
	std::vector<uint8_t> h;
	put8 (h, syndrome);
	put24(h, msn & 0xFFFFFF);
	std::string name = (syndrome == 0x1F) ? "ACK" : ((syndrome & 0x60) == 0x60 ? "AETH NAK" : "AETH RNR/reserved");
	CtrlPacket c = { name, MRC_ACK, qpn, cackPsn & 0xFFFFFF, h, "]: ACK qp" };
	return c;
}

// Reliability SACK: only the cumulative part, empty bitmap
static CtrlPacket ReqSack(uint32_t qpn, uint32_t cackPsn)
{
	SackFields f;
	f.cackPsn = cackPsn & 0xFFFFFF;  f.entropy = 0xE0000000 | f.cackPsn;
	f.spdcid  = qpn;                 f.dpdcid  = reqRemoteQpn(qpn);
	CtrlPacket c = sackPacket(f);
	c.dqp = qpn;
	return c;
}

// Reliability NACK: BTH.psn = NETH.nack_psn = the packet that triggered it
static CtrlPacket ReqNack(uint32_t qpn, uint8_t reason, uint32_t nackPsn)
{
	NackFields f;
	f.nackReason = reason;  f.nackPsn = nackPsn & 0xFFFFFF;  f.entropy = 0xE0000000 | f.nackPsn;
	f.spdcid     = qpn;     f.dpdcid  = reqRemoteQpn(qpn);   f.ccType  = 0x2;
	CtrlPacket c = nackPacket(f);
	c.dqp = qpn;
	return c;
}

// =================================================================================================
// Scenario rows
// =================================================================================================

enum ReqRowKind { RQ_POST, RQ_RX, RQ_IDLE, RQ_CHECK };

struct TxExp  { uint32_t qpn; uint32_t psn; bool resend; };   // a request packet expected on TX
struct CplExp { uint32_t qpn; bool lst; };                    // an ackMeta expected for the host
struct LogExp { std::string text; size_t count; };            // the module printed 'text' 'count' times

struct ReqRow
{
	ReqRowKind  kind;
	std::string name;
	// RQ_POST: one packet of a write, exactly what the shell puts on s_axis_sq_meta
	uint32_t    qpn;
	mrcOpCode   op;
	uint64_t    laddr;
	uint64_t    raddr;
	uint32_t    len;
	bool        lst;
	// RQ_RX: a control packet from the remote responder
	CtrlPacket  ctrl;
	// RQ_POST / RQ_RX: idle cycles after the row.  RQ_IDLE: idle cycles.  RQ_CHECK: drain cycles.
	int         cycles;
	// RQ_CHECK: what the group must produce
	std::vector<TxExp>  tx;
	std::vector<CplExp> cpl;
	std::vector<LogExp> logs;
};

static ReqRow reqRow(ReqRowKind kind, const std::string& name)
{
	ReqRow r;
	r.kind = kind;  r.name = name;
	r.qpn = 0;      r.op = MRC_RDMA_WRITE_ONLY;  r.laddr = 0;  r.raddr = 0;  r.len = 0;  r.lst = false;
	r.cycles = 0;
	return r;
}

static ReqRow ReqPost(const std::string& name, uint32_t qpn, mrcOpCode op, uint64_t laddr, uint64_t raddr,
                      uint32_t len, bool lst, int idleAfter = 0)
{
	ReqRow r = reqRow(RQ_POST, name);
	r.qpn = qpn;  r.op = op;  r.laddr = laddr;  r.raddr = raddr;  r.len = len;  r.lst = lst;
	r.cycles = idleAfter;
	return r;
}

static ReqRow ReqRx(const std::string& name, const CtrlPacket& c, int idleAfter = 0)
{
	ReqRow r = reqRow(RQ_RX, name);
	r.ctrl = c;  r.cycles = idleAfter;
	return r;
}

static ReqRow ReqIdle(int cycles)
{
	ReqRow r = reqRow(RQ_IDLE, "idle");
	r.cycles = cycles;
	return r;
}

static ReqRow ReqCheck(const std::string& name, const std::vector<TxExp>& tx, const std::vector<CplExp>& cpl,
                       const std::vector<LogExp>& logs, int drain = REQ_DRAIN_CYCLES)
{
	ReqRow r = reqRow(RQ_CHECK, name);
	r.tx = tx;  r.cpl = cpl;  r.logs = logs;  r.cycles = drain;
	return r;
}

// =================================================================================================
// Host memory model and output collection
// =================================================================================================

// Byte stored at host address 'a'. Depends only on the address, so a resend must carry the same bytes.
static uint8_t reqMemByte(uint64_t a) { return (uint8_t) ((a ^ (a >> 8) ^ 0x5A) & 0xFF); }

static std::vector<uint8_t> reqMemBytes(uint64_t addr, uint32_t len)
{
	std::vector<uint8_t> b(len);
	for (uint32_t i = 0; i < len; i++) b[i] = reqMemByte(addr + i);
	return b;
}

// Where the payload starts in a request the module SENDS. TX still uses the RC layout:
// [BTH][RETH][payload] on FIRST/ONLY, [BTH][payload] on MIDDLE/LAST. When TX moves to the MRC layout
// ([BTH][METH][TSETH?][RETH][ImmDt?][payload] on every packet), change this function.
static size_t txPayloadOffset(unsigned op)
{
	return (op == MRC_RDMA_WRITE_ONLY || op == MRC_RDMA_WRITE_FIRST) ? 12 + 16 : 12;
}

struct ReqOutputs
{
	struct Pkt { std::vector<uint8_t> bytes; int cycle; };
	struct Cpl { ackMeta meta; int cycle; };
	struct Rd  { memCmd cmd; int cycle; };

	std::vector<Pkt>       tx;
	std::vector<ipUdpMeta> txMetas;
	std::vector<Cpl>       cpl;
	std::vector<Rd>        memReads;
	size_t                 memWrites = 0;
	std::vector<uint8_t>   txPartial;

	void collect(Dut& dut)
	{
		while (!dut.m_axis_tx_meta.empty())
			txMetas.push_back(dut.m_axis_tx_meta.read());
		while (!dut.m_axis_tx_data.empty())
		{
			net_axis<DATA_WIDTH> w = dut.m_axis_tx_data.read();
			appendBeatBytes(w, txPartial);
			if (w.last) { Pkt p = { txPartial, g_cycle }; tx.push_back(p); txPartial.clear(); }
		}
		while (!dut.m_axis_rx_ack_meta.empty())
		{
			Cpl c = { dut.m_axis_rx_ack_meta.read(), g_cycle };
			cpl.push_back(c);
		}
		while (!dut.m_axis_mem_write_cmd.empty())  { dut.m_axis_mem_write_cmd.read(); memWrites++; }
		while (!dut.m_axis_mem_write_data.empty()) dut.m_axis_mem_write_data.read();
	}
};

// Answer every read command with the bytes of the memory model
static void serveMemReads(Dut& dut, ReqOutputs& out)
{
	while (!dut.m_axis_mem_read_cmd.empty())
	{
		memCmd c = dut.m_axis_mem_read_cmd.read();
		ReqOutputs::Rd r = { c, g_cycle };
		out.memReads.push_back(r);
		std::vector<net_axis<DATA_WIDTH> > beats = bytesToBeats(reqMemBytes(c.addr.to_uint64(), c.len.to_uint()));
		for (size_t i = 0; i < beats.size(); i++) dut.s_axis_mem_read_data.write(beats[i]);
	}
}

static void reqTick(Dut& dut, ReqOutputs& out)
{
	dut.tick();
	serveMemReads(dut, out);
	out.collect(dut);
}

static std::string cycTag()
{
	std::stringstream ss;
	ss << std::dec << std::setfill(' ') << "[cyc " << std::setw(4) << g_cycle << "] ";
	return ss.str();
}

// =================================================================================================
// Running
// =================================================================================================

struct ReqTbState
{
	struct Sent { mrcOpCode op; uint64_t laddr; uint32_t len; };
	std::map<uint32_t, uint32_t> nextPsn;                       // per requestor QP
	std::map<std::pair<uint32_t, uint32_t>, Sent> sent;         // (qpn, psn) -> what was posted
};

static void setupRequestorQps(Dut& dut, ReqTbState& st)
{
	for (size_t i = 0; i < REQ_NUM_QPS; i++)
	{
		const ReqQp& q = REQ_QPS[i];
		// qpContext(state, qp_num, remote_psn, local_psn, r_key, virtual_address)
		//   remote_psn -> req_next_psn: first PSN this QP sends
		dut.s_axis_qp_interface.write(qpContext(READY_RECV, q.qpn, q.firstPsn, 0, RKEY, 0));
		dut.s_axis_qp_conn_interface.write(ifConnReq(q.qpn, q.remoteQpn, REMOTE_IP, REMOTE_UDP_PORT));
		st.nextPsn[q.qpn] = q.firstPsn & 0xFFFFFF;
		std::cout << "[TB] requestor QP " << hex(q.qpn) << " -> remote QP " << hex(q.remoteQpn)
		          << ", first PSN " << hex(q.firstPsn) << std::endl;
	}
	dut.run(20 * (int) REQ_NUM_QPS);
}

static bool feedReqCtrl(Dut& dut, ReqOutputs& out, const ReqRow& r)
{
	std::vector<uint8_t> bytes = buildCtrlPacket(r.ctrl);
	uint16_t udpLen = (uint16_t) (8 + bytes.size() + 4);
	std::vector<net_axis<DATA_WIDTH> > beats = bytesToBeats(bytes);

	std::cout << cycTag() << "[TB] rx " << r.name << ": " << r.ctrl.name << "  DestQP=" << hex(r.ctrl.dqp)
	          << "  PSN=" << hex(r.ctrl.psn) << "  header=" << std::dec << r.ctrl.hdr.size() << " B" << std::endl;
	hexDump(bytes);

	dut.s_axis_rx_meta.write(ipUdpMeta(REMOTE_IP, REMOTE_UDP_PORT, RDMA_DEFAULT_PORT, udpLen));
	size_t b = 0;
	int stalled = 0;
	while (b < beats.size())
	{
		if (dut.s_axis_rx_data.size() < RX_INPUT_DEPTH) { dut.s_axis_rx_data.write(beats[b++]); stalled = 0; }
		else if (++stalled > REQ_DRAIN_CYCLES)
		{
			check(false, "RX input stalled: the module stopped consuming packets");
			return false;
		}
		reqTick(dut, out);
	}
	return true;
}

static bool reqHasLogExp(const ReqRow& chk, const std::string& text)
{
	for (size_t i = 0; i < chk.logs.size(); i++) if (chk.logs[i].text == text) return true;
	return false;
}

static void checkReqGroup(Dut& dut, const ReqTbState& st, const ReqRow& chk, const ReqOutputs& out)
{
	std::cout << std::endl << "[TB] ---- Outputs ----" << std::endl;
	for (size_t i = 0; i < out.memReads.size(); i++)
		std::cout << "[TB] cyc " << std::dec << out.memReads[i].cycle << " m_axis_mem_read_cmd: addr="
		          << hex(out.memReads[i].cmd.addr.to_uint64()) << " len=" << std::dec << out.memReads[i].cmd.len.to_uint() << std::endl;
	for (size_t i = 0; i < out.tx.size(); i++)
	{
		std::cout << "[TB] cyc " << std::dec << out.tx[i].cycle << " m_axis_tx_data packet (" << out.tx[i].bytes.size() << " B)" << std::endl;
		hexDump(out.tx[i].bytes);
	}
	for (size_t i = 0; i < out.cpl.size(); i++)
	{
		const ackMeta& m = out.cpl[i].meta;
		std::cout << "[TB] cyc " << std::dec << out.cpl[i].cycle << " m_axis_rx_ack_meta: op=" << opcodeName(m.op_code)
		          << " qpn=" << hex(m.qpn.to_uint()) << " host=" << m.host.to_uint() << " lst=" << m.lst.to_uint() << std::endl;
	}

	std::cout << std::endl << "[TB] ---- Checks ----" << std::endl;

	// ---- request packets on TX (first sends and resends, in order) ----
	checkCount("request packets sent on TX", out.tx.size(), chk.tx.size());
	checkCount("tx meta entries = tx packets", out.txMetas.size(), out.tx.size());
	for (size_t i = 0; i < out.tx.size() && i < chk.tx.size(); i++)
	{
		const TxExp& e = chk.tx[i];
		const std::vector<uint8_t>& p = out.tx[i].bytes;
		std::cout << "[TB] TX packet " << i << (e.resend ? " (expected: resend)" : " (expected: first send)") << std::endl;
		if (p.size() < 12) { check(false, "  packet is shorter than a BTH"); continue; }

		const unsigned op  = p[0];
		const uint32_t dqp = get24(p, 5);
		const uint32_t psn = get24(p, 9);
		const bool     ar  = (p[8] & 0x80) != 0;
		const bool     rtx = (p[8] & 0x20) != 0;
		std::cout << "[TB]   " << opcodeName(op) << "  DestQP=" << hex(dqp) << "  PSN=" << hex(psn)
		          << "  AR=" << ar << "  rtx=" << rtx << std::endl;

		checkEq("  BTH.DestQP (remote QP)", dqp, reqRemoteQpn(e.qpn));
		checkEq("  BTH.PSN", psn, e.psn & 0xFFFFFF);

		std::map<std::pair<uint32_t, uint32_t>, ReqTbState::Sent>::const_iterator it =
			st.sent.find(std::make_pair(e.qpn, e.psn & 0xFFFFFF));
		check(it != st.sent.end(), "  the testbench posted a packet that got this PSN");
		if (it != st.sent.end())
		{
			const ReqTbState::Sent& s = it->second;
			checkEq("  BTH.opcode", op, s.op);
			const size_t off = txPayloadOffset(op);
			std::vector<uint8_t> payload;
			if (p.size() > off) payload.assign(p.begin() + off, p.end());
			checkCount("  payload length", payload.size(), s.len);
			check(payload == reqMemBytes(s.laddr, s.len), "  payload = host memory at the posted local address");
		}
		if (!e.resend)
			check(ar, "  BTH.AR set on a first send");
		else
			std::cout << "[TB]   (info) resend: BTH.rtx = " << rtx << " (MRC requires 1, 7.4.4; TX does not set it yet)" << std::endl;
		if (i < out.txMetas.size())
			checkCount("  tx meta UDP length = packet + ICRC", out.txMetas[i].length.to_uint(), p.size() + 4);
	}
	checkCount("memory reads issued (one per packet sent)", out.memReads.size(), chk.tx.size());

	// ---- completions for the host ----
	checkCount("completions (ackMeta) for the host", out.cpl.size(), chk.cpl.size());
	for (size_t i = 0; i < out.cpl.size() && i < chk.cpl.size(); i++)
	{
		const ackMeta& m = out.cpl[i].meta;
		std::cout << "[TB] Completion " << i << std::endl;
		checkEq("  ackMeta.qpn", m.qpn.to_uint(), chk.cpl[i].qpn);
		checkEq("  ackMeta.lst", m.lst.to_uint(), chk.cpl[i].lst ? 1 : 0);
	}

	// ---- which branches ran ----
	for (size_t i = 0; i < chk.logs.size(); i++)
		checkCount("module printed \"" + chk.logs[i].text + "\"", dut.logCount(chk.logs[i].text), chk.logs[i].count);

	// ---- always ----
	if (!reqHasLogExp(chk, "TIMER Retransmit triggered"))
		checkCount("no Local ACK timeout during the group", dut.logCount("TIMER Retransmit triggered"), 0);
	checkCount("no memory writes (requestor test)", out.memWrites, 0);
	check(dut.s_axis_rx_data.empty() && dut.s_axis_rx_meta.empty(), "module consumed the whole RX input");
	check(dut.s_axis_sq_meta.empty(), "module consumed every posted request");
}

static void runReqGroup(Dut& dut, ReqTbState& st, const std::vector<ReqRow>& rows, size_t begin, size_t end)
{
	const ReqRow& chk = rows[end];
	banner("REQUESTOR GROUP: " + chk.name);

	ReqOutputs out;
	dut.clearLog();
	bool aborted = false;

	for (size_t i = begin; i < end && !aborted; i++)
	{
		const ReqRow& r = rows[i];
		if (r.kind == RQ_POST)
		{
			const uint32_t psn = st.nextPsn[r.qpn];
			st.nextPsn[r.qpn] = (psn + 1) & 0xFFFFFF;
			ReqTbState::Sent s = { r.op, r.laddr, r.len };
			st.sent[std::make_pair(r.qpn, psn)] = s;

			std::cout << cycTag() << "[TB] post " << r.name << ": qp " << hex(r.qpn) << " " << opcodeName(r.op)
			          << " laddr=" << hex(r.laddr) << " raddr=" << hex(r.raddr) << " len=" << std::dec << r.len
			          << " lst=" << r.lst << "  -> should get PSN " << hex(psn) << std::endl;
			dut.s_axis_sq_meta.write(txMeta(r.op, r.qpn, 1, r.lst ? 1 : 0, 0, r.laddr, r.raddr, r.len, 0));
			reqTick(dut, out);
		}
		else if (r.kind == RQ_RX)
		{
			aborted = !feedReqCtrl(dut, out, r);
		}
		for (int c = 0; c < r.cycles; c++) reqTick(dut, out);
	}

	std::cout << "[TB] Inputs done, draining " << std::dec << chk.cycles << " cycles..." << std::endl;
	for (int c = 0; c < chk.cycles; c++) reqTick(dut, out);

	if (aborted) std::cout << "[TB] (group aborted, the checks below are expected to fail)" << std::endl;
	checkReqGroup(dut, st, chk, out);
}

static void runRequestorScenario(Dut& dut, const std::vector<ReqRow>& rows)
{
	banner("REQUESTOR SCENARIO (step C): the module sends writes, the testbench plays the responder");
	ReqTbState st;
	setupRequestorQps(dut, st);

	size_t begin = 0;
	for (size_t i = 0; i < rows.size(); i++)
	{
		if (rows[i].kind == RQ_CHECK)
		{
			runReqGroup(dut, st, rows, begin, i);
			begin = i + 1;
		}
	}
	if (begin < rows.size())
		std::cout << "[TB] WARNING: rows after the last ReqCheck(...) were not run" << std::endl;
}