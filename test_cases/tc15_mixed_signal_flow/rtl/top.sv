// Top module with mixed signal flow
// Tests combined combinatorial/sequential chains, feedback loops, multi-bit buses
`timescale 1ns/1ps

module mixed_signal_flow_top #(
    parameter DATA_WIDTH   = 16,
    parameter PIPE_STAGES  = 8,
    parameter FEEDBACK_SEL = 0  // 0: none, 1: output, 2: accumulator, 3: both
) (
    input  wire                 clk,
    input  wire                 rst_n,
    input  wire                 enable,
    input  wire                 hold,
    input  wire [DATA_WIDTH-1:0] data_in,
    input  wire [2:0]           op_sel,
    output wire [DATA_WIDTH-1:0] data_out,
    output wire                 valid,
    output wire                 overflow,
    output wire [PIPE_STAGES-1:0] pipe_status
);

    // Pipeline registers
    wire [DATA_WIDTH-1:0] pipe_data [PIPE_STAGES:0];
    wire [PIPE_STAGES:0]  pipe_valid;
    
    // Feedback paths
    wire [DATA_WIDTH-1:0] feedback_out;
    wire [DATA_WIDTH-1:0] feedback_combined;
    
    // Accumulator
    reg [DATA_WIDTH-1:0] accum_reg;
    wire [DATA_WIDTH-1:0] accum_next;
    wire                 accum_overflow;
    
    // Input stage with feedback selection
    assign pipe_data[0] = feedback_combined + data_in;
    assign pipe_valid[0] = enable;
    
    // Feedback selection
    generate
        case (FEEDBACK_SEL)
            0: assign feedback_combined = {DATA_WIDTH{1'b0}};
            1: assign feedback_combined = feedback_out;
            2: assign feedback_combined = accum_reg;
            3: assign feedback_combined = feedback_out + accum_reg;
            default: assign feedback_combined = {DATA_WIDTH{1'b0}};
        endcase
    endgenerate
    
    // Pipeline stages (mixed combinatorial and sequential)
    genvar i;
    generate
        for (i = 0; i < PIPE_STAGES; i = i + 1) begin : gen_pipeline
            reg [DATA_WIDTH-1:0] stage_reg;
            reg                  valid_reg;
            reg [DATA_WIDTH-1:0] processed_data;
            
            // Combinatorial processing before register
            // Different operation per stage based on op_sel
            always @(*) begin
                case (op_sel)
                    3'b000: processed_data = pipe_data[i];              // Pass through
                    3'b001: processed_data = pipe_data[i] + DATA_WIDTH'(i + 1);  // Add offset
                    3'b010: processed_data = pipe_data[i] << 1;         // Shift left
                    3'b011: processed_data = pipe_data[i] >> 1;         // Shift right
                    3'b100: processed_data = pipe_data[i] ^ DATA_WIDTH'(i);      // XOR with index
                    3'b101: processed_data = pipe_data[i] & DATA_WIDTH'(i);      // AND with index
                    3'b110: processed_data = pipe_data[i] | DATA_WIDTH'(i);      // OR with index
                    3'b111: processed_data = ~pipe_data[i];             // Invert
                    default: processed_data = pipe_data[i];
                endcase
            end
            
            // Sequential stage
            always @(posedge clk or negedge rst_n) begin
                if (!rst_n) begin
                    stage_reg <= {DATA_WIDTH{1'b0}};
                    valid_reg <= 1'b0;
                end else if (enable && !hold) begin
                    stage_reg <= processed_data;
                    valid_reg <= pipe_valid[i];
                end
            end
            
            assign pipe_data[i + 1] = stage_reg;
            assign pipe_valid[i + 1] = valid_reg;
        end
    endgenerate
    
    // Output stage
    assign data_out = pipe_data[PIPE_STAGES];
    assign valid    = pipe_valid[PIPE_STAGES];
    
    // Pipeline status
    assign pipe_status = pipe_valid[PIPE_STAGES-1:0];
    
    // Feedback output
    assign feedback_out = data_out;
    
    // Accumulator with overflow detection
    assign accum_next = accum_reg + data_out;
    assign accum_overflow = (accum_next < accum_reg);  // Unsigned overflow detection
    
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            accum_reg <= {DATA_WIDTH{1'b0}};
        else if (enable && (FEEDBACK_SEL == 2 || FEEDBACK_SEL == 3))
            accum_reg <= accum_next;
    end
    
    assign overflow = accum_overflow;
    
    // Complex enable chain with multiple conditions
    wire enable_chain [PIPE_STAGES:0];
    assign enable_chain[0] = enable;
    generate
        for (i = 0; i < PIPE_STAGES; i = i + 1) begin : gen_enable_chain
            assign enable_chain[i + 1] = enable_chain[i] && !hold && pipe_valid[i];
        end
    endgenerate

endmodule
