// top: bd_top
// TODO item 4 risk case: bind directives. A bind to the definition adds the bound instance to every body
// (bodies still share); a bind to one instance path gives that instance a hierarchyOverrideNode (not cached);
// bind-created instances and their subtrees never participate in caching.
module bd_chk (
    input  logic a,
    input  logic b,
    output logic viol
);
  logic seen;
  assign seen = a | b;
  assign viol = a & ~b & seen;
endmodule

module bd_unit (
    input  logic clk,
    input  logic d,
    output logic q
);
  logic r;
  always_ff @(posedge clk) r <= d;
  assign q = r;
endmodule

module bd_top (
    input  logic       clk,
    input  logic [3:0] d,
    output logic [3:0] q
);
  bd_unit u0 (.clk(clk), .d(d[0]), .q(q[0]));
  bd_unit u1 (.clk(clk), .d(d[1]), .q(q[1]));
  bd_unit u2 (.clk(clk), .d(d[2]), .q(q[2]));
  bd_unit u3 (.clk(clk), .d(d[3]), .q(q[3]));
endmodule

bind bd_unit bd_chk c_all (.a(d), .b(r), .viol());
bind bd_top.u2 bd_chk c_one (.a(q), .b(r), .viol());
