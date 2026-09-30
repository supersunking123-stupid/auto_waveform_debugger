// Throttled consumer: accepts 2 of every 3 cycles unless `stall` is asserted.
`timescale 1ns/1ps

module xl_sink #(
    parameter integer W = 25
) (
    input  wire         clk,
    input  wire         rst_n,
    input  wire         stall,
    input  wire         rd_valid,
    input  wire [W-1:0] rd_data,
    output wire         rd_ready,
    output reg  [15:0]  checksum
);

    reg [1:0] thr;

    assign rd_ready = ~stall & (thr != 2'd2);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            thr      <= 2'd0;
            checksum <= 16'd0;
        end else begin
            thr <= (thr == 2'd2) ? 2'd0 : thr + 2'd1;
            if (rd_valid & rd_ready)
                checksum <= checksum ^ rd_data[15:0];
        end
    end

endmodule
