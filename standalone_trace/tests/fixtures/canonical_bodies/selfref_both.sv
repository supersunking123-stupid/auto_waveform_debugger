// top: sr2_top
// canonical-iface: selfref
// Both instances connect to the interface inside u0.
interface sr2_if;
  logic [3:0] data;
  logic       valid;
  assign valid = |data;
endinterface
module sr2_m(sr2_if b, input logic [3:0] d, output logic [3:0] q);
  sr2_if li();
  assign li.data = d;
  assign q = b.data;
endmodule
module sr2_top(input logic [3:0] d0, d1, output logic [3:0] q0, q1);
  sr2_m u0(.b(u0.li), .d(d0), .q(q0));
  sr2_m u1(.b(u0.li), .d(d1), .q(q1));
endmodule
