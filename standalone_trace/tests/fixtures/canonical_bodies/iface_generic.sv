// top: iface_generic_top
// canonical-iface: supported
interface hard_if;
  logic [7:0] data;
  logic valid;
  modport source(output data, valid);
  modport sink(input data, valid);
endinterface
module hard_generic(interface b, input logic [7:0] d, output logic [7:0] q);
  assign b.data = d;
  assign b.valid = ^d;
  assign q = b.valid ? b.data : ~b.data;
endmodule
module iface_generic_top(input logic [7:0] a, b, output logic [7:0] x, y);
  hard_if i0(), i1();
  hard_generic g0(i0, a, x), g1(i1, b, y);
endmodule
