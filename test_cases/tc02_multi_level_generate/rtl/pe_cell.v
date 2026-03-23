`timescale 1ns/1ps
// Processing Element (PE) cell for 2D array
// Used in multi-level generate test case
module pe_cell #(
    parameter DATA_WIDTH = 8,
    parameter PE_ID      = 0
) (
    input  wire                 clk,
    input  wire                 rst_n,
    input  wire [DATA_WIDTH-1:0] data_in,
    input  wire [DATA_WIDTH-1:0] data_left,
    output wire [DATA_WIDTH-1:0] data_out,
    output wire [DATA_WIDTH-1:0] data_right,
    output wire                 valid
);

    reg [DATA_WIDTH-1:0] data_reg;
    wire [DATA_WIDTH-1:0] sum;

    // Simple add operation
    assign sum = data_in + data_left;

    // Output routing
    assign data_right = data_reg;
    assign data_out   = sum;
    assign valid      = (data_reg != 0);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            data_reg <= {DATA_WIDTH{1'b0}};
        end else begin
            data_reg <= sum;
        end
    end

endmodule
