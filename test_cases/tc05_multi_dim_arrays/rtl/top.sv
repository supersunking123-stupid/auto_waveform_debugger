// Top module with multi-dimensional arrays
// Tests packed/unpacked arrays, bit-select, part-select
`timescale 1ns/1ps

module multi_dim_arrays_top #(
    parameter ROWS       = 4,
    parameter COLS       = 4,
    parameter DATA_WIDTH = 8
) (
    input  wire                 clk,
    input  wire                 rst_n,
    input  wire [DATA_WIDTH-1:0] data_in [ROWS-1:0],
    output wire [DATA_WIDTH-1:0] data_out [ROWS-1:0],
    output wire [ROWS*COLS-1:0]  bit_vector_out
);

    // 2D unpacked array
    reg [DATA_WIDTH-1:0] storage [ROWS-1:0][COLS-1:0];

    // 2D packed array (single vector)
    wire [ROWS*COLS-1:0][DATA_WIDTH-1:0] packed_array;

    // 1D views for bit-select testing
    wire [ROWS*COLS*DATA_WIDTH-1:0] flat_vector;

    genvar r, c;
    generate
        for (r = 0; r < ROWS; r = r + 1) begin : gen_rows
            for (c = 0; c < COLS; c = c + 1) begin : gen_cols
                localparam IDX = r * COLS + c;

                // Store input on clock edge
                always @(posedge clk or negedge rst_n) begin
                    if (!rst_n)
                        storage[r][c] <= {DATA_WIDTH{1'b0}};
                    else
                        storage[r][c] <= data_in[r];
                end

                // Assign to packed array
                assign packed_array[IDX] = storage[r][c];

                // Output routing
                assign data_out[r] = storage[r][COLS-1];
            end
        end
    endgenerate

    // Flatten for bit-vector output
    assign flat_vector = packed_array;
    assign bit_vector_out = flat_vector[ROWS*COLS-1:0];

    // Bit-select examples (individual bits from 2D structure)
    wire bit_0_0_0 = storage[0][0][0];
    wire bit_3_3_7 = storage[ROWS-1][COLS-1][DATA_WIDTH-1];

endmodule
