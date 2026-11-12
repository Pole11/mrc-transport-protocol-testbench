`timescale 1ns / 1ps

import lynxTypes::*;

// rdma_imm_arbiter: every ImmDt reaches the vFPGA of its vfid, unchanged and in order, and no other vFPGA.
// An ImmDt whose vfid has no vFPGA (vfid >= N_REGIONS) is consumed and never appears on an output.
// The source and the vFPGAs stall at random: an ImmDt must never be lost or duplicated while its vFPGA stalls
module tb_rdma_imm_arbiter;

localparam integer N_IMMS = 3000;

logic aclk = 1'b0;
logic aresetn = 1'b0;
always #2 aclk = ~aclk;

metaIntf #(.STYPE(imm_t)) s_meta (.aclk(aclk), .aresetn(aresetn));
metaIntf #(.STYPE(imm_t)) m_meta [N_REGIONS] (.aclk(aclk), .aresetn(aresetn));

rdma_imm_arbiter inst_dut (.s_meta(s_meta), .m_meta(m_meta));

// the testbench drives these, the interfaces only through the assigns
logic src_valid = 1'b0;
imm_t src_data = '0;
logic [N_REGIONS-1:0] snk_ready = '0;

assign s_meta.valid = src_valid;
assign s_meta.data  = src_data;

// per vFPGA, the ImmDts in the order they must arrive: exp_wr written by the source, exp_rd checked by the vFPGA
imm_t expected [N_REGIONS][N_IMMS];
int exp_wr [N_REGIONS];
int exp_rd [N_REGIONS];
int sent = 0;
int dropped = 0;
int received [N_REGIONS];
int errors = 0;

// source: a new ImmDt only after the previous one has been taken. vfid N_REGIONS and all ones have no vFPGA
always @(posedge aclk) begin
    if (aresetn && (!src_valid || s_meta.ready)) begin
        if (sent < N_IMMS && $urandom_range(0, 3) != 0) begin
            imm_t imm;
            int r;
            r = $urandom_range(0, N_REGIONS + 1);
            imm       = '0;
            imm.vfid  = (r == N_REGIONS + 1) ? '1 : r;
            imm.pid   = $urandom;
            imm.imm   = {16'(sent), 16'($urandom)}; // unique, so a lost or duplicated ImmDt shows as a mismatch
            imm.last  = $urandom;
            if (imm.vfid < N_REGIONS) begin
                expected[imm.vfid][exp_wr[imm.vfid]] = imm;
                exp_wr[imm.vfid]++;
            end
            else begin
                dropped++;
            end
            src_valid <= 1'b1;
            src_data  <= imm;
            sent++;
        end
        else begin
            src_valid <= 1'b0;
        end
    end
end

// vFPGAs: random ready, every ImmDt taken is checked against the next one expected
for (genvar i = 0; i < N_REGIONS; i++) begin
    assign m_meta[i].ready = snk_ready[i];

    always @(posedge aclk) begin
        if (aresetn) begin
            if (m_meta[i].valid && m_meta[i].ready) begin
                if (exp_rd[i] == exp_wr[i]) begin
                    $display("[FAIL] vFPGA %0d: unexpected ImmDt %h", i, m_meta[i].data.imm);
                    errors++;
                end
                else begin
                    imm_t exp;
                    exp = expected[i][exp_rd[i]];
                    exp_rd[i]++;
                    if (m_meta[i].data != exp) begin
                        $display("[FAIL] vFPGA %0d: got vfid %0d pid %0d ImmDt %h last %0d, expected vfid %0d pid %0d ImmDt %h last %0d", i,
                                 m_meta[i].data.vfid, m_meta[i].data.pid, m_meta[i].data.imm, m_meta[i].data.last,
                                 exp.vfid, exp.pid, exp.imm, exp.last);
                        errors++;
                    end
                end
                received[i]++;
            end
            snk_ready[i] <= ($urandom_range(0, 3) != 0);
        end
    end
end

initial begin
    int cycles;
    int pending;
    repeat (4) @(posedge aclk);
    aresetn <= 1'b1;

    // all ImmDts sent and taken, or timeout (an ImmDt without vFPGA that is not dropped stalls the source)
    cycles = 0;
    do begin
        @(posedge aclk);
        cycles++;
        pending = 0;
        for (int i = 0; i < N_REGIONS; i++) pending += exp_wr[i] - exp_rd[i];
    end while ((sent < N_IMMS || src_valid || pending != 0) && cycles < 20 * N_IMMS);

    if (cycles == 20 * N_IMMS) begin
        $display("[FAIL] stalled after %0d of %0d ImmDts, %0d not received", sent, N_IMMS, pending);
        errors++;
    end
    repeat (8) @(posedge aclk);

    for (int i = 0; i < N_REGIONS; i++) $display("vFPGA %0d: %0d ImmDts", i, received[i]);
    $display("without vFPGA (dropped): %0d", dropped);
    if (errors == 0) $display("PASS: %0d ImmDts, %0d vFPGAs", sent, N_REGIONS);
    else             $display("FAIL: %0d errors", errors);
    $finish;
end

endmodule