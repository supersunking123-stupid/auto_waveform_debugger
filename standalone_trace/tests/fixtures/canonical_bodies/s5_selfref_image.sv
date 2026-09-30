// top: s5_top
// canonical-iface: supported
interface s5_if;
  logic [3:0] data;
  logic       valid;
  assign valid = |data;
endinterface
module s5_m(s5_if b, input logic [3:0] d, output logic [3:0] q);
  s5_if li();
  assign li.data = d;
  assign q = b.data;
endmodule
module s5_top(input logic [3:0] d0, d1, output logic [3:0] q0, q1);
  s5_if if0();
  s5_m u0(.b(if0), .d(d0), .q(q0));
  s5_m u1(.b(u1.li), .d(d1), .q(q1));
endmodule
