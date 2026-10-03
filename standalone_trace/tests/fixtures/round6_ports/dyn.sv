module leaf(input logic [3:0] d, output logic [3:0] q); endmodule
module mid(input logic [7:0] d, output logic [7:0] q); leaf l(.d(d[7:4]), .q(q[7:4])); endmodule
typedef struct packed { logic [3:0] a; logic [3:0] b; } pk_t;
module dyn(input logic clk, input logic [2:0] idx, input logic [3:0] i0, i1, input logic v, output logic [7:0] y, output logic z0, z1, z2);
  logic [7:0] x;
  always_ff @(posedge clk) x[idx] <= v; // L xdyn
  leaf u (.d(x[7:4]), .q());
  logic [7:0] w;
  always_comb begin
    w = '0;            // L wzero
    w[3:0] = i0;       // L wlo
    if (v) w[7:4] = i1; // L whi
  end
  leaf u2 (.d(w[7:4]), .q());
  logic [7:0] f;
  always_comb for (int k = 0; k < 8; k++) f[k] = i0[k%4]; // L floop
  leaf u3 (.d(f[3:0]), .q());
  pk_t [1:0] pa;
  assign pa[0].a = i0; // L pa0a
  assign pa[0].b = i1; // L pa0b
  assign pa[1] = {i1, i0}; // L pa1
  leaf u4 (.d(pa[0].b), .q());
  mid  m (.d(pa[0]), .q(y));
  assign z0 = y[4]; // L y4
  assign z1 = y[0]; // L y0
  // Q drivers u.d => xdyn
  // Q drivers u.d[0] => xdyn
  // Q drivers u2.d => wzero whi
  // Q drivers u2.d[1] => wzero whi
  // Q drivers u3.d => floop
  // Q drivers u4.d => pa0b
  // Q drivers m.l.d => pa0a
  // Q loads m.l.q[0] => y4
endmodule

// top: dyn
