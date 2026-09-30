// Testbench top for the cross-link fixture. Module name is `top` so that
// hierarchical signal paths look like `top.dut.u_fifo.count`.
//
// Timeline (clk period 10 ns, posedges at 5 + 10*k ns, timescale 1ns/1ps):
//   0    - 33 ns  : reset
//   33   - 2000 ns: normal traffic (sink accepts 2 of 3 cycles, ready toggles)
//   2000 - 3000 ns: sink stalled -> FIFO fills -> s_ready stuck at 0
//   3000 - 4000 ns: normal traffic again
//   4000 ns       : $finish
`timescale 1ns/1ps

module top;
    reg        clk;
    reg        rst_n;
    reg        stall_req;
    wire       cfg_en = 1'b1;   // stuck-at-1 control
    wire       flush  = 1'b0;   // stuck-at-0 control

    wire       s_valid;
    wire       s_ready;
    wire       s_last;
    wire [7:0] s_id;
    wire       mon_fire;
    wire [15:0] mon_beat_cnt;
    wire [15:0] mon_stall_cnt;

    xl_dut dut (
        .clk       (clk),
        .rst_n     (rst_n),
        .cfg_en    (cfg_en),
        .flush     (flush),
        .stall_req (stall_req),
        .s_valid   (s_valid),
        .s_ready   (s_ready),
        .s_last    (s_last),
        .s_id      (s_id)
    );

    xl_hs_mon hs_mon (
        .clk       (clk),
        .rst_n     (rst_n),
        .valid_in  (s_valid),
        .ready_in  (s_ready),
        .last_in   (s_last),
        .id_in     (s_id),
        .fire      (mon_fire),
        .beat_cnt  (mon_beat_cnt),
        .stall_cnt (mon_stall_cnt)
    );

    initial begin
        clk = 1'b0;
        forever #5 clk = ~clk;
    end

    initial begin
`ifdef FSDB
        $fsdbDumpfile("wave.fsdb");
        $fsdbDumpvars(0, top);
`endif
        rst_n     = 1'b0;
        stall_req = 1'b0;
        #33   rst_n     = 1'b1;
        #1967 stall_req = 1'b1;   // t = 2000 ns
        #1000 stall_req = 1'b0;   // t = 3000 ns
        #1000 $finish;            // t = 4000 ns
    end
endmodule
