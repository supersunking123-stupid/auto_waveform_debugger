// Passive handshake monitor (analogous to a bandwidth monitor on a bus).
`timescale 1ns/1ps

module xl_hs_mon (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        valid_in,
    input  wire        ready_in,
    input  wire        last_in,
    input  wire [7:0]  id_in,
    output wire        fire,
    output reg  [15:0] beat_cnt,
    output reg  [15:0] stall_cnt
);

    assign fire = valid_in & ready_in;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            beat_cnt  <= 16'd0;
            stall_cnt <= 16'd0;
        end else begin
            if (fire)
                beat_cnt <= beat_cnt + 16'd1;
            if (valid_in & ~ready_in)
                stall_cnt <= stall_cnt + 16'd1;
        end
    end

endmodule
