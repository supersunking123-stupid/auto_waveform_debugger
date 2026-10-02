// top: fixed_owner_cases
module fixed_owner_leaf(input logic [3:0] d, output wire [3:0] q);
  assign q[0] = d[0]; // CHECK leaf0
  assign q[1] = d[1]; // CHECK leaf1
  assign q[2] = d[2]; // CHECK leaf2
  assign q[3] = d[3]; // CHECK leaf3
endmodule
module fixed_owner_bool(input logic en, output wire q);
  assign q = en; // CHECK boolean_use
endmodule
module fixed_owner_cases(input logic [3:0] a[0:2], input logic [3:0] seed,
                         input logic en, input logic sel, output wire [3:0] z[0:2],
                         output wire [3:0] sink, output wire bq);
  wire [3:0] r[0:2], w[0:2];
  assign r[0][0] = seed[0]; // CHECK r0
  assign r[1][1] = seed[1]; // CHECK r1
  assign r[2][2] = seed[2]; // CHECK r2
  assign r[0][3] = seed[3]; // CHECK r3
  assign sink[0] = w[0][0]; // CHECK w0
  assign sink[1] = w[1][1]; // CHECK w1
  assign sink[2] = w[2][2]; // CHECK w2
  assign sink[3] = w[0][3]; // CHECK w3
  fixed_owner_leaf u(.d({a[0][0],a[1][1],a[2][2],a[0][3]}),
                     .q({z[0][0],z[1][1],z[2][2],z[0][3]}));
  fixed_owner_leaf reverse_u(.d({r[0][0],r[1][1],r[2][2],r[0][3]}),
                             .q({w[0][0],w[1][1],w[2][2],w[0][3]}));
  fixed_owner_bool boolean_u(.en(en && sel),.q(bq));
endmodule
