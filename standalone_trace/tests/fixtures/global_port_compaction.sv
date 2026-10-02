// Stress fixture for compact global nets (>=1024 loads on clk/rst names),
// struct members, port loads and inferred assignment LHS.
module gleaf (input logic clk, input logic rst_n, input logic [7:0] d, output logic [7:0] q);
  always_ff @(posedge clk or negedge rst_n) begin : blk
    logic [7:0] tmp;
    if (!rst_n) q <= '0; else begin tmp = d; q <= tmp; end
  end
endmodule

module gleaf2 (.ck(clk_i), .dd(d_i), .qq(q_o));
  input clk_i; input d_i; output reg q_o;
  always @(posedge clk_i) q_o <= d_i;
endmodule

module gpass (input logic clk_in, output logic clk_out);
  assign clk_out = clk_in;
endmodule

module gnet_top (input logic clk, input logic rst_n, input logic core_clk, input logic [7:0] din,
                 output logic [7:0] dout, output logic [3:0] sdout);
  typedef struct packed { logic [3:0] a; logic [3:0] b; } s_t;
  logic [7:0] chain [0:1100];
  s_t sreg [0:1100];
  logic [3:0] cat_a, cat_b;
  logic gclk;
  gpass u_pass (.clk_in(core_clk), .clk_out(gclk));
  assign chain[0] = din;
  genvar i;
  for (i = 0; i < 1100; i++) begin : g
    gleaf u (.clk(clk), .rst_n(rst_n), .d(chain[i]), .q(chain[i+1]));
    gleaf2 u2 (.ck(clk), .dd(chain[i][0]), .qq());
    logic [3:0] r;
    always_ff @(posedge gclk) r <= chain[i][3:0];
    always_ff @(posedge core_clk or negedge rst_n)
      if (!rst_n) sreg[i] <= '0; else begin sreg[i].a <= chain[i][3:0]; sreg[i].b <= r; end
  end
  always_ff @(posedge core_clk) {cat_a, cat_b} <= chain[3];
  assign dout = chain[1100];
  assign sdout = sreg[5].a ^ cat_a;
endmodule
