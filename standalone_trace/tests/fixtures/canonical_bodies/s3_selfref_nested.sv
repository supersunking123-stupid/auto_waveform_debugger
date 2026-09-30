// top: s3_top
// canonical-iface: selfref
interface s3_in;
  logic [3:0] data;
  logic       valid;
  assign valid = |data;
endinterface
interface s3_out;
  s3_in n();
  logic x;
  assign x = n.valid;
endinterface
module s3_m(s3_in b, input logic [3:0] d, output logic [3:0] q);
  s3_out lo();
  assign lo.n.data = d;
  assign q = b.data;
endmodule
module s3_top(input logic [3:0] d0, d1, output logic [3:0] q0, q1);
  s3_in if1();
  s3_m u0(.b(u0.lo.n), .d(d0), .q(q0));
  s3_m u1(.b(if1), .d(d1), .q(q1));
endmodule
