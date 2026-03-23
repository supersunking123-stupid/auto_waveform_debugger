// Top module with deep structural hierarchy
// Tests cross-module driver/load tracing, port connectivity
`timescale 1ns/1ps

module structural_hierarchy_top (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        data_in,
    output wire        data_out
);

    wire chain_0, chain_1, chain_2, chain_3;

    // Deep hierarchy chain: top -> block -> intermediate -> leaf
    block_module u_block0 (
        .in  (data_in),
        .out (chain_0)
    );

    block_module u_block1 (
        .in  (chain_0),
        .out (chain_1)
    );

    block_module u_block2 (
        .in  (chain_1),
        .out (chain_2)
    );

    block_module u_block3 (
        .in  (chain_2),
        .out (chain_3)
    );

    // Register at end
    reg data_reg;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            data_reg <= 1'b0;
        else
            data_reg <= chain_3;
    end

    assign data_out = data_reg;

endmodule
