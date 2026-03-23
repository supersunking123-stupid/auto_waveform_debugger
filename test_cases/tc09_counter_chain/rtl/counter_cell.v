`timescale 1ns/1ps
// Counter cell with carry-out for chaining
module counter_cell #(
    parameter WIDTH = 4
) (
    input  wire               clk,
    input  wire               rst_n,
    input  wire               enable,
    input  wire               carry_in,
    output wire [WIDTH-1:0]   count,
    output wire               carry_out,
    output wire               terminal_count
);

    reg [WIDTH-1:0] count_reg;
    wire max_count;

    assign max_count = (&count_reg);  // All bits are 1
    assign count = count_reg;
    assign terminal_count = (count_reg == {WIDTH{1'b0}});
    assign carry_out = enable && carry_in && max_count;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            count_reg <= {WIDTH{1'b0}};
        end else if (enable && carry_in) begin
            count_reg <= count_reg + 1'b1;
        end
    end

endmodule
