// top: s4_top
// canonical-iface: selfref
interface s4_if;
  logic [3:0] data;
  logic       valid;
  assign valid = |data;
endinterface
module s4_m(s4_if b, input logic [3:0] d, output logic [3:0] q);
  s4_if la[2]();
  assign la[0].data = d;
  assign la[1].data = ~d;
  assign q = b.data;
endmodule
module s4_top(input logic [3:0] d0, d1, output logic [3:0] q0, q1);
  s4_if if1[2]();
  s4_m u0(.b(u0.la[1]), .d(d0), .q(q0));
  s4_m u1(.b(if1[0]), .d(d1), .q(q1));
endmodule
