#pragma once

/*
 * test_requestor_pkts.hpp  --  requestor scenario: what the module must do with ACK / SACK / NACK
 * -------------------------------------------------------------------------------------------------
 * The module is the REQUESTOR of the writes posted below; the testbench plays the responder.
 * A group is every row up to the next ReqCheck(...). Rows:
 *
 *   ReqPost(name, qpn, opcode, laddr, raddr, len, lst [, idleAfter])
 *       one packet of a write on s_axis_sq_meta. The module gives it the QP's next PSN.
 *       lst = 1 on the last packet of the WR: it comes back in the completion (ackMeta.lst).
 *   ReqRx(name, packet [, idleAfter])
 *       a control packet from the responder: ReqAck(qpn, cack [, syndrome]), ReqSack(qpn, cack),
 *       ReqNack(qpn, reason, psn).
 *   ReqIdle(cycles)
 *   ReqCheck(name, { TX packets }, { completions }, { prints } [, drain])
 *       TX packets  : { qpn, psn, resend } in the order they leave the module
 *       completions : { qpn, lst } in the order they reach the host
 *       prints      : { "text", count }: how often the module printed text during the group
 *
 * PSNs: psnA(k) = k-th packet QP A sends. QP A starts at 0xFFFFFD, so psnA(3) = 0x000000:
 * every comparison in rx_req_fsm and the retransmitter is exercised across the 24-bit wrap.
 * (Set REQ_FIRST_PSN_A to e.g. 0x000200 in tb_config.hpp to debug without the wrap.)
 *
 * Timing: in csim the Local ACK timer fires after about 10 x MAX_QPS cycles without progress.
 * Groups are kept short (REQ_DRAIN_CYCLES) so it never fires unless a test wants it to.
 *
 * QP A ends in ERROR (retry limit), QP B ends in ERROR (AETH NAK), QP C ends in ERROR (UNEXP_EVENT).
 * The error state is sticky, so each error test uses its own QP and comes last for that QP.
 * -------------------------------------------------------------------------------------------------
 */

#include <vector>

#include "tests_requestor.hpp"

static const uint32_t QA = REQ_QPN_A;
static const uint32_t QB = REQ_QPN_B;
static const uint32_t QC = REQ_QPN_C;

