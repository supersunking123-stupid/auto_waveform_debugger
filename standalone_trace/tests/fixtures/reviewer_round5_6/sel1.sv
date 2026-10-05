module leaf4(input logic [3:0] d, output logic [3:0] q); assign q = d; endmodule
module leaf8(input logic [7:0] d, output logic [7:0] q); assign q = d; endmodule
module leafa(input logic [4:11] d, output logic [7:0] q); assign q = d; endmodule
module sel1(input logic [3:0] i0,i1,i2,i3, output logic [7:0] y0,y1,y2,y3,y4,y5);
  logic [3:0][7:0] m;
  assign m[0] = {i0,i0}; // L m0
  assign m[1] = {i1,i1}; // L m1
  assign m[2] = {i2,i2}; // L m2
  assign m[3] = {i3,i3}; // L m3
  logic [23:8] nz;
  assign nz[11:8]  = i0; // L nz0
  assign nz[15:12] = i1; // L nz1
  assign nz[19:16] = i2; // L nz2
  assign nz[23:20] = i3; // L nz3
  logic [0:15] asc;
  assign asc[0:3]  = i0; // L a0
  assign asc[4:7]  = i1; // L a1
  assign asc[8:11] = i2; // L a2
  assign asc[12:15]= i3; // L a3
  logic [3:0] o1, o2, o3, o4, o5;
  leaf8 u_md (.d(m[2]), .q(y0));
  leaf4 u_md2(.d(m[1][5:2]), .q(o1));
  leaf8 u_md3(.d(m[3][3:0] ^ 4'h0), .q());
  leaf4 u_nz (.d(nz[15:12]), .q(o2));
  leaf4 u_nzp(.d(nz[16 +: 4]), .q(o3));
  leaf4 u_nzm(.d(nz[23 -: 4]), .q(o4));
  leaf4 u_as (.d(asc[4 +: 4]), .q(o5));
  leaf8 u_as2(.d(asc[8:15]), .q(y1));
  leafa u_aa (.d({nz[19:16], asc[0:3]}), .q(y2));
  // Q drivers u_md.d => m2
  // Q drivers u_md.d[0] => m2
  // Q drivers u_md2.d[0] => m1
  // Q drivers u_md2.d[3] => m1
  // Q drivers u_nz.d[0] => nz1
  // Q drivers u_nzp.d => nz2
  // Q drivers u_nzm.d[3] => nz3
  // Q drivers u_as.d => a1
  // Q drivers u_as2.d[0] => a3
  // Q drivers u_as2.d[7] => a2
  // Q drivers u_aa.d[4] => nz2
  // Q drivers u_aa.d[11] => a0
  // Q drivers u_aa.d[8:11] => a0
  // Q loads m[2] => 
  // Q loads m[2][0] => 
  // Q loads nz[12] => 
  // Q loads asc[5] => 
  // Q loads asc[15] => 
endmodule
