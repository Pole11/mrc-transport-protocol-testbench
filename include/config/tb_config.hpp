#pragma once

#include <cstdint>
#include <iostream>

#include "rocev2_config.hpp"

// --- Our QP (the one inside the module, i.e. the RESPONDER) -------------------------------------
static const uint32_t LOCAL_QPN        = 0x05;       // must be < MAX_QPS; used as table index
static const uint32_t FIRST_RX_PSN     = 0x0A0B0C;   // PSN the module expects on the first packet
static const uint32_t LOCAL_TX_PSN     = 0x000100;   // PSN the module would use when IT sends (unused here)
static const uint32_t RKEY             = 0x1234;     // stored by the module, NOT checked on RX WRITE

// --- The remote side (played by this testbench, i.e. the REQUESTER) ------------------------------
static const uint32_t REMOTE_QPN       = 0x0042;     // module must put this in the ACK's BTH.DestQP
static const uint64_t REMOTE_IP        = 0x0A000002; // 10.0.0.2 (128-bit field, IPv4 in low 32 bits)
static const uint16_t REMOTE_UDP_PORT  = 0xC000;     // requester's UDP source port

// --- Simulation ------------------------------------------------------------------------------------
static const int      CYCLES_PER_TEST  = 300;        // DUT calls after the last packet of a group (DRAIN),
                                                     // and how long the RX input may stall before failing
static const size_t   RX_INPUT_DEPTH   = 2;          // beats the TB may have waiting at s_axis_rx_data:
                                                     // a new beat is pushed only when there is room (TREADY)
static const int      RX_META_LAG      = 0;          // cycles between a packet's first beat and its ipUdpMeta
static const bool     PRINT_DBG_STREAMS = true;     // also print the m_axis_dbg_* (psnPkg) streams
static const size_t   MAX_DUMP_BYTES   = 64;         // hex dumps longer than this are truncated
static const bool     RUN_MRC_CTRL_UNIT_TESTS = true; // parser unit checks of the MRC control headers

static const int BYTES_PER_BEAT = DATA_WIDTH / 8;    // 64 for a 512-bit data path

// --- Requestor scenario (step C): QPs of the module that SEND writes; the TB plays their responder ---
static const bool     RUN_REQUESTOR_TESTS = true;
static const int      REQ_DRAIN_CYCLES    = 200;  // short groups: the csim Local ACK timer fires after ~10 x MAX_QPS cycles
static const int      REQ_NACK_GAP        = 80;   // idle cycles after a post / between NACKs: each resend completes first
