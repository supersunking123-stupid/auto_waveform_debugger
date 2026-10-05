interface bus_if; logic [7:0] data; logic vld; modport mp(input data, input vld); endinterface
module leaf(input logic [3:0] d, output logic [3:0] q); endmodule
module leafo8(output logic [7:0] q); endmodule
module leafio(inout wire [3:0] b); endmodule
module leafarr(input logic [3:0] d [0:1], output logic [3:0] q); endmodule
module leafif(bus_if.mp bi, output logic [3:0] q); endmodule
module leafifu(bus_if.mp bi, output logic [7:0] q); assign q = bi.data; endmodule
module misc(input logic [3:0] i0,i1,i2,i3, output logic [3:0] z0,z1,z2,z3, output logic [1:0] z4);
  logic [15:0] x;
  assign x[3:0] = i0;   // L x0
  assign x[7:4] = i1;   // L x1
  assign x[11:8] = i2;  // L x2
  assign x[15:12] = i3; // L x3
  for (genvar g = 0; g < 4; g++) begin : G
    leaf u (.d(x[g*4 +: 4]), .q());
    leaf r (.d(x[15 - g*4 -: 4]), .q());
  end
  logic [7:0] n;
  leafo8 on (.q(n[3:0]));       // narrow: q[3:0]->n[3:0], q[7:4] dropped
  assign z0 = n[3:0];  // L n0
  assign z1 = n[7:4];  // L n1
  wire [7:0] tri_w;
  assign tri_w[3:0] = i0; // L t0
  assign tri_w[7:4] = i1; // L t1
  leafio io (.b(tri_w[7:4]));
  assign z2 = tri_w[7:4]; // L t1use
  logic [3:0] arr [0:3];
  assign arr[0] = i0; // L a0
  assign arr[1] = i1; // L a1
  assign arr[2] = i2; // L a2
  assign arr[3] = i3; // L a3
  leafarr ua (.d(arr[1:2]), .q());
  leafarr ub (.d('{arr[3], arr[0]}), .q());
  bus_if bif();
  assign bif.data[3:0] = i0; // L bd0
  assign bif.data[7:4] = i1; // L bd1
  assign bif.vld = i2[0];    // L bv
  leafif ui (.bi(bif), .q());
  leafifu uiu (.bi(bif), .q());
  logic [7:0] wide;
  leaf uw (.d(wide), .q(z4));  // 2-bit actual on 4-bit output: q[1:0] -> z4
  assign wide = {i1, i0}; // L wide
  assign z3 = z4; // L z4use
  // Q drivers G[1].u.d => x1
  // Q drivers G[2].u.d[3] => x2
  // Q drivers G[0].r.d => x3
  // Q drivers G[3].r.d[0] => x0
  // Q loads x[5] => 
  // Q loads on.q[1] => n0
  // Q loads on.q[6] => 
  // Q drivers io.b => t1
  // Q loads io.b[0] => t1use
  // Q loads tri_w[4] =>
  // Q drivers ua.d => a1 a2
  // Q drivers ub.d => a0 a3


  // Q loads uw.q[0] => z4use
  // Q loads uw.q[3] =>
  // Q drivers uw.d => wide
endmodule
