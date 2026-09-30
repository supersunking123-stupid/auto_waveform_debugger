// top: iface_local_wrappers_top
// canonical-iface: supported
interface hard_if;
  logic [7:0] data;
  logic valid;
  modport source(output data, valid);
  modport sink(input data, valid);
endinterface
module hard_local_leaf(hard_if b, input logic [7:0] d, output logic [7:0] q);
  assign b.data = d;
  assign q = b.data;
endmodule
module hard_local_middle(hard_if b, input logic [7:0] d, output logic [7:0] q);
  hard_local_leaf l(b, d, q);
endmodule
module hard_local_wrap(input logic [7:0] d, output logic [7:0] q);
  hard_if local_bus(), local_bus2();
  logic [7:0] q0, q1;
  hard_local_middle child0(local_bus, d, q0);
  hard_local_middle child1(local_bus2, ~d, q1);
  assign q = q0 ^ q1;
endmodule
module iface_local_wrappers_top(input logic [7:0] a, b, output logic [7:0] x, y);
  hard_local_wrap w0(a, x), w1(b, y), w2(a ^ b, );
endmodule
