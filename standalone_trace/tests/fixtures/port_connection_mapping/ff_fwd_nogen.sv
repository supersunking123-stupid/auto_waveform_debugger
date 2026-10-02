module leaf (input logic [3:0] d, output logic [3:0] q);
  assign q = ~d;
endmodule
module wrap (input logic [3:0] d, output logic [3:0] q);
  leaf u_leaf(.d(d), .q(q));
endmodule
module top(input logic [3:0] a, b, c, e, output logic [3:0] o0, o1, o2, o3);
  logic [15:0] x, w;
  assign x[3:0] = a;
  assign x[7:4] = b;
  assign x[11:8] = c;
  assign x[15:12] = e;
  wrap u_w(.d(x[11:8]), .q(w[11:8]));
  assign o0 = w[3:0];
  assign o1 = w[7:4];
  assign o2 = w[11:8];
  assign o3 = w[15:12];
endmodule
