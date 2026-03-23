// Top module with counter chain
// Tests sequential logic with feedback and carry propagation
`timescale 1ns/1ps

module counter_chain_top #(
    parameter NUM_COUNTERS = 4,
    parameter COUNT_WIDTH  = 8
) (
    input  wire               clk,
    input  wire               rst_n,
    input  wire               enable,
    output wire [COUNT_WIDTH-1:0] total_count,
    output wire               overflow
);

    wire [NUM_COUNTERS-1:0]   carry_chain;
    wire [COUNT_WIDTH-1:0]    counter_out [NUM_COUNTERS-1:0];

    // First counter gets carry_in = 1 (always enabled when enable is high)
    assign carry_chain[0] = 1'b1;

    genvar i;
    generate
        for (i = 0; i < NUM_COUNTERS; i = i + 1) begin : gen_counters
            counter_cell #(
                .WIDTH(COUNT_WIDTH)
            ) u_counter (
                .clk       (clk),
                .rst_n     (rst_n),
                .enable    (enable),
                .carry_in  (carry_chain[i]),
                .count     (counter_out[i]),
                .carry_out (carry_chain[i + 1]),
                .terminal_count ()
            );
        end
    endgenerate

    // Total count is concatenation of all counter outputs
    assign total_count = counter_out[NUM_COUNTERS-1];
    assign overflow    = carry_chain[NUM_COUNTERS];

endmodule
