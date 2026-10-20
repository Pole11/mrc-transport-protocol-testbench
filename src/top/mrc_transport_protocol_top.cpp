#include "mrc_transport_protocol_top.hpp"

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
	ap_uint<32>& regIbvCountTx)
{
#pragma HLS DATAFLOW disable_start_propagation
	mrc_transport_protocol<DATA_WIDTH, 0>(
		s_axis_rx_meta, s_axis_rx_data,
		m_axis_tx_meta, m_axis_tx_data,
		s_axis_sq_meta,
		m_axis_rx_ack_meta,
		m_axis_mem_write_cmd, m_axis_mem_read_cmd,
		m_axis_mem_write_data, s_axis_mem_read_data,
		s_axis_qp_interface, s_axis_qp_conn_interface,
#ifdef DBG_IBV
		m_axis_dbg_0, m_axis_dbg_1, m_axis_dbg_2,
#endif
		regInvalidPsnDropCount, regRetransCount, regIbvCountRx, regIbvCountTx);
}