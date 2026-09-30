// Small synchronous FIFO. wr_ready deasserts when full (or when the
// configuration enable is low / flush is asserted), which makes it the
// "ready" handshake signal that can get stuck when the sink stalls.
`timescale 1ns/1ps

module xl_fifo #(
    parameter integer DEPTH = 4,
    parameter integer W     = 25
) (
    input  wire         clk,
    input  wire         rst_n,
    input  wire         cfg_en,
    input  wire         flush,
    input  wire         wr_valid,
    output wire         wr_ready,
    input  wire [W-1:0] wr_data,
    output wire         rd_valid,
    input  wire         rd_ready,
    output wire [W-1:0] rd_data,
    output wire         full,
    output wire         empty
);

    reg  [W-1:0] mem [0:DEPTH-1];
    reg  [1:0]   wr_ptr;
    reg  [1:0]   rd_ptr;
    reg  [2:0]   count;

    assign full     = (count == DEPTH);
    assign empty    = (count == 3'd0);
    assign wr_ready = ~full & cfg_en & ~flush;
    assign rd_valid = ~empty;
    assign rd_data  = mem[rd_ptr];

    wire wr_fire = wr_valid & wr_ready;
    wire rd_fire = rd_valid & rd_ready;

    always @(posedge clk) begin
        if (wr_fire)
            mem[wr_ptr] <= wr_data;
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wr_ptr <= 2'd0;
            rd_ptr <= 2'd0;
            count  <= 3'd0;
        end else begin
            if (wr_fire) wr_ptr <= wr_ptr + 2'd1;
            if (rd_fire) rd_ptr <= rd_ptr + 2'd1;
            count <= count + {2'b00, wr_fire} - {2'b00, rd_fire};
        end
    end

endmodule
