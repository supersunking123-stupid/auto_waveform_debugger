// top: iface_conflict_top
// canonical-iface: conflict
// The outer-root substitution disagrees with its nested interface substitution.
interface conflict_leaf_if;
  logic data;
endinterface
interface conflict_outer_if;
  conflict_leaf_if nested();
  logic data;
endinterface
module conflict_use(conflict_outer_if a, conflict_leaf_if b,
                    input logic x, output logic y);
  assign y = x;
  assign a.data = x;
  assign b.data = x;
endmodule
module iface_conflict_top(input logic x, output logic y0, y1);
  conflict_outer_if a0(), a1();
  conflict_leaf_if b1();
  conflict_use u0(a0, a0.nested, x, y0), u1(a1, b1, x, y1);
endmodule
