`timescale 1ns/1ps
// Level 1: Clock distribution block with hierarchical gating
module clock_dist_block #(
    parameter BLOCK_ID = 0
) (
    input  wire clk_in,
    input  wire global_en,
    input  wire block_en,
    input  wire fine_en,
    input  wire se,
    input  wire si,
    output wire clk_out,
    output wire clk_status
);
    wire clk_level1, clk_level2;
    
    // 3-level clock gating hierarchy
    icg_cell u_icg_l1 (
        .clk       (clk_in),
        .en        (global_en),
        .se        (se),
        .si        (si),
        .gated_clk (clk_level1)
    );
    
    icg_cell u_icg_l2 (
        .clk       (clk_level1),
        .en        (block_en),
        .se        (se),
        .si        (si),
        .gated_clk (clk_level2)
    );
    
    icg_cell u_icg_l3 (
        .clk       (clk_level2),
        .en        (fine_en),
        .se        (se),
        .si        (si),
        .gated_clk (clk_out)
    );
    
    assign clk_status = global_en && block_en && fine_en;
endmodule

// Level 1: Reset distribution block
module reset_dist_block #(
    parameter NUM_DOMAINS = 4
) (
    input  wire rst_n_raw,
    input  wire clk,
    output wire [NUM_DOMAINS-1:0] rst_n_sync
);
    wire [NUM_DOMAINS-1:0] rst_n_stage1;
    
    genvar i;
    generate
        for (i = 0; i < NUM_DOMAINS; i = i + 1) begin : gen_reset_sync
            // Two-stage synchronization for each domain
            reset_sync_cell u_sync1 (
                .clk       (clk),
                .rst_n_in  (rst_n_raw),
                .rst_n_out (rst_n_stage1[i])
            );
            
            reset_sync_cell u_sync2 (
                .clk       (clk),
                .rst_n_in  (rst_n_stage1[i]),
                .rst_n_out (rst_n_sync[i])
            );
        end
    endgenerate
endmodule
