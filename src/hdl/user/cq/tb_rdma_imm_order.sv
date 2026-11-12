`timescale 1ns / 1ps

import lynxTypes::*;

// rdma_imm_order: every ImmDt comes out unchanged, in order, and only after the write completion of its group.
// A group is the ImmDts released by one packet (last = 1 on its final one); the n-th completion with LOCAL_WRITE and
// host = 0 belongs to the n-th group. Each group's completion arrives at random before, during or after its ImmDts.
// Completions that must not count (RDMA ACKs, host-issued writes, reads) are mixed in, and the output stalls at random.
// The queue is small (8), so the input is also backpressured
module tb_rdma_imm_order;

localparam integer N_GROUPS = 1000;
localparam integer MAX_IMMS = 4 * N_GROUPS;

logic aclk = 1'b0;
logic aresetn = 1'b0;
always #2 aclk = ~aclk;

metaIntf #(.STYPE(imm_t)) s_imm (.aclk(aclk), .aresetn(aresetn));
metaIntf #(.STYPE(ack_t)) s_cq_wr (.aclk(aclk), .aresetn(aresetn));
metaIntf #(.STYPE(imm_t)) m_imm (.aclk(aclk), .aresetn(aresetn));

rdma_imm_order #(.QDEPTH(8)) inst_dut (.aclk(aclk), .aresetn(aresetn), .s_imm(s_imm), .s_cq_wr(s_cq_wr), .m_imm(m_imm));

// the testbench drives these, the interfaces only through the assigns
logic imm_valid = 1'b0;
imm_t imm_data = '0;
logic cq_valid = 1'b0;
ack_t cq_data = '0;
logic cq_real = 1'b0;       // the completion on cq_wr is the one of a group (not noise)
logic out_ready = 1'b0;

assign s_imm.valid   = imm_valid;
assign s_imm.data    = imm_data;
assign s_cq_wr.valid = cq_valid;
assign s_cq_wr.data  = cq_data;
assign m_imm.ready   = out_ready;

// plan: the ImmDts of every group, and when each group's completion may be sent
imm_t imms [MAX_IMMS];
int group_of [MAX_IMMS];
int first [N_GROUPS];       // index of the group's first ImmDt
int last [N_GROUPS];        // index of the group's last ImmDt
int mode [N_GROUPS];        // completion: 0 any time, 1 after its first ImmDt is taken, 2 after all its ImmDts are taken
int n_imms = 0;
int errors = 0;

initial begin
    for (int g = 0; g < N_GROUPS; g++) begin
        int size;
        size = $urandom_range(1, 4);
        first[g] = n_imms;
        for (int k = 0; k < size; k++) begin
            imms[n_imms]      = '0;
            imms[n_imms].imm  = {16'(g), 8'(k), 8'($urandom)};
            imms[n_imms].pid  = $urandom;
            imms[n_imms].last = (k == size - 1);
            group_of[n_imms]  = g;
            n_imms++;
        end
        last[g] = n_imms - 1;
        mode[g] = $urandom_range(0, 2);
    end
end

// ImmDt source, in order: imm_taken = ImmDts the DUT has taken
int imm_next = 0;
int imm_taken = 0;
always @(posedge aclk) begin
    if (aresetn) begin
        if (imm_valid && s_imm.ready)
            imm_taken <= imm_taken + 1;
        if (!imm_valid || s_imm.ready) begin
            if (imm_next < n_imms && $urandom_range(0, 3) != 0) begin
                imm_valid <= 1'b1;
                imm_data  <= imms[imm_next];
                imm_next  <= imm_next + 1;
            end
            else begin
                imm_valid <= 1'b0;
            end
        end
    end
end

// completions: the groups' ones in order, each when its mode allows it, plus noise that must not be counted
int cpl_next = 0;
always @(posedge aclk) begin
    if (aresetn) begin
        ack_t c;
        c = '0;
        c.pid  = $urandom;
        c.dest = $urandom;
        c.strm = STRM_HOST;
        if (cpl_next < N_GROUPS && $urandom_range(0, 2) == 0 &&
            (mode[cpl_next] == 0 || (mode[cpl_next] == 1 && imm_taken > first[cpl_next]) || (mode[cpl_next] == 2 && imm_taken > last[cpl_next]))) begin
            c.opcode = LOCAL_WRITE;
            cq_data  <= c;
            cq_valid <= 1'b1;
            cq_real  <= 1'b1;
            cpl_next <= cpl_next + 1;
        end
        else if ($urandom_range(0, 5) == 0) begin
            case ($urandom_range(0, 2))
                0: c.opcode = RC_ACK;                                   // ACK of a write this node sent
                1: begin c.opcode = LOCAL_WRITE; c.host = 1'b1; end     // write issued by the host, not by the vFPGA
                2: c.opcode = LOCAL_READ;
            endcase
            cq_data  <= c;
            cq_valid <= 1'b1;
            cq_real  <= 1'b0;
        end
        else begin
            cq_valid <= 1'b0;
            cq_real  <= 1'b0;
        end
    end
end

// checker: cpl_seen = group completions taken at earlier edges, so an ImmDt of group g needs cpl_seen > g
int cpl_seen = 0;
int out_cnt = 0;
always @(posedge aclk) begin
    if (aresetn) begin
        if (cq_valid && !s_cq_wr.ready) begin
            $display("[FAIL] cq_wr not ready: a completion would be lost");
            errors++;
        end
        if (cq_valid && cq_real)
            cpl_seen <= cpl_seen + 1;
        if (m_imm.valid && m_imm.ready) begin
            if (out_cnt >= n_imms) begin
                $display("[FAIL] extra ImmDt %h", m_imm.data.imm);
                errors++;
            end
            else begin
                if (m_imm.data != imms[out_cnt]) begin
                    $display("[FAIL] ImmDt %0d: got %h last %0d, expected %h last %0d", out_cnt,
                             m_imm.data.imm, m_imm.data.last, imms[out_cnt].imm, imms[out_cnt].last);
                    errors++;
                end
                if (group_of[out_cnt] >= cpl_seen) begin
                    $display("[FAIL] ImmDt %0d of group %0d released after only %0d completion(s)", out_cnt, group_of[out_cnt], cpl_seen);
                    errors++;
                end
            end
            out_cnt <= out_cnt + 1;
        end
        out_ready <= ($urandom_range(0, 3) != 0);
    end
end

initial begin
    int cycles;
    repeat (4) @(posedge aclk);
    aresetn <= 1'b1;

    // every ImmDt released, or timeout (a completion lost or not counted leaves its group held forever)
    cycles = 0;
    do begin
        @(posedge aclk);
        cycles++;
    end while ((out_cnt < n_imms || cpl_next < N_GROUPS) && cycles < 50 * MAX_IMMS);

    if (cycles == 50 * MAX_IMMS) begin
        $display("[FAIL] stalled: %0d of %0d ImmDts released, %0d of %0d completions sent", out_cnt, n_imms, cpl_next, N_GROUPS);
        errors++;
    end
    repeat (20) @(posedge aclk);

    $display("%0d groups, %0d ImmDts released", N_GROUPS, out_cnt);
    if (errors == 0) $display("PASS");
    else             $display("FAIL: %0d errors", errors);
    $finish;
end

endmodule