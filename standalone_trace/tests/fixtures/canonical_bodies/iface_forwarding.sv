// top: iface_forwarding_top
// canonical-iface: forwarding
// An interface itself forwards a connection beyond the supported leaf-root map.
interface forwarding_leaf_if;
  logic data;
endinterface
interface forwarding_outer_if(forwarding_leaf_if f);
  logic data;
endinterface
module forwarding_use(forwarding_outer_if a, input logic x, output logic y);
  assign y = x;
  assign a.data = x;
endmodule
module iface_forwarding_top(input logic x, output logic y0, y1);
  forwarding_leaf_if b0(), b1();
  forwarding_outer_if a0(b0), a1(b1);
  forwarding_use u0(a0, x, y0), u1(a1, x, y1);
endmodule
