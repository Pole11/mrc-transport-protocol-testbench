`timescale 1ns / 1ps

import lynxTypes::*;

// rdma_imm_csr: every read of register 0 returns the oldest completion ({1, qpn, imm}) and removes it, or 0 when there is
// none; reads of another register return 0 and remove nothing; writes get an OKAY response and change nothing.
// Completions arrive at random and the queue is small (4), so the input is also backpressured. The host side issues
// reads and writes at random, as getCSR / setCSR would
module tb_rdma_imm_csr;

localparam integer N_IMMS = 2000;

logic aclk = 1'b0;
logic aresetn = 1'b0;
always #2 aclk = ~aclk;

metaIntf #(.STYPE(imm_t)) s_imm (.aclk(aclk), .aresetn(aresetn));
AXI4L axi_ctrl (.aclk(aclk), .aresetn(aresetn));

rdma_imm_csr #(.QDEPTH(4)) inst_dut (.aclk(aclk), .aresetn(aresetn), .s_imm(s_imm), .axi_ctrl(axi_ctrl));

// the testbench drives these, the interfaces only through the assigns
logic imm_valid = 1'b0;
imm_t imm_data = '0;
logic arvalid = 1'b0;
logic [AXI_ADDR_BITS-1:0] araddr = '0;
logic rready = 1'b0;
logic awvalid = 1'b0;
logic wvalid = 1'b0;
logic bready = 1'b0;

assign s_imm.valid       = imm_valid;
assign s_imm.data        = imm_data;
assign axi_ctrl.arvalid  = arvalid;
assign axi_ctrl.araddr   = araddr;
assign axi_ctrl.arprot   = '0;
assign axi_ctrl.arqos    = '0;
assign axi_ctrl.arregion = '0;
assign axi_ctrl.rready   = rready;
assign axi_ctrl.awvalid  = awvalid;
assign axi_ctrl.awaddr   = '0;
assign axi_ctrl.awprot   = '0;
assign axi_ctrl.awqos    = '0;
assign axi_ctrl.awregion = '0;
assign axi_ctrl.wvalid   = wvalid;
assign axi_ctrl.wdata    = '1;
assign axi_ctrl.wstrb    = '1;
assign axi_ctrl.bready   = bready;

imm_t imms [N_IMMS];
int errors = 0;

initial begin
    for (int k = 0; k < N_IMMS; k++) begin
        imms[k]      = '0;
        imms[k].imm  = {16'(k), 16'($urandom)};
        imms[k].pid  = $urandom;
        imms[k].vfid = $urandom;
        imms[k].last = $urandom;
    end
end

// completions source, in order: phases of 250 faster than the host reads (queue full) and slower (queue empty)
int imm_next = 0;
always @(posedge aclk) begin
    if (aresetn && (!imm_valid || s_imm.ready)) begin
        if (imm_next < N_IMMS && $urandom_range(0, ((imm_next / 250) % 2) ? 1 : 30) == 0) begin
            imm_valid <= 1'b1;
            imm_data  <= imms[imm_next];
            imm_next  <= imm_next + 1;
        end
        else begin
            imm_valid <= 1'b0;
        end
    end
end

// host: one read (register 0 or 1) or one write at a time, then waits for its response; got = completions read
int got = 0;
int empty_reads = 0;
logic rd_reg0 = 1'b0;
logic pending = 1'b0;       // a transaction waits for its response (R or B)
always @(posedge aclk) begin
    if (aresetn) begin
        if (arvalid && axi_ctrl.arready) arvalid <= 1'b0;
        if (awvalid && axi_ctrl.awready) awvalid <= 1'b0;
        if (wvalid && axi_ctrl.wready)   wvalid  <= 1'b0;

        if ((axi_ctrl.rvalid && rready) || (axi_ctrl.bvalid && bready))
            pending <= 1'b0;
        if (axi_ctrl.rvalid && rready) begin
            logic [63:0] exp;
            exp = '0;
            if (rd_reg0 && axi_ctrl.rdata[63]) begin
                exp = {1'b1, 15'b0, {(16-DEST_BITS-PID_BITS){1'b0}}, imms[got].vfid, imms[got].pid, imms[got].imm};
                got <= got + 1;
            end
            else if (rd_reg0) begin
                empty_reads <= empty_reads + 1;
            end
            if (axi_ctrl.rdata != exp || axi_ctrl.rresp != 2'b00) begin
                $display("[FAIL] read of register %0d: got %h resp %0d, expected %h", !rd_reg0, axi_ctrl.rdata, axi_ctrl.rresp, exp);
                errors++;
            end
        end
        if (axi_ctrl.bvalid && bready && axi_ctrl.bresp != 2'b00) begin
            $display("[FAIL] write response %0d", axi_ctrl.bresp);
            errors++;
        end

        rready <= ($urandom_range(0, 1) == 0);
        bready <= ($urandom_range(0, 1) == 0);

        // a new transaction once the previous one has completed
        if (!pending && $urandom_range(0, 3) == 0) begin
            pending <= 1'b1;
            case ($urandom_range(0, 5))
                0:       begin awvalid <= 1'b1; wvalid <= 1'b1; end
                1:       begin arvalid <= 1'b1; araddr <= 8; rd_reg0 <= 1'b0; end
                default: begin arvalid <= 1'b1; araddr <= 0; rd_reg0 <= 1'b1; end
            endcase
        end
    end
end

initial begin
    int cycles;
    repeat (4) @(posedge aclk);
    aresetn <= 1'b1;

    cycles = 0;
    do begin
        @(posedge aclk);
        cycles++;
    end while (got < N_IMMS && cycles < 100 * N_IMMS);

    if (got < N_IMMS) begin
        $display("[FAIL] stalled: %0d of %0d completions read", got, N_IMMS);
        errors++;
    end
    repeat (40) @(posedge aclk);

    $display("%0d completions read, %0d reads found the queue empty", got, empty_reads);
    if (errors == 0) $display("PASS");
    else             $display("FAIL: %0d errors", errors);
    $finish;
end

endmodule