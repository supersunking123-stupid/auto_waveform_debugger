// Buffer cell - simple buffer with drive strength
// Used in generate loop test case
`timescale 1ns/1ps

module buffer_cell (
    input  wire in,
    output wire out
);
    assign out = in;
endmodule
