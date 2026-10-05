typedef struct packed { logic [1:0] x; logic [1:0] y; } in_t;
typedef struct packed { in_t i; logic [3:0] c; } out_t;
typedef union packed { logic [7:0] w; struct packed {logic [3:0] h; logic [3:0] l;} p; } un_t;
module leaf4(input logic [3:0] d, output logic [3:0] q); assign q = d; endmodule
module leaf8(input logic [7:0] d, output logic [7:0] q); assign q = d; endmodule
module st(input logic [3:0] i0,i1,i2,i3, output logic [7:0] y5, output logic [3:0] o4,o5,o6,o7,o8);
  out_t s;
  assign s.i.x = i0[1:0]; // L six
  assign s.i.y = i1[1:0]; // L siy
  assign s.c = i2; // L sc
  un_t un;
  assign un.p.h = i3; // L unh
  assign un.p.l = i0; // L unl
  in_t t;
  assign t = {i0[1:0], i1[1:0]}; // L t
  out_t s2;
  assign s2 = {i0, i1}; // L s2
  leaf4 u_st2(.d(s.i), .q(o5));
  leaf8 u_st3(.d(s), .q(y5));
  leaf4 u_un(.d(un.p.h), .q(o6));
  leaf4 u_un2(.d(un.w[3:0]), .q(o7));
  leaf4 u_t(.d(t), .q(o4));
  leaf4 u_s2(.d(s2.i), .q(o8));
  // Q drivers s => six siy sc
  // Q drivers s.i => six siy
  // Q drivers s.c => sc
  // Q drivers u_st2.d[3] => six
  // Q drivers u_st3.d[7] => six
  // Q drivers u_un.d => unh
  // Q drivers u_un2.d => unl
  // Q drivers un => unh unl
  // Q drivers u_t.d => t
  // Q drivers u_s2.d => s2
endmodule
