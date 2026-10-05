module packed_member_leaf(input logic [3:0] d); endmodule
typedef struct packed { logic [3:0] a; logic [3:0] b; } packed_member_t;
module packed_members(input logic [3:0] i0, i1);
  packed_member_t [5:4] descending;
  packed_member_t [2:3] ascending;
  assign descending[5].a = i0;
  assign descending[5].b = i1;
  assign descending[4] = {i1, i0};
  assign ascending[2].a = i0;
  assign ascending[2].b = i1;
  assign ascending[3] = {i1, i0};
  packed_member_leaf upper_b(.d(descending[5].b));
  packed_member_leaf ascending_a(.d(ascending[2].a));
endmodule
