// top: iface_virtual_port_top
// canonical-iface: supported
interface hard_if;
  logic [7:0] data;
  logic valid;
  modport source(output data, valid);
  modport sink(input data, valid);
endinterface
module hard_virtual(hard_if b, input logic [7:0] d, output logic [7:0] q);
  virtual hard_if vif;
  initial vif = b;
  assign b.data = d;
  always_comb q = vif.data ^ b.data;
endmodule
module iface_virtual_port_top(input logic [7:0] a, b, output logic [7:0] x, y);
  hard_if i0(), i1();
  hard_virtual v0(i0, a, x), v1(i1, b, y);
endmodule
