#pragma once

#include "axi_utils.hpp"
#include "mrc_transport_protocol/mrc_transport_protocol.hpp"
#include "rocev2_config.hpp"

// non-template top for Vitis HLS (a template cannot be the top): it is mrc_transport_protocol<DATA_WIDTH, 0>
void mrc_transport_protocol_top(
hls::stream<ipUdpMeta>&	s_axis_rx_meta,
	hls::stream<net_axis<DATA_WIDTH> >& s_axis_rx_data,
	hls::stream<ipUdpMeta>&	m_axis_tx_meta,
	hls::stream<net_axis<DATA_WIDTH> >& m_axis_tx_data,
	hls::stream<txMeta>& s_axis_sq_meta,
	hls::stream<ackMeta>& m_axis_rx_ack_meta,
	hls::stream<memCmd>& m_axis_mem_write_cmd,
	hls::stream<memCmd>& m_axis_mem_read_cmd,
	hls::stream<net_axis<DATA_WIDTH> >& m_axis_mem_write_data,
	hls::stream<net_axis<DATA_WIDTH> >& s_axis_mem_read_data,
	hls::stream<qpContext>&	s_axis_qp_interface,
	hls::stream<ifConnReq>&	s_axis_qp_conn_interface,
#ifdef DBG_IBV
	hls::stream<psnPkg>& m_axis_dbg_0,
	hls::stream<psnPkg>& m_axis_dbg_1,
	hls::stream<psnPkg>& m_axis_dbg_2,
#endif
	ap_uint<32>& regInvalidPsnDropCount,
	ap_uint<32>& regRetransCount,
	ap_uint<32>& regIbvCountRx,
	ap_uint<32>& regIbvCountTx
);