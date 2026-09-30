// top: s2_top
// canonical-iface: selfref
interface s2_if;
  logic [3:0] data;
  logic       valid;
  assign valid = |data;
endinterface
module s2_m(s2_if b, input logic [3:0] d, output logic [3:0] q);
  s2_if li();
  assign li.data = d;
  assign q = b.data;
endmodule
module s2_w(input logic [3:0] d, output logic [3:0] q0, q1);
  s2_if lb();
  s2_m ma(.b(ma.li), .d(d), .q(q0));
  s2_m mb(.b(lb), .d(~d), .q(q1));
endmodule
module s2_top(input logic [3:0] d0, d1, output logic [3:0] q0, q1, q2, q3);
  s2_w w0(.d(d0), .q0(q0), .q1(q1));
  s2_w w1(.d(d1), .q0(q2), .q1(q3));
endmodule
