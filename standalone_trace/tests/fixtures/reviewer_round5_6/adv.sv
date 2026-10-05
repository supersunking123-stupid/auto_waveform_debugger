module leaf(input logic [3:0] d, output logic [3:0] q); endmodule
module leaf8(input logic [7:0] d, output logic [7:0] q); endmodule
module leafx(.d({a,b}), .q(q)); input logic [1:0] a, b; output logic [3:0] q; endmodule
module leafy(.d(w[7:4]), .q(q)); input wire [7:0] w; output logic [3:0] q; endmodule
module leafm(input logic [1:0][3:0] d, output logic q); endmodule
module adv(input logic [3:0] i0,i1,i2,i3, output logic [15:0] y, output logic [3:0] z0,z1,z2,z3);
  logic [15:0] x;
  assign x[3:0] = i0;   // L x0
  assign x[7:4] = i1;   // L x1
  assign x[11:8] = i2;  // L x2
  assign x[15:12] = i3; // L x3
  leaf ua [3:0] (.d(x), .q(y));
  assign z0 = y[3:0];   // L y0
  assign z1 = y[15:12]; // L y3
  logic [3:0][7:0] m;
  assign m[0] = {i0,i0}; // L m0
  assign m[1] = {i1,i1}; // L m1
  assign m[2] = {i2,i2}; // L m2
  assign m[3] = {i3,i3}; // L m3
  leaf8 u2 [1:0] (.d(m[2:1]), .q());
  logic [0:3][3:0] am;
  assign am[0] = i0; // L am0
  assign am[1] = i1; // L am1
  assign am[2] = i2; // L am2
  assign am[3] = i3; // L am3
  leaf8 u3 (.d(am[1:2]), .q());
  leafm u4 (.d(am[2:3]), .q());
  leafx u5 (.d(x[7:4]), .q());
  leafy u6 (.d(x[11:8]), .q());
  logic [3:0] d;
  assign d = i3; // L dimp
  leaf u7 (.d, .q());
  logic [3:0] q; leaf u8 (.*);
  // Q drivers ua[3].d => x3
  // Q drivers ua[0].d[0] => x0
  // Q loads ua[3].q => y3
  // Q loads ua[1].q =>
  // Q drivers u2[1].d => m2
  // Q drivers u2[0].d[0] => m1
  // Q drivers u3.d[0] => am2
  // Q drivers u3.d[7] => am1
  // Q drivers u4.d[1] => am2
  // Q drivers u4.d[0][0] => am3
  // Q drivers u5.a => x1
  // Q drivers u5.b[0] => x1
  // Q drivers u6.w[7] => x2
  // Q drivers u6.w[3] =>
  // Q drivers u7.d => dimp
  // Q drivers u8.d => dimp
  // Q loads x[13] => 
endmodule
