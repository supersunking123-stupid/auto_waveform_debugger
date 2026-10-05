module leafio(inout wire [3:0] b, input logic en, input logic [3:0] o);
  assign b = en ? o : 'z; // L io_child
endmodule
module leafua(input logic d[2]); endmodule
module leaf4(input logic [3:0] d, output logic [3:0] q); assign q = d; endmodule
module leaf1(input logic d, output logic q); assign q = d; endmodule
module misc(input logic en, input logic [3:0] o, i0, i1, i2, i3, input logic s, a, output wire [3:0] io,
            output logic [3:0] r0, r1, r2, r3, r4, output logic e0, e1, e2);
  leafio u_io(.b(io), .en(en), .o(o));
  assign io = !en ? i0 : 'z; // L io_parent
  logic arr[4];
  assign arr[0] = i0[0]; // L arr0
  assign arr[1] = i1[0]; // L arr1
  assign arr[2] = i2[0]; // L arr2
  assign arr[3] = i3[0]; // L arr3
  leafua ua(.d(arr[1:2]));
  leafua ub(.d('{arr[3], arr[0]}));
  logic [1:0] t; assign t = i0[1:0]; // L t
  logic [7:0] big; assign big = {i1, i0}; // L big
  leaf4 u_rep(.d({2{t}}), .q(r0));
  leaf4 u_cast(.d(4'(big)), .q(r1));
  leaf4 u_mux(.d(s ? i1 : i2), .q(r2));
  leaf4 u_stream(.d({<<{i3}}), .q(r3));
  leaf1 u_not(.d(!a), .q(e0));
  leaf1 u_and(.d(a & i0[0]), .q(e1));
  logic [7:0] w; assign w = {i1, i0}; // L w
  leaf4 u_cat(.d({w[5], w[2], w[1], w[0]}), .q(r4));
  logic [7:0] ps; assign ps[4:0] = {i0, a}; // L ps
  leaf1 u_ps(.d(ps[2]), .q(e2));
endmodule
