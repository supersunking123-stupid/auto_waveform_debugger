// Top module with multiple driving/loading cones and reconvergent fanout
// Tests complex signal flow with shared logic
`timescale 1ns/1ps

module multi_cone_top #(
    parameter DATA_WIDTH = 8,
    parameter CONE_DEPTH = 6
) (
    input  wire                 clk,
    input  wire                 rst_n,
    // Multiple inputs that will have reconvergent paths
    input  wire [DATA_WIDTH-1:0] in_a,
    input  wire [DATA_WIDTH-1:0] in_b,
    input  wire [DATA_WIDTH-1:0] in_c,
    input  wire [DATA_WIDTH-1:0] in_d,
    input  wire                 sel_0,
    input  wire                 sel_1,
    input  wire                 sel_2,
    // Control signals
    input  wire                 enable,
    input  wire                 mode,
    // Outputs with multiple driving cones
    output wire [DATA_WIDTH-1:0] out_primary,
    output wire [DATA_WIDTH-1:0] out_secondary,
    output wire                 flag_converge,
    output wire                 flag_multi_driver
);

    // Stage 0: Input registers (multiple drivers for same clock)
    reg [DATA_WIDTH-1:0] reg_a, reg_b, reg_c, reg_d;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            reg_a <= {DATA_WIDTH{1'b0}};
            reg_b <= {DATA_WIDTH{1'b0}};
            reg_c <= {DATA_WIDTH{1'b0}};
            reg_d <= {DATA_WIDTH{1'b0}};
        end else begin
            reg_a <= in_a;
            reg_b <= in_b;
            reg_c <= in_c;
            reg_d <= in_d;
        end
    end

    // Stage 1: First level logic cones (divergent paths)
    wire [DATA_WIDTH-1:0] cone_a1, cone_b1, cone_c1, cone_d1;
    
    // Cone A: AND chain
    assign cone_a1[0] = reg_a[0] & reg_b[0];
    genvar i;
    generate
        for (i = 1; i < DATA_WIDTH; i = i + 1) begin : gen_cone_a
            assign cone_a1[i] = cone_a1[i-1] & reg_a[i] & reg_b[i];
        end
    endgenerate
    
    // Cone B: OR chain
    assign cone_b1[0] = reg_c[0] | reg_d[0];
    generate
        for (i = 1; i < DATA_WIDTH; i = i + 1) begin : gen_cone_b
            assign cone_b1[i] = cone_b1[i-1] | reg_c[i] | reg_d[i];
        end
    endgenerate
    
    // Cone C: XOR chain
    assign cone_c1[0] = reg_a[0] ^ reg_c[0];
    generate
        for (i = 1; i < DATA_WIDTH; i = i + 1) begin : gen_cone_c
            assign cone_c1[i] = cone_c1[i-1] ^ reg_a[i] ^ reg_c[i];
        end
    endgenerate
    
    // Cone D: Mixed logic
    assign cone_d1 = (reg_b & reg_d) | (reg_a ^ reg_c);

    // Stage 2: First reconvergence point (4 cones -> 2 cones)
    wire [DATA_WIDTH-1:0] reconverge_0, reconverge_1;
    assign reconverge_0 = sel_0 ? (cone_a1 & cone_b1) : (cone_a1 | cone_b1);
    assign reconverge_1 = sel_1 ? (cone_c1 & cone_d1) : (cone_c1 | cone_d1);

    // Stage 3: Deep logic cone (multiple levels)
    wire [DATA_WIDTH-1:0] deep_cone [CONE_DEPTH-1:0];
    assign deep_cone[0] = reconverge_0 ^ reconverge_1;
    
    generate
        for (i = 1; i < CONE_DEPTH; i = i + 1) begin : gen_deep_cone
            assign deep_cone[i] = (deep_cone[i-1] & reconverge_0) | 
                                  (deep_cone[i-1] ^ reconverge_1) |
                                  (reconverge_0 ^ reconverge_1);
        end
    endgenerate

    // Stage 4: Second reconvergence (final merge)
    wire [DATA_WIDTH-1:0] final_merge;
    assign final_merge = sel_2 ? deep_cone[CONE_DEPTH-1] : 
                         (deep_cone[CONE_DEPTH-1] ^ reconverge_0 ^ reconverge_1);

    // Stage 5: Output registers with multiple feedback paths
    reg [DATA_WIDTH-1:0] out_reg;
    wire [DATA_WIDTH-1:0] feedback;
    assign feedback = mode ? out_reg : {DATA_WIDTH{1'b0}};
    
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            out_reg <= {DATA_WIDTH{1'b0}};
        else if (enable)
            out_reg <= final_merge + feedback;
    end
    
    assign out_primary = out_reg;

    // Secondary output with different cone
    wire [DATA_WIDTH-1:0] secondary_cone;
    assign secondary_cone[0] = in_a[0] & in_b[0] & in_c[0] & in_d[0];
    generate
        for (i = 1; i < DATA_WIDTH; i = i + 1) begin : gen_secondary
            assign secondary_cone[i] = secondary_cone[i-1] & 
                                       in_a[i] & in_b[i] & 
                                       in_c[i] & in_d[i];
        end
    endgenerate
    assign out_secondary = secondary_cone;

    // Flag signals with multiple drivers (convergent detection)
    wire flag_a = &cone_a1;  // AND reduction
    wire flag_b = |cone_b1;  // OR reduction
    wire flag_c = ^cone_c1;  // XOR reduction
    wire flag_d = ~&cone_d1; // NAND reduction
    
    // Convergent flag - all paths must agree
    assign flag_converge = flag_a && flag_b && flag_c && flag_d;
    
    // Multi-driver flag - any path triggers
    assign flag_multi_driver = flag_a || flag_b || flag_c || flag_d;

endmodule
