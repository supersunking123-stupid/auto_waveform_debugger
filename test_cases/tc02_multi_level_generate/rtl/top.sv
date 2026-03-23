// Top module with 2D nested generate blocks
// Tests multi-level generate hierarchy traversal
`timescale 1ns/1ps

module multi_level_generate_top #(
    parameter ROWS        = 4,
    parameter COLS        = 4,
    parameter DATA_WIDTH  = 8
) (
    input  wire                 clk,
    input  wire                 rst_n,
    input  wire [DATA_WIDTH-1:0] data_in [ROWS-1:0],
    output wire [DATA_WIDTH-1:0] data_out [ROWS-1:0],
    output wire [ROWS-1:0][COLS-1:0] pe_valid
);

    // 2D arrays for inter-PE connections
    wire [DATA_WIDTH-1:0] pe_data_out [ROWS-1:0][COLS-1:0];
    wire [DATA_WIDTH-1:0] pe_data_right [ROWS-1:0][COLS-1:0];

    // Level 1: Generate rows
    genvar row, col;
    generate
        for (row = 0; row < ROWS; row = row + 1) begin : gen_rows
            // Level 2: Generate columns within each row
            for (col = 0; col < COLS; col = col + 1) begin : gen_cols
                localparam PE_ID = row * COLS + col;

                // First column gets data from input
                wire [DATA_WIDTH-1:0] data_left;
                assign data_left = (col == 0) ? {DATA_WIDTH{1'b0}} : pe_data_right[row][col-1];

                // PE instance
                pe_cell #(
                    .DATA_WIDTH(DATA_WIDTH),
                    .PE_ID     (PE_ID)
                ) u_pe (
                    .clk        (clk),
                    .rst_n      (rst_n),
                    .data_in    (data_in[row]),
                    .data_left  (data_left),
                    .data_out   (data_out[row]),
                    .data_right (pe_data_right[row][col]),
                    .valid      (pe_valid[row][col])
                );
            end
        end
    endgenerate

endmodule
