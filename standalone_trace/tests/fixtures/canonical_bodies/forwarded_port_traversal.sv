// top: forwarded_port_top
module forward_leaf #(parameter bit FLOP = 0)
    (input logic clk, input logic [7:0] d, output logic [7:0] q);
  if (FLOP) begin : g_flop
    always_ff @(posedge clk) q <= d;
  end else begin : g_combo
    assign q = d;
  end
endmodule
module forward_wrapper #(parameter bit FLOP = 0)
    (input logic clk, input logic [7:0] d, output logic [7:0] q);
  if (0) begin : g_inactive
    assign q = d;
  end
  // Raw port uses survive, but the endpoints are rendered on the active leaf.
  forward_leaf #(.FLOP(FLOP)) u_leaf(.clk(clk), .d(d), .q(q));
endmodule
module forward_outer(input logic clk, input logic [7:0] d, output logic [7:0] q);
  if (0) begin : g_inactive
    assign q = d;
  end
  forward_wrapper u_wrapper(.clk(clk), .d(d), .q(q));
endmodule
module forwarded_port_top(input logic [7:0] a, b, c, input logic clk_a, clk_b, en,
                           output logic [7:0] z0, z1, z2, z3);
  logic [7:0] x0, x1, r0, r1, r2, r3;
  logic clk0, clk1;
  assign x0 = a ^ b; // CASE x0
  assign x1 = b ^ c; // CASE x1
  assign clk0 = clk_a & en; // CASE clk0
  assign clk1 = clk_b | en; // CASE clk1
  forward_wrapper u0(.clk(clk0), .d(x0), .q(r0));
  forward_wrapper u1(.clk(clk1), .d(x1), .q(r1));
  forward_outer u_nested(.clk(clk0), .d(x0), .q(r2));
  forward_wrapper #(.FLOP(1)) u_flopped(.clk(clk1), .d(x1), .q(r3));
  assign z0 = r0 ^ c; // CASE r0
  assign z1 = r1 ^ a; // CASE r1
  assign z2 = r2 ^ c; // CASE r2
  assign z3 = r3 ^ a; // CASE r3
endmodule
