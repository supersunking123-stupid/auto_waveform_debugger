// top: sr_top
// canonical-iface: selfref
// u0's interface port is connected to an interface declared *inside* u0 (downward hierarchical connection);
// u1's port is connected to a top-level interface. Same cache key -> u1 shares u0's body.
interface sr_if;
  logic [3:0] data;
  logic       valid;
  assign valid = |data;
endinterface
module sr_m(sr_if b, input logic [3:0] d, output logic [3:0] q);
  sr_if li();
  assign li.data = d;
  assign q = b.data;
endmodule
module sr_top(input logic [3:0] d0, d1, output logic [3:0] q0, q1);
  sr_if if1();
  sr_m u0(.b(u0.li), .d(d0), .q(q0));
  sr_m u1(.b(if1), .d(d1), .q(q1));
endmodule
