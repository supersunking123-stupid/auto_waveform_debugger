// Top module with large generate loop
// Tests signal tracing across many generated instances
`timescale 1ns/1ps

module generate_loop_top #(
    parameter NUM_INSTANCES = 16,
    parameter DATA_WIDTH    = 8
) (
    input  wire                 clk,
    input  wire                 rst_n,
    input  wire [DATA_WIDTH-1:0] data_in,
    output wire [DATA_WIDTH-1:0] data_out,
    output wire [NUM_INSTANCES-1:0] stage_valid
);

    // Internal signals for generate loop
    wire [DATA_WIDTH-1:0] stage_data [NUM_INSTANCES:0];
    wire [NUM_INSTANCES:0] enable_chain;

    // First stage gets input directly
    assign stage_data[0] = data_in;
    assign enable_chain[0] = 1'b1;

    // Final output from last stage
    assign data_out = stage_data[NUM_INSTANCES];

    // Generate loop: create NUM_INSTANCES buffer stages
    genvar i;
    generate
        for (i = 0; i < NUM_INSTANCES; i = i + 1) begin : gen_buffer_stages
            // Buffer instance for data propagation
            buffer_cell u_buffer (
                .in  (stage_data[i]),
                .out (stage_data[i + 1])
            );

            // Valid signal for this stage
            assign stage_valid[i] = enable_chain[i];
            
            // Enable propagation
            assign enable_chain[i + 1] = enable_chain[i];
        end
    endgenerate

endmodule
