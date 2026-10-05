module vector_lsb(input logic a, input logic [3:0] b, output logic y, output logic [1:0] z);
  logic [1:1] p; logic [5:2] q;
  assign p = a; assign y = p[1];
  assign q = b; assign z = q[3:2];
endmodule

module vector_bounds(input logic [7:0] a, output logic [7:0] y, output logic z);
  logic [7:0] v;
  assign v = a;
  assign y = v;
  assign z = v[3];
endmodule

module vector_axes(input logic [3:0] a, output logic [1:0] ascending_y, negative_y);
  logic [2:5] ascending;
  logic [-2:-5] negative;
  assign ascending = a;
  assign negative = a;
  assign ascending_y = ascending[3:4];
  assign negative_y = negative[-3:-4];
endmodule

module vector_builtin(input logic [31:0] a, output logic int_y, integer_y, byte_y, enum_y);
  int int_value;
  integer integer_value;
  byte byte_value;
  typedef enum logic [7:0] { ZERO = 0 } value_t;
  value_t enum_value;
  assign int_value = int'(a);
  assign integer_value = integer'(a);
  assign byte_value = byte'(a);
  assign enum_value = value_t'(a[7:0]);
  assign int_y = int_value[3];
  assign integer_y = integer_value[3];
  assign byte_y = byte_value[3];
  assign enum_y = enum_value[3];
endmodule

module vector_port_leaf(input logic [5:2] d, output logic [1:0] z);
  assign z = d[3:2];
endmodule
module vector_port(input logic [5:2] q, output logic [1:0] z);
  vector_port_leaf u(.d(q), .z(z));
endmodule

module vector_clip_leaf(input logic [5:2] d, output logic [3:0] z);
  assign z = d[5:2];
endmodule
module vector_clip(input logic [1:0] a, output logic [3:0] z);
  logic [5:2] q;
  assign q[3:2] = a;
  vector_clip_leaf u(.d(q), .z(z));
endmodule
