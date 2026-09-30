// Burst stream source for the cross-link fixture.
// Emits BURST beats per burst (valid held until accepted), then idles GAP+1
// cycles. `id` increments per burst, `last` marks the final beat of a burst,
// `data` is a 16-bit LFSR advanced on every accepted beat.
`timescale 1ns/1ps

module xl_src #(
    parameter integer BURST = 4,
    parameter integer GAP   = 2
) (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        ready,
    output reg         valid,
    output reg         last,
    output reg  [7:0]  id,
    output reg  [15:0] data
);

    reg  [2:0] beat;
    reg  [2:0] gap;
    wire       fire = valid & ready;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid <= 1'b0;
            last  <= 1'b0;
            id    <= 8'd0;
            data  <= 16'h1D1D;
            beat  <= 3'd0;
            gap   <= 3'd0;
        end else if (fire) begin
            data <= {data[14:0], data[15] ^ data[13] ^ data[12] ^ data[10]};
            if (beat == BURST - 1) begin
                beat  <= 3'd0;
                valid <= 1'b0;
                last  <= 1'b0;
                id    <= id + 8'd1;
                gap   <= GAP;
            end else begin
                beat <= beat + 3'd1;
                last <= (beat == BURST - 2);
            end
        end else if (!valid) begin
            if (gap == 3'd0) begin
                valid <= 1'b1;
                last  <= (BURST == 1);
            end else begin
                gap <= gap - 3'd1;
            end
        end
    end

endmodule
