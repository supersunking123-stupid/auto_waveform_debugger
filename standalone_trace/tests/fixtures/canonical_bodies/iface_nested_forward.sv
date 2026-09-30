// top: iface_nested_forward_top
// canonical-iface: supported
interface hard_if;
  logic [7:0] data;
  logic valid;
  modport source(output data, valid);
  modport sink(input data, valid);
endinterface
module hard_leaf(hard_if b, input logic [7:0] d, output logic [7:0] q);
  assign b.data = d;
  assign q = b.data ^ 8'h5a;
endmodule
module hard_middle(hard_if b, input logic [7:0] d, output logic [7:0] q);
  hard_leaf child(b, d, q);
endmodule
module hard_outer(hard_if b, input logic [7:0] d, output logic [7:0] q);
  hard_middle child(b, d, q);
endmodule
module iface_nested_forward_top(input logic [7:0] a, b, output logic [7:0] x, y);
  hard_if i0(), i1();
  hard_outer o0(i0, a, x), o1(i1, b, y);
endmodule
