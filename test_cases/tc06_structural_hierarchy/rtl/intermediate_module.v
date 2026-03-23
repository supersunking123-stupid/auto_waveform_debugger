`timescale 1ns/1ps
// Level 1: Intermediate module wrapping leaf
module intermediate_module (
    input  wire        in,
    output wire        out
);
    wire internal;
    leaf_cell u_leaf (
        .in  (in),
        .out (internal)
    );
    assign out = internal;
endmodule
