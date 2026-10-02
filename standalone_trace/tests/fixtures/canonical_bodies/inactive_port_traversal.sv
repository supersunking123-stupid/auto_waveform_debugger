// top: port_parent_top
module port_leaf #(parameter bit EN = 0)(input logic [7:0] d, output logic [7:0] q);
  if (EN) begin : g_selected
    assign q = d; // CASE optional_use
  end
endmodule
module port_wrapper(input logic [7:0] d, output logic [7:0] q);
  port_leaf u_leaf(.d(d), .q(q));
endmodule
module port_parent_top(input logic [7:0] a, b, c,
                       output logic [7:0] z0, z1, z2, z3, z4);
  logic [7:0] x0, x1, r0, r1, r2, r3, r4;
  assign x0 = a ^ b; // CASE drive_x0
  assign x1 = b ^ c; // CASE drive_x1
  port_leaf u_direct0(.d(x0), .q(r0));
  port_leaf u_direct1(.d(x1), .q(r1));
  port_wrapper u_nested0(.d(x0), .q(r2));
  port_wrapper u_nested1(.d(x1), .q(r3));
  port_leaf #(.EN(1)) u_active(.d(x0), .q(r4));
  port_leaf u_open(.d(), .q());
  port_leaf u_constant(.d(8'h55), .q());
  assign z0 = r0 ^ c; // CASE use_r0
  assign z1 = r1 ^ c; // CASE use_r1
  assign z2 = r2 ^ c; // CASE use_r2
  assign z3 = r3 ^ c; // CASE use_r3
  assign z4 = r4 ^ c; // CASE use_r4
endmodule
