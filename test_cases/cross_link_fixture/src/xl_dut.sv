// DUT: source -> FIFO -> sink. Exposes the source-side handshake (s_*) for
// the testbench monitor.
`timescale 1ns/1ps

module xl_dut (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        cfg_en,      // tied 1 by the testbench (stuck-at-1 control)
    input  wire        flush,       // tied 0 by the testbench (stuck-at-0 control)
    input  wire        stall_req,   // sink stall request (drives handshake stuck window)
    output wire        s_valid,
    output wire        s_ready,
    output wire        s_last,
    output wire [7:0]  s_id
);

    wire [15:0] src_data;
    wire        fifo_rd_valid;
    wire        fifo_rd_ready;
    wire [24:0] fifo_rd_data;
    wire        fifo_full;
    wire        fifo_empty;
    wire [15:0] sink_checksum;

    xl_src #(.BURST(4), .GAP(0)) u_src (
        .clk   (clk),
        .rst_n (rst_n),
        .ready (s_ready),
        .valid (s_valid),
        .last  (s_last),
        .id    (s_id),
        .data  (src_data)
    );

    xl_fifo #(.DEPTH(4), .W(25)) u_fifo (
        .clk      (clk),
        .rst_n    (rst_n),
        .cfg_en   (cfg_en),
        .flush    (flush),
        .wr_valid (s_valid),
        .wr_ready (s_ready),
        .wr_data  ({s_last, s_id, src_data}),
        .rd_valid (fifo_rd_valid),
        .rd_ready (fifo_rd_ready),
        .rd_data  (fifo_rd_data),
        .full     (fifo_full),
        .empty    (fifo_empty)
    );

    xl_sink #(.W(25)) u_sink (
        .clk      (clk),
        .rst_n    (rst_n),
        .stall    (stall_req),
        .rd_valid (fifo_rd_valid),
        .rd_data  (fifo_rd_data),
        .rd_ready (fifo_rd_ready),
        .checksum (sink_checksum)
    );

endmodule
