// Top module with complex clock/reset tree
// Tests hierarchical clock gating, reset sync, and CDC-like structures
`timescale 1ns/1ps

module clock_reset_tree_top #(
    parameter NUM_SUBSYSTEMS   = 4,
    parameter NUM_CLK_DOMAINS  = 4,
    parameter NUM_RST_DOMAINS  = 4,
    parameter DATA_WIDTH       = 8
) (
    // Primary/secondary clock inputs
    input  wire clk_primary,
    input  wire clk_secondary,
    input  wire clk_mux_sel,
    
    // Global controls
    input  wire global_clk_en,
    input  wire global_rst_n,
    input  wire scan_en,
    input  wire scan_in,
    
    // Per-subsystem controls
    input  wire [NUM_SUBSYSTEMS-1:0] subsystem_en,
    input  wire [NUM_SUBSYSTEMS*NUM_CLK_DOMAINS-1:0] domain_en,
    input  wire [NUM_SUBSYSTEMS*NUM_CLK_DOMAINS-1:0] fine_en,
    
    // Data inputs
    input  wire [DATA_WIDTH-1:0] data_in,
    
    // Data outputs
    output wire [DATA_WIDTH-1:0] data_out,
    output wire                  valid_out,
    
    // Status outputs
    output wire [NUM_SUBSYSTEMS-1:0][NUM_CLK_DOMAINS-1:0] clk_status,
    output wire [NUM_SUBSYSTEMS-1:0]                      subsystem_clk
);

    // Clock and reset distribution
    wire [NUM_SUBSYSTEMS-1:0][NUM_CLK_DOMAINS-1:0] domain_clk;
    wire [NUM_SUBSYSTEMS-1:0][NUM_RST_DOMAINS-1:0] domain_rst_n;
    wire [NUM_SUBSYSTEMS-1:0]                      mux_clk;
    wire [NUM_SUBSYSTEMS-1:0][NUM_CLK_DOMAINS-1:0] local_clk_status;
    
    // Internal data path
    wire [NUM_SUBSYSTEMS-1:0][DATA_WIDTH-1:0] subsystem_data;
    wire [NUM_SUBSYSTEMS-1:0]                 subsystem_valid;
    
    genvar s, d;
    generate
        for (s = 0; s < NUM_SUBSYSTEMS; s = s + 1) begin : gen_subsystems
            // Clock subsystem instance
            clock_subsystem #(
                .NUM_CLOCK_DOMAINS(NUM_CLK_DOMAINS),
                .NUM_RESET_DOMAINS (NUM_RST_DOMAINS)
            ) u_clock_subsystem (
                .clk_primary   (clk_primary),
                .clk_secondary (clk_secondary),
                .clk_mux_sel   (clk_mux_sel),
                .global_clk_en (global_clk_en),
                .domain_clk_en (domain_en[s*NUM_CLK_DOMAINS +: NUM_CLK_DOMAINS]),
                .fine_clk_en   (fine_en[s*NUM_CLK_DOMAINS +: NUM_CLK_DOMAINS]),
                .se            (scan_en),
                .si            (scan_in),
                .rst_n_raw     (global_rst_n),
                .domain_clk    (domain_clk[s]),
                .domain_rst_n  (domain_rst_n[s]),
                .clk_status    (local_clk_status[s]),
                .mux_clk_out   (mux_clk[s])
            );
            
            assign subsystem_clk[s] = domain_clk[s][0];
            assign clk_status[s] = local_clk_status[s];
            
            // Data processing in each subsystem
            reg [DATA_WIDTH-1:0] data_reg;
            reg                  valid_reg;
            
            always @(posedge domain_clk[s][0] or negedge domain_rst_n[s][0]) begin
                if (!domain_rst_n[s][0]) begin
                    data_reg <= {DATA_WIDTH{1'b0}};
                    valid_reg <= 1'b0;
                end else if (subsystem_en[s]) begin
                    if (s == 0)
                        data_reg <= data_in;
                    else
                        data_reg <= subsystem_data[s-1];
                    valid_reg <= 1'b1;
                end
            end
            
            assign subsystem_data[s] = data_reg;
            assign subsystem_valid[s] = valid_reg;
        end
    endgenerate
    
    // Final output
    assign data_out   = subsystem_data[NUM_SUBSYSTEMS-1];
    assign valid_out  = subsystem_valid[NUM_SUBSYSTEMS-1];

endmodule
