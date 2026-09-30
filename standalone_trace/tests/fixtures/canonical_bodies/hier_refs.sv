// top: hr_top
// TODO item 4 risk case: hierarchical references. Downward references stay inside a shared body;
// upward references (absolute or by upward name lookup) make slang refuse caching; a hierarchical
// assignment into a cached instance makes slang un-cache it.
module hr_leaf (
    input  logic clk,
    input  logic d,
    output logic q
);
  logic r;
  always_ff @(posedge clk) r <= d;
  assign q = r;
endmodule

module hr_down (
    input  logic clk,
    input  logic d,
    output logic q,
    output logic peek
);
  logic w;
  logic ext;
  logic ext_used;
  hr_leaf u_a (.clk(clk), .d(d), .q(q));
  assign peek = u_a.r;
  assign w = u_a.r & d;
  assign ext_used = ext | w;
endmodule

module hr_up (
    input  logic clk,
    input  logic d,
    output logic q
);
  logic r;
  always_ff @(posedge clk) r <= d;
  assign q = r ^ hr_top.gsig;
endmodule

module hr_up2 (
    input  logic d,
    output logic q
);
  assign q = d & u_sib.r;
endmodule

module hr_top (
    input  logic       clk,
    input  logic [3:0] d,
    output logic [3:0] q,
    output logic [3:0] pk
);
  logic gsig;
  logic hx;
  assign gsig = d[0] ^ d[1];
  hr_leaf u_sib (.clk(clk), .d(d[2]), .q());
  hr_down dn0 (.clk(clk), .d(d[0]), .q(q[0]), .peek(pk[0]));
  hr_down dn1 (.clk(clk), .d(d[1]), .q(q[1]), .peek(pk[1]));
  hr_down dn2 (.clk(clk), .d(d[2]), .q(q[2]), .peek(pk[2]));
  hr_down dn3 (.clk(clk), .d(d[3]), .q(), .peek());
  hr_up up0 (.clk(clk), .d(d[3]), .q(q[3]));
  hr_up up1 (.clk(clk), .d(d[2]), .q());
  hr_up2 up2a (.d(d[0]), .q(pk[3]));
  hr_up2 up2b (.d(d[1]), .q());
  assign hx = dn2.u_a.r;
  assign dn1.ext = d[3];
endmodule
