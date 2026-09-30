// top: iface_scalar_modport_top
// canonical-iface: supported
interface hard_if;
  logic [7:0] data;
  logic valid;
  modport source(output data, valid);
  modport sink(input data, valid);
endinterface
module hard_source(hard_if.source b, input logic [7:0] d);
  assign b.data = d;
  assign b.valid = |d;
endmodule
module hard_sink(hard_if.sink b, output logic [7:0] q);
  assign q = b.valid ? b.data : 8'h00;
endmodule
module iface_scalar_modport_top(input logic [7:0] a, b, output logic [7:0] x, y);
  hard_if i0(), i1();
  hard_source s0(i0, a), s1(i1, b);
  hard_sink c0(i0, x), c1(i1, y);
endmodule
