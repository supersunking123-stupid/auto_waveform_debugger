`timescale 1ns/1ps
// Integrated Clock Gating Cell (ICG)
// Standard latch-based clock gating for low power
module icg_cell (
    input  wire clk,
    input  wire en,
    input  wire se,  // scan enable
    input  wire si,  // scan input
    output wire gated_clk
);

    wire latch_out;
    wire and_out;

    // Active-low enable latch (transparent when clk=0)
    // Output follows enable when clk is low
    assign latch_out = ~(clk ? latch_out : ~(en | se));

    // AND gate with inverted polarity for negative-edge latch
    assign and_out = latch_out & clk;
    assign gated_clk = and_out;

endmodule
