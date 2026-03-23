// Top module with conditional generate blocks
// Tests if/else generate and case generate
`timescale 1ns/1ps

module conditional_generate_top #(
    parameter NUM_STAGES   = 4,
    parameter DATA_WIDTH   = 8,
    parameter USE_PIPELINE = 1,
    parameter OP_SELECT      = 0  // 0: add, 1: subtract, 2: and, 3: or
) (
    input  wire                 clk,
    input  wire                 rst_n,
    input  wire [DATA_WIDTH-1:0] data_a,
    input  wire [DATA_WIDTH-1:0] data_b,
    output wire [DATA_WIDTH-1:0] result
);

    wire [DATA_WIDTH-1:0] stage_data [NUM_STAGES:0];
    wire [DATA_WIDTH-1:0] op_result;

    // First stage input
    assign stage_data[0] = data_a;

    // Case generate: select operation type
    generate
        case (OP_SELECT)
            0: assign op_result = data_a + data_b;
            1: assign op_result = data_a - data_b;
            2: assign op_result = data_a & data_b;
            3: assign op_result = data_a | data_b;
            default: assign op_result = data_a + data_b;
        endcase
    endgenerate

    // Conditional generate: pipeline or combinational
    generate
        if (USE_PIPELINE) begin : gen_pipeline
            genvar i;
            for (i = 0; i < NUM_STAGES; i = i + 1) begin : gen_pipe_stages
                reg [DATA_WIDTH-1:0] pipe_reg;
                always @(posedge clk or negedge rst_n) begin
                    if (!rst_n)
                        pipe_reg <= {DATA_WIDTH{1'b0}};
                    else if (i == 0)
                        pipe_reg <= op_result;
                    else
                        pipe_reg <= stage_data[i];
                end
                assign stage_data[i + 1] = pipe_reg;
            end
            assign result = stage_data[NUM_STAGES];
        end else begin : gen_combinational
            // Direct combinational path
            assign result = op_result;
        end
    endgenerate

endmodule
