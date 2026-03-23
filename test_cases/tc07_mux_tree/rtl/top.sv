// Top module with large combinational mux tree
// Tests combinational logic cone tracing
`timescale 1ns/1ps

module mux_tree_top #(
    parameter NUM_INPUTS = 16,
    parameter DATA_WIDTH = 8,
    parameter LOG_NUM    = 4  // log2(NUM_INPUTS)
) (
    input  wire                 clk,
    input  wire                 rst_n,
    input  wire [NUM_INPUTS-1:0] data_in,
    input  wire [LOG_NUM-1:0]    sel,
    output wire                 data_out
);

    // Tree structure for mux
    // Level 0: inputs
    // Level 1: 8 mux2s
    // Level 2: 4 mux2s
    // Level 3: 2 mux2s
    // Level 4: 1 mux2 (output)

    wire [NUM_INPUTS/2-1:0] level1;
    wire [NUM_INPUTS/4-1:0] level2;
    wire [NUM_INPUTS/8-1:0] level3;

    genvar i;

    // Level 1: 8 2-to-1 muxes
    generate
        for (i = 0; i < NUM_INPUTS/2; i = i + 1) begin : gen_level1
            assign level1[i] = sel[0] ? data_in[2*i + 1] : data_in[2*i];
        end
    endgenerate

    // Level 2: 4 2-to-1 muxes
    generate
        for (i = 0; i < NUM_INPUTS/4; i = i + 1) begin : gen_level2
            assign level2[i] = sel[1] ? level1[2*i + 1] : level1[2*i];
        end
    endgenerate

    // Level 3: 2 2-to-1 muxes
    generate
        for (i = 0; i < NUM_INPUTS/8; i = i + 1) begin : gen_level3
            assign level3[i] = sel[2] ? level2[2*i + 1] : level2[2*i];
        end
    endgenerate

    // Level 4: final mux
    assign data_out = sel[3] ? level3[1] : level3[0];

endmodule
