// logic_ops_tb.sv — Verilog testbench exercising all operators for cross-validation
`timescale 1ns/1ps
module tb_top;

    logic clk, rst_n;
    initial clk = 0;
    always #5 clk = ~clk;

    // --- Input signals ---
    logic        a, b;
    logic [3:0]  bus_a, bus_b;
    logic signed [3:0] signed_a;

    // --- Bitwise ---
    wire and_ab    = a & b;
    wire or_ab     = a | b;
    wire xor_ab    = a ^ b;
    wire not_a     = ~a;

    // --- Reduction ---
    wire red_and_a = &bus_a;
    wire red_or_a  = |bus_a;
    wire red_xor_a = ^bus_a;
    wire red_nand_a = ~&bus_a;
    wire red_nor_a  = ~|bus_a;
    wire red_xnor_a = ~^bus_a;

    // --- Logical ---
    wire land_ab   = a && b;
    wire lor_ab    = a || b;
    wire lnot_a    = !a;

    // --- Comparison ---
    wire eq_ab     = (a == b);
    wire ne_ab     = (a != b);
    wire lt_bus    = (bus_a < bus_b);
    wire ge_bus    = (bus_a >= bus_b);

    // --- Shift ---
    wire [3:0] shl_bus = bus_a << 2;
    wire [3:0] shr_bus = bus_a >> 1;
    wire signed [3:0] asr_signed = signed_a >>> 1;

    // --- Arithmetic ---
    wire [3:0] add_bus = bus_a + bus_b;
    wire [3:0] sub_bus = bus_a - bus_b;

    // --- Ternary ---
    wire mux_out = a ? bus_a[1] : bus_b[0];

    // --- Multi-bit bitwise ---
    wire [3:0] bus_and = bus_a & bus_b;
    wire [3:0] bus_or  = bus_a | bus_b;
    wire [3:0] bus_xor = bus_a ^ bus_b;

    // --- Complex expression ---
    wire complex = (^bus_a) & a | (bus_b == 4'b0000);

    initial begin
        rst_n = 0;
        repeat(5) @(posedge clk);
        rst_n = 1;
    end

    initial begin
        $dumpfile("wave.vcd");
        $dumpvars(0, tb_top);
    end

    initial begin
        // Initialize
        a = 0; b = 0;
        bus_a = 4'b0000; bus_b = 4'b0000;
        signed_a = 4'sb0000;

        @(posedge rst_n);

        // Pattern 1: basic scalar
        @(posedge clk); a = 1; b = 0;
        @(posedge clk); a = 1; b = 1;
        @(posedge clk); a = 0; b = 1;

        // Pattern 2: bus operations
        @(posedge clk); bus_a = 4'b1010; bus_b = 4'b1100;
        @(posedge clk); bus_a = 4'b1111; bus_b = 4'b0000;
        @(posedge clk); bus_a = 4'b0011; bus_b = 4'b0101;

        // Pattern 3: signed shift
        @(posedge clk); signed_a = 4'sb1111; a = 0;  // -1
        @(posedge clk); signed_a = 4'sb1000;          // -8

        // Pattern 4: complex expression inputs
        @(posedge clk); bus_a = 4'b1011; bus_b = 4'b0000; a = 1;
        @(posedge clk); bus_a = 4'b1010; bus_b = 4'b0001; a = 0;

        // Hold for a few cycles
        repeat(10) @(posedge clk);
        $finish;
    end

endmodule
