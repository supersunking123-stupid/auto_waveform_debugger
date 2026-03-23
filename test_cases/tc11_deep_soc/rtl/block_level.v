`timescale 1ns/1ps
// Level 2: Processing block with multiple stages
module proc_block #(
    parameter DATA_WIDTH = 8,
    parameter BLOCK_ID   = 0
) (
    input  wire                 clk,
    input  wire                 rst_n,
    input  wire                 block_en,
    input  wire [DATA_WIDTH-1:0] data_in,
    input  wire [DATA_WIDTH-1:0] ctrl_in,
    output wire [DATA_WIDTH-1:0] data_out,
    output wire                 block_valid,
    output wire [3:0]           status
);

    wire [DATA_WIDTH-1:0] stage1_out, stage2_out, stage3_out;
    wire                  stage1_en, stage2_en;
    wire                  zero_flag;
    
    // Stage 1: Input register
    assign stage1_en = block_en;
    reg_slice #(.WIDTH(DATA_WIDTH)) u_stage1_reg (
        .clk (clk),
        .rst_n (rst_n),
        .en  (stage1_en),
        .d   (data_in),
        .q   (stage1_out)
    );
    
    // Stage 2: Combinational logic
    assign stage2_en = block_en;
    comb_logic #(.WIDTH(DATA_WIDTH)) u_comb (
        .a   (stage1_out),
        .b   (ctrl_in),
        .sel (block_en),
        .y   (stage2_out),
        .zero_flag (zero_flag)
    );
    
    // Stage 3: Output register
    reg_slice #(.WIDTH(DATA_WIDTH)) u_stage3_reg (
        .clk (clk),
        .rst_n (rst_n),
        .en  (stage2_en),
        .d   (stage2_out),
        .q   (stage3_out)
    );
    
    assign data_out   = stage3_out;
    assign block_valid = stage2_en;
    assign status     = {block_en, zero_flag, stage1_en, stage2_en};
endmodule

// Level 2: Control block
module ctrl_block #(
    parameter NUM_BLOCKS = 4
) (
    input  wire               clk,
    input  wire               rst_n,
    input  wire [NUM_BLOCKS-1:0] block_ready,
    output wire [NUM_BLOCKS-1:0] block_en,
    output wire [2:0]          ctrl_state
);
    reg [2:0] state_reg;
    
    // Simple state machine
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            state_reg <= 3'b000;
        else begin
            case (state_reg)
                3'b000: state_reg <= 3'b001;
                3'b001: state_reg <= 3'b010;
                3'b010: state_reg <= 3'b011;
                3'b011: state_reg <= 3'b100;
                3'b100: state_reg <= 3'b000;
                default: state_reg <= 3'b000;
            endcase
        end
    end
    
    assign ctrl_state = state_reg;
    
    // Generate block enables based on state
    genvar i;
    generate
        for (i = 0; i < NUM_BLOCKS; i = i + 1) begin : gen_block_en
            assign block_en[i] = (state_reg == i[2:0]) && block_ready[i];
        end
    endgenerate
endmodule
