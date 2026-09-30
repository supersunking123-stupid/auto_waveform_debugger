// top: iface_alias_split_top
// canonical-iface: alias-split
interface hard_if;
  logic [7:0] data;
  logic valid;
  modport source(output data, valid);
  modport sink(input data, valid);
endinterface
// Alias a non-first array element: slang keys only the first leaf.
// Scalar aliases would disable slang body caching before this fallback is reached.
module hard_alias(hard_if left[1:0], hard_if right, input logic [7:0] d, output logic [7:0] q);
  assign left[0].data = d;
  assign q = left[0].data ^ right.data;
endmodule
module iface_alias_split_top(input logic [7:0] a, b, output logic [7:0] x, y);
  hard_if i0[1:0](), i1[1:0](), i2();
  hard_alias p0(i0, i0[0], a, x), p1(i1, i2, b, y);
endmodule
