module leaf(input logic [3:0] d, output logic [3:0] q); endmodule
module mid #(parameter int P = 0) (input logic [7:0] d, output logic [7:0] q);
  leaf l (.d(d[P +: 4]), .q(q[P +: 4]));
endmodule
module mid2(input logic [15:0] d, output logic [15:0] q);
  mid #(.P(4)) m (.d(d[11:4]), .q(q[11:4]));
endmodule
module outl(input logic [3:0] a, output logic [3:0] q); assign q = a; endmodule
module hier(input logic [3:0] i0,i1,i2,i3, output logic [3:0] z0,z1,z2,z3,z4,z5,z6,z7);
  logic [15:0] x, w;
  assign x[3:0] = i0;   // L x0
  assign x[7:4] = i1;   // L x1
  assign x[11:8] = i2;  // L x2
  assign x[15:12] = i3; // L x3
  mid #(.P(0)) ma (.d(x[7:0]),  .q(w[7:0]));
  mid #(.P(4)) mb (.d(x[15:8]), .q(w[15:8]));
  assign z0 = w[3:0];   // L w0
  assign z1 = w[7:4];   // L w1
  assign z2 = w[11:8];  // L w2
  assign z3 = w[15:12]; // L w3
  logic [15:0] v, u;
  assign v[3:0] = i0;   // L v0
  assign v[7:4] = i1;   // L v1
  assign v[11:8] = i2;  // L v2
  assign v[15:12] = i3; // L v3
  mid2 mm (.d(v), .q(u));
  assign z4 = u[3:0];   // L u0
  assign z5 = u[7:4];   // L u1
  assign z6 = u[11:8];  // L u2
  assign z7 = u[15:12]; // L u3
  logic [7:0] ow;
  outl o0 (.a(i0), .q(ow[3:0]));
  outl o1 (.a(i1), .q(ow[7:4]));
  assign z0[0] = ow[4]; // L ow4
  // Q drivers ma.l.d => x0
  // Q drivers mb.l.d => x3
  // Q drivers ma.l.d[3] => x0
  // Q drivers mb.l.d[0] => x3
  // Q loads ma.l.q => w0
  // Q loads mb.l.q => w3
  // Q loads mb.l.q[1] => w3
  // Q drivers mm.m.l.d => v2
  // Q drivers mm.m.l.d[0] => v2
  // Q loads mm.m.l.q => u2
  // Q drivers ma.d => x0 x1
  // Q drivers mb.d[0] => x2
  // Q loads o1.q[0] => ow4
  // Q loads o0.q => 
endmodule
