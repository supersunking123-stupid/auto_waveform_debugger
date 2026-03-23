`timescale 1ns/1ps
// Level 2: Clock subsystem with mux and multiple domains
module clock_subsystem #(
    parameter NUM_CLOCK_DOMAINS = 4,
    parameter NUM_RESET_DOMAINS = 4
) (
    // Raw clock inputs
    input  wire clk_primary,
    input  wire clk_secondary,
    input  wire clk_mux_sel,
    
    // Clock enables (hierarchical)
    input  wire global_clk_en,
    input  wire [NUM_CLOCK_DOMAINS-1:0] domain_clk_en,
    input  wire [NUM_CLOCK_DOMAINS-1:0] fine_clk_en,
    
    // Scan signals
    input  wire se,
    input  wire si,
    
    // Raw reset
    input  wire rst_n_raw,
    
    // Gated clock outputs
    output wire [NUM_CLOCK_DOMAINS-1:0] domain_clk,
    
    // Synchronized reset outputs
    output wire [NUM_RESET_DOMAINS-1:0] domain_rst_n,
    
    // Status
    output wire [NUM_CLOCK_DOMAINS-1:0] clk_status,
    output wire                         mux_clk_out
);

    // Clock mux at top level
    clock_mux u_clk_mux (
        .clk_0   (clk_primary),
        .clk_1   (clk_secondary),
        .sel     (clk_mux_sel),
        .clk_out (mux_clk_out)
    );
    
    // Reset distribution
    reset_dist_block #(.NUM_DOMAINS(NUM_RESET_DOMAINS)) u_reset_dist (
        .rst_n_raw (rst_n_raw),
        .clk       (mux_clk_out),
        .rst_n_sync (domain_rst_n)
    );
    
    // Clock distribution to each domain
    genvar i;
    generate
        for (i = 0; i < NUM_CLOCK_DOMAINS; i = i + 1) begin : gen_clock_dist
            clock_dist_block #(.BLOCK_ID(i)) u_clock_dist (
                .clk_in    (mux_clk_out),
                .global_en (global_clk_en),
                .block_en  (domain_clk_en[i]),
                .fine_en   (fine_clk_en[i]),
                .se        (se),
                .si        (si),
                .clk_out   (domain_clk[i]),
                .clk_status (clk_status[i])
            );
        end
    endgenerate

endmodule
