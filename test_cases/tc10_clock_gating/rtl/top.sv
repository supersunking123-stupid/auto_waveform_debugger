// Top module with clock gating
// Tests clock gating cells and high-fanout clock network
`timescale 1ns/1ps

module clock_gating_top #(
    parameter NUM_DOMAINS = 4,
    parameter DATA_WIDTH  = 8
) (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        global_en,
    input  wire [NUM_DOMAINS-1:0] domain_en,
    input  wire        scan_en,
    input  wire        scan_in,
    output wire [NUM_DOMAINS-1:0] gated_clk_out,
    output wire [DATA_WIDTH-1:0] domain_data [NUM_DOMAINS-1:0]
);

    wire [NUM_DOMAINS-1:0] gated_clk;
    reg [DATA_WIDTH-1:0] domain_data_reg [NUM_DOMAINS-1:0];

    genvar i;
    generate
        for (i = 0; i < NUM_DOMAINS; i = i + 1) begin : gen_clock_gating
            // Clock gating cell for each domain
            icg_cell u_icg (
                .clk       (clk),
                .en        (domain_en[i] && global_en),
                .se        (scan_en),
                .si        (scan_in),
                .gated_clk (gated_clk[i])
            );

            assign gated_clk_out[i] = gated_clk[i];

            // Domain register bank with gated clock
            always @(posedge gated_clk[i] or negedge rst_n) begin
                if (!rst_n)
                    domain_data_reg[i] <= {DATA_WIDTH{1'b0}};
                else
                    domain_data_reg[i] <= domain_data_reg[i] + 1'b1;
            end
        end
    endgenerate

    // Assign output
    assign domain_data = domain_data_reg;

endmodule
