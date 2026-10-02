typedef struct packed { logic [3:0] a; logic [3:0] b; } s_t;
module c_struct (input s_t d, output logic [3:0] q);
  assign q = d.a;
endmodule
module wrap (input s_t d, output logic [3:0] q);
  c_struct u(.d(d), .q(q));
endmodule
module top(input logic [3:0] p, r, output logic [3:0] o0, o1);
  s_t s;
  assign s.a = p;
  assign s.b = r;
  wrap u_sm(.d({s.b, s.a}), .q(o0));
  wrap u_w(.d(s), .q(o1));
endmodule
