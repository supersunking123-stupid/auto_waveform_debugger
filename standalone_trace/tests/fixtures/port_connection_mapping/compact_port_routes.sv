// top: compact_port_routes
typedef struct packed {logic[3:0] a,b;} route_packet_t;
module route_struct(input route_packet_t d,output route_packet_t q);assign q=d;endmodule
module route_leaf(input wire [7:0] d, output wire [7:0] q);
  assign q=d;
endmodule
module route_leaf16(input wire [15:0] d, output wire [15:0] q);
  assign q=d;
endmodule
module route_bit(input wire d, output wire q);
  assign q=d;
endmodule
module route_sync(input wire [15:0] d,output wire[15:0]q);
  for(genvar i=0;i<16;i++) begin: L
    route_bit u(.d(d[i]),.q(q[i]));
  end
endmodule
module route_sync_wrapper(input wire[15:0] d,output wire[15:0]q);
  wire[15:0]unused;
  assign unused=d;
  route_sync u(.d(d),.q(q));
endmodule
module route_producer(input wire[3:0] seed,output wire[7:0]dbg);
  assign dbg[3:0]=0; // CHECK scalar_zero
  assign dbg[7:4]=seed; // CHECK scalar_data
endmodule
module route_producer_top(input wire[3:0]seed,output wire[15:0]dbg);
  route_producer p0(.seed(seed),.dbg(dbg[7:0]));
  route_producer p1(.seed(seed),.dbg(dbg[15:8]));
endmodule
module route_ascending(input wire[0:7]d,output wire[7:0]q);
  assign q=d;
endmodule
module route_nonzero(input wire[11:4]d,output wire[7:0]q);
  assign q=d;
endmodule
module compact_port_routes(input wire[7:0]seed,output wire[7:0]y);
  wire[7:0]a[2:0],w[2:0];
  for(genvar i=0;i<3;i++) begin: rows
    assign a[i]=seed; // CHECK array_driver
    route_leaf u(.d(a[i]),.q(w[i]));
  end
  wire lo,hi;
  assign lo=|w[1][3:0]; // CHECK array_low_use
  assign hi=|w[1][7:4]; // CHECK array_high_use
  wire[7:0]x,z;
  assign x[3:0]=seed[3:0]; // CHECK concat_low
  assign x[7:4]=seed[7:4]; // CHECK concat_high
  route_leaf swap_u(.d({x[3:0],x[7:4]}),.q({z[3:0],z[7:4]}));
  route_leaf repeat_u(.d({x[3:0],x[3:0]}),.q());
  wire zlo,zhi;
  assign zlo=|z[3:0]; // CHECK swapped_low_use
  assign zhi=|z[7:4]; // CHECK swapped_high_use
  route_ascending asc_u(.d(a[1]),.q());
  route_nonzero nz_u(.d(a[1]),.q());
  wire[15:0]dbg,unused_sync;
  route_producer_top p(.seed(seed[3:0]),.dbg(dbg));
  route_sync_wrapper sync_u(.d(dbg),.q(unused_sync));
  route_packet_t src,out_packet;
  assign src.a=seed[7:4]; // CHECK struct_high
  assign src.b=seed[3:0]; // CHECK struct_low
  route_struct struct_u(.d({src.b,src.a}),.q());
  route_struct struct_out_u(.d(src),.q(out_packet));
  wire use_a,use_b;
  assign use_a=|out_packet.a; // CHECK struct_output_a
  assign use_b=|out_packet.b; // CHECK struct_output_b
  wire[7:0]ra[1:0],rw[1:0];
  assign ra[0]=seed; // CHECK row_zero_driver
  assign ra[1]=seed; // CHECK row_one_driver
  route_leaf16 row_concat_u(.d({ra[1],ra[0]}),.q({rw[1],rw[0]}));
  route_leaf16 row_swap_u(.d({ra[0],ra[1]}),.q());
  route_leaf16 row_repeat_u(.d({ra[0],ra[0]}),.q());
  route_leaf fragment_u(.d({ra[1][6:0],ra[0][7]}),.q());
  route_leaf gap_u(.d({ra[1][6:0],ra[0][0]}),.q());
  wire row_zero_use,row_one_use;
  assign row_zero_use=|rw[0]; // CHECK row_zero_use
  assign row_one_use=|rw[1]; // CHECK row_one_use
  assign y=seed;
endmodule
