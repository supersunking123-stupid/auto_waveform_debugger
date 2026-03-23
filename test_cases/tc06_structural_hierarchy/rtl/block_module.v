`timescale 1ns/1ps
// Level 2: Block module wrapping intermediate
module block_module (
    input  wire        in,
    output wire        out
);
    wire internal;
    intermediate_module u_inter (
        .in  (in),
        .out (internal)
    );
    assign out = internal;
endmodule
