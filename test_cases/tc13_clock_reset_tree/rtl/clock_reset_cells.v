`timescale 1ns/1ps
// Level 0: Integrated Clock Gating Cell (ICG)
module icg_cell (
    input  wire clk,
    input  wire en,
    input  wire se,  // scan enable
    input  wire si,  // scan input
    output wire gated_clk
);
    wire latch_out, and_out;
    // Active-low enable latch
    assign latch_out = ~(clk ? latch_out : ~(en | se));
    assign and_out = latch_out & clk;
    assign gated_clk = and_out;
endmodule

// Level 0: Reset synchronizer cell
module reset_sync_cell (
    input  wire clk,
    input  wire rst_n_in,
    output wire rst_n_out
);
    reg [1:0] sync_reg;
    always @(posedge clk or negedge rst_n_in) begin
        if (!rst_n_in)
            sync_reg <= 2'b00;
        else
            sync_reg <= {sync_reg[0], 1'b1};
    end
    assign rst_n_out = sync_reg[1];
endmodule

// Level 0: Clock mux
module clock_mux (
    input  wire clk_0,
    input  wire clk_1,
    input  wire sel,
    output wire clk_out
);
    // Glitch-free mux (both clocks assumed synchronous)
    assign clk_out = sel ? clk_1 : clk_0;
endmodule
