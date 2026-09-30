// top: iface_array_ports_top
// canonical-iface: supported
interface hard_if;
  logic [7:0] data;
  logic valid;
  modport source(output data, valid);
  modport sink(input data, valid);
endinterface
module hard_array(hard_if.source b[1:0], input logic [7:0] d);
  assign b[0].data = d;
  assign b[1].data = ~d;
  assign b[0].valid = |d;
  assign b[1].valid = &d;
endmodule
module hard_array_read(hard_if.sink b[0:1], output logic [7:0] q);
  assign q = b[0].data ^ b[1].data ^ {8{b[0].valid}};
endmodule
module iface_array_ports_top(input logic [7:0] a, b, output logic [7:0] x, y);
  hard_if i0[1:0](), i1[0:1]();
  hard_array s0(i0, a), s1(i1, b);
  hard_array_read c0(i0, x), c1(i1, y);
endmodule
