// top: dp_top
// TODO item 4 risk case: defparams. slang gives every defparam target and all its ancestors a
// hierarchyOverrideNode, which makes them ineligible for instance caching; untouched siblings still share.
module dp_leaf #(parameter int W = 4) (
    input  logic         clk,
    input  logic [W-1:0] d,
    output logic [W-1:0] q
);
  logic [W-1:0] r;
  always_ff @(posedge clk) r <= d;
  assign q = r ^ {W{1'b1}};
endmodule

module dp_mid #(parameter int W = 4) (
    input  logic         clk,
    input  logic [W-1:0] a,
    output logic [W-1:0] y
);
  logic [W-1:0] t;
  dp_leaf #(.W(W)) u_l0 (.clk(clk), .d(a), .q(t));
  dp_leaf #(.W(W)) u_l1 (.clk(clk), .d(t), .q(y));
endmodule

module dp_top (
    input  logic       clk,
    input  logic [7:0] in0,
    output logic [7:0] o0,
    output logic [7:0] o1,
    output logic [7:0] o2,
    output logic [7:0] o3,
    output logic [7:0] o4
);
  dp_mid #(.W(8)) m0 (.clk(clk), .a(in0), .y(o0));
  dp_mid #(.W(8)) m1 (.clk(clk), .a(o0), .y(o1));
  dp_mid #(.W(8)) m2 (.clk(clk), .a(o1), .y(o2));
  dp_mid #(.W(8)) m3 (.clk(clk), .a(o2), .y(o3));
  dp_mid #(.W(8)) m4 (.clk(clk), .a(o3), .y(o4));
  // Same value as the instance parameter: still a defparam target.
  defparam m1.u_l1.W = 8;
  // Different value on a leaf of m3 (port width mismatch is only a warning).
  defparam m3.u_l0.W = 6;
endmodule
