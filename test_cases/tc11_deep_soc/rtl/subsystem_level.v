`timescale 1ns/1ps
// Level 3: Subsystem with multiple processing blocks
module proc_subsystem #(
    parameter NUM_BLOCKS   = 4,
    parameter DATA_WIDTH   = 8,
    parameter SUBSYSTEM_ID = 0
) (
    input  wire                 clk,
    input  wire                 rst_n,
    input  wire                 subsystem_en,
    input  wire [DATA_WIDTH-1:0] data_in,
    input  wire [DATA_WIDTH-1:0] ctrl_in,
    output wire [DATA_WIDTH-1:0] data_out,
    output wire                 subsystem_valid,
    output wire [7:0]           subsystem_status
);

    wire [NUM_BLOCKS-1:0] block_en;
    wire [NUM_BLOCKS-1:0] block_valid;
    wire [NUM_BLOCKS-1:0][DATA_WIDTH-1:0] block_data_out;
    wire [NUM_BLOCKS-1:0][3:0] block_status;
    wire [2:0] ctrl_state;
    
    // Control block for sequencing
    ctrl_block #(.NUM_BLOCKS(NUM_BLOCKS)) u_ctrl (
        .clk         (clk),
        .rst_n       (rst_n),
        .block_ready (block_valid),
        .block_en    (block_en),
        .ctrl_state  (ctrl_state)
    );
    
    // Processing blocks chain
    genvar i;
    generate
        for (i = 0; i < NUM_BLOCKS; i = i + 1) begin : gen_blocks
            wire [DATA_WIDTH-1:0] block_data_in;
            
            // First block gets input from subsystem input
            // Subsequent blocks get input from previous block
            assign block_data_in = (i == 0) ? data_in : block_data_out[i-1];
            
            proc_block #(
                .DATA_WIDTH(DATA_WIDTH),
                .BLOCK_ID  (i)
            ) u_block (
                .clk       (clk),
                .rst_n     (rst_n),
                .block_en  (block_en[i] && subsystem_en),
                .data_in   (block_data_in),
                .ctrl_in   (ctrl_in),
                .data_out  (block_data_out[i]),
                .block_valid (block_valid[i]),
                .status    (block_status[i])
            );
        end
    endgenerate
    
    // Output from last block
    assign data_out        = block_data_out[NUM_BLOCKS-1];
    assign subsystem_valid = block_valid[NUM_BLOCKS-1];
    assign subsystem_status = {subsystem_en, ctrl_state, block_valid};
endmodule
