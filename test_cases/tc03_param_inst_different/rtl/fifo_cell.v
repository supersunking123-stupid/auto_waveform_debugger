`timescale 1ns/1ps
// Parameterized FIFO cell
// Tests same module instantiated with different parameters
module fifo_cell #(
    parameter DATA_WIDTH = 8,
    parameter DEPTH      = 4,
    parameter ADDR_WIDTH = 2
) (
    input  wire                 clk,
    input  wire                 rst_n,
    input  wire                 wr_en,
    input  wire                 rd_en,
    input  wire [DATA_WIDTH-1:0] wr_data,
    output wire [DATA_WIDTH-1:0] rd_data,
    output wire                 full,
    output wire                 empty
);

    reg [DATA_WIDTH-1:0] mem [DEPTH-1:0];
    reg [ADDR_WIDTH-1:0] wr_ptr, rd_ptr;
    wire [ADDR_WIDTH-1:0] next_wr_ptr, next_rd_ptr;

    assign next_wr_ptr = (wr_ptr == ADDR_WIDTH'(DEPTH-1)) ? ADDR_WIDTH'(0) : wr_ptr + 1'b1;
    assign next_rd_ptr = (rd_ptr == ADDR_WIDTH'(DEPTH-1)) ? ADDR_WIDTH'(0) : rd_ptr + 1'b1;

    assign full  = (wr_ptr == rd_ptr) && (wr_ptr != next_wr_ptr);
    assign empty = (wr_ptr == rd_ptr);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wr_ptr <= ADDR_WIDTH'(0);
            rd_ptr <= ADDR_WIDTH'(0);
        end else if (wr_en && !full) begin
            mem[wr_ptr] <= wr_data;
            wr_ptr <= next_wr_ptr;
        end
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rd_ptr <= ADDR_WIDTH'(0);
        end else if (rd_en && !empty) begin
            rd_ptr <= next_rd_ptr;
        end
    end

    assign rd_data = mem[rd_ptr];

endmodule
