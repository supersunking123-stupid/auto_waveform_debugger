// top: dpr_top
// args: --relax-defparam
// TODO item 4 risk case: --relax-defparam with an unresolvable defparam next to valid ones.
module dpr_leaf #(parameter int W = 4) (
    input  logic         clk,
    input  logic [W-1:0] d,
    output logic [W-1:0] q
);
  logic [W-1:0] r;
  always_ff @(posedge clk) r <= d;
  assign q = r;
endmodule

module dpr_top (
    input  logic       clk,
    input  logic [3:0] d,
    output logic [3:0] q0,
    output logic [3:0] q1,
    output logic [3:0] q2
);
  dpr_leaf #(.W(4)) u0 (.clk(clk), .d(d), .q(q0));
  dpr_leaf #(.W(4)) u1 (.clk(clk), .d(q0), .q(q1));
  dpr_leaf #(.W(4)) u2 (.clk(clk), .d(q1), .q(q2));
  defparam u1.W = 4;
  defparam u_missing.W = 3;
endmodule
