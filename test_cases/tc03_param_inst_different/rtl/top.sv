// Top module with same module instantiated with different parameters
// Tests parameter resolution and signal tracing across param variations
`timescale 1ns/1ps

module param_inst_different_top (
    input  wire        clk,
    input  wire        rst_n,
    input  wire [7:0]  data_in,
    output wire [15:0] data_out,
    output wire        fifo0_full,
    output wire        fifo1_full,
    output wire        fifo2_full
);

    // Internal connections between FIFOs
    wire [7:0]  fifo0_rd_data;
    wire [15:0] fifo1_rd_data;
    wire [7:0]  fifo1_wr_data;
    wire [15:0] fifo2_rd_data;

    // Control signals
    wire fifo0_rd_en, fifo0_wr_en;
    wire fifo1_rd_en, fifo1_wr_en;
    wire fifo2_rd_en, fifo2_wr_en;
    wire fifo0_empty, fifo1_empty, fifo2_empty;

    // Simple control logic
    assign fifo0_wr_en = 1'b1;
    assign fifo0_rd_en = ~fifo0_full;
    assign fifo1_wr_en = ~fifo0_empty;
    assign fifo1_rd_en = ~fifo1_full;
    assign fifo2_wr_en = ~fifo1_empty;
    assign fifo2_rd_en = ~fifo2_full;

    // FIFO 0: 8-bit data, depth 4
    fifo_cell #(
        .DATA_WIDTH(8),
        .DEPTH     (4),
        .ADDR_WIDTH(2)
    ) u_fifo0 (
        .clk     (clk),
        .rst_n   (rst_n),
        .wr_en   (fifo0_wr_en),
        .rd_en   (fifo0_rd_en),
        .wr_data (data_in),
        .rd_data (fifo0_rd_data),
        .full    (fifo0_full),
        .empty   ()
    );

    // FIFO 1: 16-bit data, depth 8 (wider data, deeper)
    fifo_cell #(
        .DATA_WIDTH(16),
        .DEPTH     (8),
        .ADDR_WIDTH(3)
    ) u_fifo1 (
        .clk     (clk),
        .rst_n   (rst_n),
        .wr_en   (fifo1_wr_en),
        .rd_en   (fifo1_rd_en),
        .wr_data (fifo1_wr_data),
        .rd_data (fifo1_rd_data),
        .full    (fifo1_full),
        .empty   ()
    );

    // FIFO 2: 8-bit data, depth 16 (same width as fifo0, but deeper)
    fifo_cell #(
        .DATA_WIDTH(8),
        .DEPTH     (16),
        .ADDR_WIDTH(4)
    ) u_fifo2 (
        .clk     (clk),
        .rst_n   (rst_n),
        .wr_en   (fifo2_wr_en),
        .rd_en   (fifo2_rd_en),
        .wr_data (fifo0_rd_data),
        .rd_data (data_out[7:0]),
        .full    (fifo2_full),
        .empty   ()
    );

    // Sign extension for output
    assign data_out[15:8] = fifo1_rd_data[15:8];

    // Width conversion
    assign fifo1_wr_data = {fifo0_rd_data, fifo0_rd_data};

endmodule
