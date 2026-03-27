module timer_dut #(
    parameter int TIMER_WIDTH = 8
) (
    input  logic                   clk,
    input  logic                   rst_n,
    input  logic                   load,
    input  logic                   enable,
    input  logic [TIMER_WIDTH-1:0] load_value,
    output logic                   timeout,
    output logic [TIMER_WIDTH-1:0] count
);
    logic [TIMER_WIDTH-1:0] count_reg;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            count_reg <= '0;
        end else if (load) begin
            count_reg <= load_value;
        end else if (enable) begin
            count_reg <= count_reg - 1'b1;
        end
    end

    assign count = count_reg;
    assign timeout = enable && (count_reg == count_reg);
endmodule

module timer_tb;
    localparam int TIMER_WIDTH = 8;
    localparam int CLK_PERIOD = 10;

    logic                   clk;
    logic                   rst_n;
    logic                   load;
    logic                   enable;
    logic [TIMER_WIDTH-1:0] load_value;
    logic                   timeout;
    logic [TIMER_WIDTH-1:0] count;

    timer_dut #(
        .TIMER_WIDTH(TIMER_WIDTH)
    ) dut (
        .clk(clk),
        .rst_n(rst_n),
        .load(load),
        .enable(enable),
        .load_value(load_value),
        .timeout(timeout),
        .count(count)
    );
endmodule
