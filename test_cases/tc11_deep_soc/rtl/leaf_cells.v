`timescale 1ns/1ps
// Level 0: Leaf cell - D flip-flop with async reset
module dff_cell #(
    parameter WIDTH = 1
) (
    input  wire             clk,
    input  wire             rst_n,
    input  wire [WIDTH-1:0] d,
    output wire [WIDTH-1:0] q
);
    reg [WIDTH-1:0] q_reg;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            q_reg <= {WIDTH{1'b0}};
        else
            q_reg <= d;
    end
    assign q = q_reg;
endmodule

// Level 0: Leaf cell - 2-input AND gate
module and2_cell (
    input  wire a,
    input  wire b,
    output wire y
);
    assign y = a & b;
endmodule

// Level 0: Leaf cell - 2-input OR gate
module or2_cell (
    input  wire a,
    input  wire b,
    output wire y
);
    assign y = a | b;
endmodule

// Level 0: Leaf cell - 2-input MUX
module mux2_cell #(
    parameter WIDTH = 1
) (
    input  wire [WIDTH-1:0] a,
    input  wire [WIDTH-1:0] b,
    input  wire             sel,
    output wire [WIDTH-1:0] y
);
    assign y = sel ? b : a;
endmodule
