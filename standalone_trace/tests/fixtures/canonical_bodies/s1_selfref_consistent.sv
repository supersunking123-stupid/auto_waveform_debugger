// top: s1_top
// canonical-iface: supported
interface s1_if;
  logic [3:0] data;
  logic       valid;
  assign valid = |data;
endinterface
module s1_m(s1_if b, input logic [3:0] d, output logic [3:0] q);
  s1_if li();
  assign li.data = d;
  assign q = b.data;
endmodule
module s1_top(input logic [3:0] d0, d1, output logic [3:0] q0, q1);
  s1_m u0(.b(u0.li), .d(d0), .q(q0));
  s1_m u1(.b(u1.li), .d(d1), .q(q1));
endmodule
