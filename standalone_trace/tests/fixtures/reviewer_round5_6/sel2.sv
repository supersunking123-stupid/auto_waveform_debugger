typedef struct packed { logic [1:0] x; logic [1:0] y; } in_t;
typedef struct packed { in_t i; logic [3:0] c; } out_t;
typedef union packed { logic [7:0] w; struct packed {logic [3:0] h; logic [3:0] l;} p; } un_t;
module leaf4(input logic [3:0] d, output logic [3:0] q); assign q = d; endmodule
module leaf8(input logic [7:0] d, output logic [7:0] q); assign q = d; endmodule
module leafs8(input logic signed [7:0] d, output logic [7:0] q); assign q = d; endmodule
module sel2(input logic [3:0] i0,i1,i2,i3, input logic sel, output logic [7:0] y0,y1,y2,y3,y4,y5,y6,y7);
  logic [7:0] x;
  assign x[3:0] = i0; // L x0
  assign x[7:4] = i1; // L x1
  logic signed [3:0] sx;
  assign sx = i2; // L sx
  out_t s;
  assign s.i.x = i0[1:0]; // L six
  assign s.i.y = i1[1:0]; // L siy
  assign s.c = i2; // L sc
  un_t un;
  assign un.p.h = i3; // L unh
  assign un.p.l = i0; // L unl
  logic [3:0] o1,o2,o3,o4,o5,o6,o7,o8;
  leaf8 u_rep (.d({2{x[7:4]}}), .q(y0));
  leaf8 u_rep2(.d({x[3:0],{2{x[5:4]}}}), .q(y1));
  leaf4 u_trunc(.d(x), .q(o1));              // 8->4 truncation: d = x[3:0]
  leaf8 u_zext(.d(x[7:4]), .q(y2));          // 4->8 zero ext: d[3:0]=x[7:4]
  leaf8 u_sext(.d(sx), .q(y3));              // signed 4 -> 8 port (sx is signed, port unsigned)
  leafs8 u_sext2(.d(sx), .q(y4));
  leaf4 u_cast(.d(4'(x >> 4)), .q(o2));
  leaf4 u_cast2(.d(4'(x)), .q(o3));
  leaf4 u_st(.d({s.i.y, s.i.x}), .q(o4));
  leaf4 u_st2(.d(s.i), .q(o5));
  leaf8 u_st3(.d(s), .q(y5));
  leaf4 u_un(.d(un.p.h), .q(o6));
  leaf8 u_strm(.d({<<{x}}), .q(y6));
  leaf8 u_strm4(.d({<<4{x}}), .q(y7));
  leaf4 u_not(.d(~x[7:4]), .q(o7));
  leaf4 u_cond(.d(sel ? x[3:0] : x[7:4]), .q(o8));
  // Q drivers u_rep.d[0] => x1
  // Q drivers u_rep.d[7] => x1
  // Q drivers u_rep2.d[0] => x1
  // Q drivers u_rep2.d[3] => x1
  // Q drivers u_rep2.d[4] => x0
  // Q drivers u_trunc.d => x0
  // Q drivers u_trunc.d[3] => x0
  // Q drivers u_zext.d[0] => x1
  // Q drivers u_zext.d[7] =>
  // Q drivers u_sext.d[0] => sx
  // Q drivers u_sext.d[7] => sx
  // Q drivers u_sext2.d[7] => sx
  // Q drivers u_cast.d =>  x1
  // Q drivers u_cast2.d => x0
  // Q drivers u_st.d[0] => six
  // Q drivers u_st.d[3] => siy
  // Q drivers u_st2.d[3] => six
  // Q drivers u_st2.d[0] => siy
  // Q drivers u_st3.d[0] => sc
  // Q drivers u_st3.d[7] => six
  // Q drivers u_st3.d[5] => siy
  // Q drivers u_un.d => unh
  // Q drivers u_strm.d[0] => x1
  // Q drivers u_strm.d[7] => x0
  // Q drivers u_strm4.d[0] => x1
  // Q drivers u_strm4.d[7] => x0
  // Q drivers u_not.d => x1
  // Q drivers u_cond.d[0] => x0 x1
  // Q loads x[0] => 
  // Q loads s.i.x => 
endmodule
