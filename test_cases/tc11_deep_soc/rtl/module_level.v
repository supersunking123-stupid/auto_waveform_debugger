`timescale 1ns/1ps
// Level 1: Register slice module
module reg_slice #(
    parameter WIDTH = 8
) (
    input  wire             clk,
    input  wire             rst_n,
    input  wire             en,
    input  wire [WIDTH-1:0] d,
    output wire [WIDTH-1:0] q
);
    wire [WIDTH-1:0] gated_d;
    
    // Gated input using MUX
    mux2_cell #(.WIDTH(WIDTH)) u_mux (
        .a   ({WIDTH{1'b0}}),
        .b   (d),
        .sel (en),
        .y   (gated_d)
    );
    
    dff_cell #(.WIDTH(WIDTH)) u_dff (
        .clk (clk),
        .rst_n (rst_n),
        .d   (gated_d),
        .q   (q)
    );
endmodule

// Level 1: Combinational logic block
module comb_logic #(
    parameter WIDTH = 8
) (
    input  wire [WIDTH-1:0] a,
    input  wire [WIDTH-1:0] b,
    input  wire             sel,
    output wire [WIDTH-1:0] y,
    output wire             zero_flag
);
    wire [WIDTH-1:0] and_result, or_result;
    genvar i;
    generate
        for (i = 0; i < WIDTH; i = i + 1) begin : gen_bitwise
            and2_cell u_and (.a(a[i]), .b(b[i]), .y(and_result[i]));
            or2_cell  u_or  (.a(a[i]), .b(b[i]), .y(or_result[i]));
        end
    endgenerate
    
    mux2_cell #(.WIDTH(WIDTH)) u_mux (
        .a   (and_result),
        .b   (or_result),
        .sel (sel),
        .y   (y)
    );
    
    // Zero flag - NOR reduction
    assign zero_flag = ~(y[0] | y[1] | y[2] | y[3] | y[4] | y[5] | y[6] | y[7]);
endmodule
