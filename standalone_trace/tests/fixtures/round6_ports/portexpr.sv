module leafy(.d(w[7:4]), .q(r[7:4])); input wire [7:0] w; output wire [7:0] r;
  assign r[3:0] = w[3:0]; // L lo_use
  assign r[7:4] = w[7:4]; // L hi_use
endmodule
module leafc(.d({a,b}), .q(q)); input wire [1:0] a, b; output wire [3:0] q;
  assign q = {a,b}; // L cuse
endmodule
module portexpr(input logic [3:0] i0, i1, output logic [3:0] z0, z1);
  logic [7:0] x; logic [3:0] y, yc;
  assign x[3:0] = i0; // L x0
  assign x[7:4] = i1; // L x1
  leafy u (.d(x[7:4]), .q(y));
  leafc c (.d(x[3:0]), .q(yc));
  assign z0 = y;   // L yuse
  assign z1 = yc;  // L ycuse
  // Q drivers u.w[7] => x1
  // Q drivers u.w[4] => x1
  // Q drivers u.w[3] =>
  // Q drivers u.w[0] =>
  // Q loads x[4] => hi_use
  // Q loads x[0] => cuse
  // Q loads u.r[4] => yuse
  // Q loads u.r[0] =>
  // Q drivers c.a => x0
  // Q drivers c.b[0] => x0
  // Q drivers y => hi_use
endmodule

// top: portexpr
