// Level 4: SoC Top Level with multiple subsystems, clock gating, and reset tree
`timescale 1ns/1ps

module deep_soc_top #(
    parameter NUM_SUBSYSTEMS = 4,
    parameter DATA_WIDTH     = 8
) (
    // Global clock and reset
    input  wire                 clk_raw,
    input  wire                 rst_n_raw,
    
    // Clock gating controls
    input  wire                 global_en,
    input  wire [NUM_SUBSYSTEMS-1:0] subsystem_en,
    
    // System inputs
    input  wire [DATA_WIDTH-1:0] data_in,
    input  wire [DATA_WIDTH-1:0] ctrl_in,
    
    // System outputs
    output wire [DATA_WIDTH-1:0] data_out,
    output wire                 valid_out,
    output wire [15:0]          soc_status
);

    // Clock gating tree
    wire clk_gated;
    wire clk_raw_en;
    assign clk_raw_en = global_en;
    
    // Simple clock gate (for testing - not a proper ICG)
    wire clk_en = clk_raw_en && global_en;
    assign clk_gated = clk_raw && clk_en;
    
    // Reset tree with synchronization
    wire rst_n_sync1, rst_n_sync2, rst_n_global;
    reg rst_n_sync1_reg, rst_n_sync2_reg;
    
    always @(posedge clk_raw or negedge rst_n_raw) begin
        if (!rst_n_raw) begin
            rst_n_sync1_reg <= 1'b0;
            rst_n_sync2_reg <= 1'b0;
        end else begin
            rst_n_sync1_reg <= 1'b1;
            rst_n_sync2_reg <= rst_n_sync1_reg;
        end
    end
    assign rst_n_sync1 = rst_n_sync1_reg;
    assign rst_n_sync2 = rst_n_sync2_reg;
    assign rst_n_global = rst_n_sync2 && global_en;
    
    // Subsystem signals
    wire [NUM_SUBSYSTEMS-1:0] subsystem_valid;
    wire [NUM_SUBSYSTEMS-1:0][DATA_WIDTH-1:0] subsystem_data_out;
    wire [NUM_SUBSYSTEMS-1:0][7:0] subsystem_status;
    
    // Subsystem input routing
    wire [DATA_WIDTH-1:0] subsystem_data_in [NUM_SUBSYSTEMS-1:0];
    genvar i;
    generate
        for (i = 0; i < NUM_SUBSYSTEMS; i = i + 1) begin : gen_subsystem_routing
            // First subsystem gets external input
            // Subsequent subsystems get output from previous
            assign subsystem_data_in[i] = (i == 0) ? data_in : subsystem_data_out[i-1];
        end
    endgenerate
    
    // Subsystem instances
    generate
        for (i = 0; i < NUM_SUBSYSTEMS; i = i + 1) begin : gen_subsystems
            proc_subsystem #(
                .NUM_BLOCKS(4),
                .DATA_WIDTH(DATA_WIDTH),
                .SUBSYSTEM_ID(i)
            ) u_subsystem (
                .clk            (clk_gated),
                .rst_n          (rst_n_global),
                .subsystem_en   (subsystem_en[i]),
                .data_in        (subsystem_data_in[i]),
                .ctrl_in        (ctrl_in),
                .data_out       (subsystem_data_out[i]),
                .subsystem_valid (subsystem_valid[i]),
                .subsystem_status (subsystem_status[i])
            );
        end
    endgenerate
    
    // Output selection (from last subsystem)
    assign data_out   = subsystem_data_out[NUM_SUBSYSTEMS-1];
    assign valid_out  = subsystem_valid[NUM_SUBSYSTEMS-1];
    
    // SOC status
    assign soc_status = {
        subsystem_status[3][7:4],
        subsystem_status[2][7:4],
        global_en,
        clk_en,
        rst_n_global
    };

endmodule
