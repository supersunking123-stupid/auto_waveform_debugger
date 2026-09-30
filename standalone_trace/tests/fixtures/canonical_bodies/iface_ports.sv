// top: if_top
// canonical-iface: supported
// TODO item 4 risk case: interface ports / modports. slang shares a body between instances whose interface
// ports connect to *different* interface instances with the same parameters, so symbols reached through an
// interface port differ per instance; the canonical tracer maps their cached interface connections.
interface bus_if #(parameter int W = 4) (input logic clk);
  logic [W-1:0] data;
  logic         valid;
  modport src(output data, output valid);
  modport dst(input data, input valid);
endinterface

module if_prod (
    bus_if.src        b,
    input logic [3:0] d
);
  assign b.data  = d;
  assign b.valid = |d;
endmodule

module if_cons (
    input  logic       clk,
    bus_if.dst         b,
    output logic [3:0] q
);
  logic [3:0] r;
  always_ff @(posedge clk) if (b.valid) r <= b.data;
  assign q = r;
endmodule

module if_pass (
    input  logic       clk,
    bus_if             b,
    output logic [3:0] q
);
  if_cons u_c (.clk(clk), .b(b), .q(q));
endmodule

// No interface ports: bodies can be shared; the interface instance is local to each copy.
module if_wrap (
    input  logic       clk,
    input  logic [3:0] d,
    output logic [3:0] q
);
  bus_if #(4) li (clk);
  if_prod p (.b(li), .d(d));
  if_cons c (.clk(clk), .b(li), .q(q));
endmodule

module if_top (
    input  logic       clk,
    input  logic [3:0] d0,
    input  logic [3:0] d1,
    output logic [3:0] q0,
    output logic [3:0] q1,
    output logic [3:0] q2,
    output logic [3:0] q3,
    output logic [3:0] q4,
    output logic [3:0] q5
);
  bus_if #(4) i0 (clk);
  bus_if #(4) i1 (clk);
  bus_if #(4) i2 (clk);
  bus_if #(4) ia[1:0] (clk);
  if_prod p0 (.b(i0), .d(d0));
  if_prod p1 (.b(i1), .d(d1));
  if_prod p2 (.b(i2), .d(d0 ^ d1));
  if_cons c0 (.clk(clk), .b(i0), .q(q0));
  if_cons c1 (.clk(clk), .b(i1), .q(q1));
  if_pass s0 (.clk(clk), .b(i2), .q(q2));
  if_pass s1 (.clk(clk), .b(i0), .q(q3));
  if_prod pa0 (.b(ia[0]), .d(d0));
  if_prod pa1 (.b(ia[1]), .d(d1));
  if_cons ca0 (.clk(clk), .b(ia[0]), .q());
  if_cons ca1 (.clk(clk), .b(ia[1]), .q());
  if_wrap w0 (.clk(clk), .d(d0), .q(q4));
  if_wrap w1 (.clk(clk), .d(d1), .q(q5));
  if_wrap w2 (.clk(clk), .d(d0 & d1), .q());
endmodule