static const std::vector<ReqRow> REQ_SCENARIO = {

	// =============================================================================================
	// QP A: transport ACK basics
	// =============================================================================================

	// ---- R01: a first send gets the next PSN, AR set, no completion yet ----
	ReqPost ("R01 ONLY 256 B",                       QA, MRC_RDMA_WRITE_ONLY, 0x1000, 0x90000, 256, true),
	ReqCheck("R01: post one ONLY write",
	         { {QA, psnA(0), false} },
	         { },
	         { }),

	// ---- R02: ACK at its PSN completes it ----
	ReqRx   ("R02 ACK cack=psnA(0)",                 ReqAck(QA, psnA(0))),
	ReqCheck("R02: ACK completes the write",
	         { },
	         { {QA, true} },
	         { {"]: ACK qp", 1}, {"released psn", 1} }),

	// ---- R03: the same ACK again releases nothing ----
	ReqRx   ("R03 ACK cack=psnA(0) again",           ReqAck(QA, psnA(0))),
	ReqCheck("R03: duplicate ACK is harmless",
	         { },
	         { },
	         { {"]: ACK qp", 1}, {"released psn", 0} }),

	// ---- R04: an ACK beyond anything sent is ignored ----
	ReqRx   ("R04 ACK cack=psnA(5)",                 ReqAck(QA, psnA(5))),
	ReqCheck("R04: ACK for a PSN never sent is ignored",
	         { },
	         { },
	         { {"acknowledges a PSN never sent", 1}, {"released psn", 0} }),

	// =============================================================================================
	// QP A: SACK vs ACK, posted order, single resend. psnA(3) is 0x000000 (wrap).
	// =============================================================================================

	// ---- R05: three WRs: ONLY (A1), FIRST+LAST (A2, A3), ONLY (A4) ----
	ReqPost ("R05 WR1 ONLY 256 B",                   QA, MRC_RDMA_WRITE_ONLY,  0x2000,        0x91000,        256,  true),
	ReqPost ("R05 WR2 FIRST PMTU",                   QA, MRC_RDMA_WRITE_FIRST, 0x3000,        0x92000,        PMTU, false),
	ReqPost ("R05 WR2 LAST 100 B",                   QA, MRC_RDMA_WRITE_LAST,  0x3000 + PMTU, 0x92000 + PMTU, 100,  true),
	ReqPost ("R05 WR3 ONLY 256 B",                   QA, MRC_RDMA_WRITE_ONLY,  0x5000,        0x93000,        256,  true),
	ReqCheck("R05: four packets, consecutive PSNs across the wrap",
	         { {QA, psnA(1), false}, {QA, psnA(2), false}, {QA, psnA(3), false}, {QA, psnA(4), false} },
	         { },
	         { },
	         2 * REQ_DRAIN_CYCLES),

	// ---- R06: a SACK moves unack_psn but never completes and never releases (7.2) ----
	ReqRx   ("R06 SACK cack=psnA(2)",                ReqSack(QA, psnA(2))),
	ReqCheck("R06: SACK is reliability only",
	         { },
	         { },
	         { {"]: SACK qp", 1}, {"released psn", 0} }),

	// ---- R07: the ACK for the same cack_psn still completes, in posted order ----
	ReqRx   ("R07 ACK cack=psnA(2)",                 ReqAck(QA, psnA(2))),
	ReqCheck("R07: ACK completes WR1 and the FIRST of WR2, even after the SACK",
	         { },
	         { {QA, true}, {QA, false} },
	         { {"]: ACK qp", 1}, {"released psn", 2} }),

	// ---- R08: NACK TRIMMED resends exactly that packet, nothing else ----
	ReqRx   ("R08 NACK TRIMMED psnA(4)",             ReqNack(QA, NACK_TRIMMED, psnA(4))),
	ReqCheck("R08: NACK resends only psnA(4)",
	         { {QA, psnA(4), true} },
	         { },
	         { {"]: NACK qp", 1}, {"(single)", 1}, {"released psn", 0} }),

	// ---- R09: NACK for a packet already acknowledged ----
	ReqRx   ("R09 NACK TRIMMED psnA(1)",             ReqNack(QA, NACK_TRIMMED, psnA(1))),
	ReqCheck("R09: NACK for an acknowledged PSN is ignored",
	         { },
	         { },
	         { {"not outstanding", 1}, {"(single)", 0} }),

	// ---- R10: NACK with a reserved reason code ----
	ReqRx   ("R10 NACK reason 0x33 psnA(3)",         ReqNack(QA, 0x33, psnA(3))),
	ReqCheck("R10: unknown NACK reason is ignored",
	         { },
	         { },
	         { {"unknown NACK reason", 1}, {"(single)", 0} }),

	// ---- R11: ACK for the last PSN completes the rest; the timer stops ----
	ReqRx   ("R11 ACK cack=psnA(4)",                 ReqAck(QA, psnA(4))),
	ReqCheck("R11: ACK completes the LAST of WR2 and WR3",
	         { },
	         { {QA, true}, {QA, true} },
	         { {"released psn", 2} }),

	// =============================================================================================
	// QP A: NACK retry limit -> ERROR (Table 7-3). The counter was reset by the ACK in R11.
	// =============================================================================================

	// ---- R12: 7 retriable NACKs are resent, the 8th puts the QP in ERROR ----
	ReqPost ("R12 ONLY 256 B",                       QA, MRC_RDMA_WRITE_ONLY, 0x6000, 0x94000, 256, true, REQ_NACK_GAP),
	ReqRx   ("R12 NACK 1",                           ReqNack(QA, NACK_TRIMMED,         psnA(5)), REQ_NACK_GAP),
	ReqRx   ("R12 NACK 2",                           ReqNack(QA, NACK_TRIMMED_LASTHOP, psnA(5)), REQ_NACK_GAP),
	ReqRx   ("R12 NACK 3",                           ReqNack(QA, NACK_NO_BITMAP,       psnA(5)), REQ_NACK_GAP),
	ReqRx   ("R12 NACK 4",                           ReqNack(QA, NACK_NO_PKT_BUFFER,   psnA(5)), REQ_NACK_GAP),
	ReqRx   ("R12 NACK 5",                           ReqNack(QA, NACK_NO_RESOURCE,     psnA(5)), REQ_NACK_GAP),
	ReqRx   ("R12 NACK 6",                           ReqNack(QA, NACK_PSN_OOR_WINDOW,  psnA(5)), REQ_NACK_GAP),
	ReqRx   ("R12 NACK 7",                           ReqNack(QA, NACK_TRIMMED,         psnA(5)), REQ_NACK_GAP),
	ReqRx   ("R12 NACK 8 (limit)",                   ReqNack(QA, NACK_TRIMMED,         psnA(5))),
	ReqCheck("R12: retry limit reached -> QP A in ERROR",
	         { {QA, psnA(5), false},
	           {QA, psnA(5), true}, {QA, psnA(5), true}, {QA, psnA(5), true}, {QA, psnA(5), true},
	           {QA, psnA(5), true}, {QA, psnA(5), true}, {QA, psnA(5), true} },
	         { },
	         { {"]: NACK qp", 8}, {"(single)", 7}, {"in ERROR", 1} }),

	// ---- R13: after the error, an ACK is ignored ----
	ReqRx   ("R13 ACK cack=psnA(5)",                 ReqAck(QA, psnA(5))),
	ReqCheck("R13: QP A in ERROR ignores the ACK",
	         { },
	         { },
	         { {"]: ACK qp", 1}, {"released psn", 0} }),

	// =============================================================================================
	// QP B: AETH RNR NAK is ignored, AETH NAK is a Class B error (Tables 6-11, 6-12)
	// =============================================================================================

	// ---- R14: two ONLY writes ----
	ReqPost ("R14 ONLY 128 B",                       QB, MRC_RDMA_WRITE_ONLY, 0x7000, 0xA0000, 128, true),
	ReqPost ("R14 ONLY 128 B",                       QB, MRC_RDMA_WRITE_ONLY, 0x7100, 0xA1000, 128, true),
	ReqCheck("R14: post two writes on QP B",
	         { {QB, psnB(0), false}, {QB, psnB(1), false} },
	         { },
	         { }),

	// ---- R15: RNR NAK does not exist in MRC ----
	ReqRx   ("R15 AETH RNR NAK psnB(0)",             ReqAck(QB, psnB(0), 0x20)),
	ReqCheck("R15: RNR NAK is ignored",
	         { },
	         { },
	         { {"RNR/reserved AETH type", 1}, {"released psn", 0}, {"in ERROR", 0} }),

	// ---- R16: a normal ACK still works ----
	ReqRx   ("R16 ACK cack=psnB(0)",                 ReqAck(QB, psnB(0))),
	ReqCheck("R16: ACK completes the first write",
	         { },
	         { {QB, true} },
	         { {"released psn", 1} }),

	// ---- R17: NAK Remote Access Error: ERROR, no resend ----
	ReqRx   ("R17 AETH NAK 0x62 psnB(1)",            ReqAck(QB, psnB(1), 0x62)),
	ReqCheck("R17: AETH NAK -> QP B in ERROR, nothing resent",
	         { },
	         { },
	         { {"-> QP error", 1}, {"in ERROR", 1}, {"retransmitting psn", 0} }),

	// ---- R18: after the error, an ACK is ignored ----
	ReqRx   ("R18 ACK cack=psnB(1)",                 ReqAck(QB, psnB(1))),
	ReqCheck("R18: QP B in ERROR ignores the ACK",
	         { },
	         { },
	         { {"]: ACK qp", 1}, {"released psn", 0} }),

	// =============================================================================================
	// QP C: NACK UNEXP_EVENT -> ERROR without resend (Table 7-3)
	// =============================================================================================

	ReqPost ("R19 ONLY 128 B",                       QC, MRC_RDMA_WRITE_ONLY, 0x8000, 0xB0000, 128, true, REQ_NACK_GAP),
	ReqRx   ("R19 NACK UNEXP_EVENT psnC(0)",         ReqNack(QC, NACK_UNEXP_EVENT, psnC(0))),
	ReqCheck("R19: UNEXP_EVENT -> QP C in ERROR, nothing resent",
	         { {QC, psnC(0), false} },
	         { },
	         { {"]: NACK qp", 1}, {"in ERROR", 1}, {"(single)", 0} }),

	// =============================================================================================
	// Quiet period: every timer was stopped (all acknowledged, or QP in ERROR)
	// =============================================================================================

	ReqIdle (3000),
	ReqCheck("R20: no timeout resend after 3000 idle cycles",
	         { },
	         { },
	         { {"TIMER Retransmit triggered", 0} }),
};