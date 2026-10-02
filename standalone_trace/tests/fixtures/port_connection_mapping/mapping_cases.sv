// top: mapping_cases
module unused_port #(parameter L=7, R=0)(input logic [L:R] d, output wire [L:R] q);
  if (0) begin : disabled
    assign q = d;
  end
endmodule
module mapped_wrapper(input logic [7:0] d, output wire [7:0] q);
  unused_port u(.d({d[3:0],d[7:4]}), .q({q[3:0],q[7:4]}));
endmodule
module mapping_cases(input logic [3:0] a,b, input logic choose,
                     output wire [3:0] ya,yb,za,zb);
  wire [15:0] x;
  wire [0:15] ascending;
  wire [15:0] indexed;
  wire [7:0] arrays [0:1];
  wire [15:0] out;
  wire [7:0] repeated_out;
  wire [7:0] nested_out;
  wire signed [3:0] signed_x;
  assign x[3:0] = a;       // CHECK low
  assign x[11:8] = b;      // CHECK high
  assign x[7:4] = a ^ b;   // CHECK neighbor
  assign ascending[0:3] = a; // CHECK ascending_high
  assign ascending[4:7] = b; // CHECK ascending_low
  assign indexed[15:12] = b; // CHECK indexed_high
  assign indexed[11:8] = a;  // CHECK indexed_low
  assign arrays[0] = {a,b};  // CHECK array_zero
  assign arrays[1] = {b,a};  // CHECK array_one
  assign signed_x = a;       // CHECK sign_low
  unused_port concat(.d({x[3:0],x[11:8]}), .q({out[3:0],out[11:8]}));
  unused_port repeat_u(.d({x[11:8],x[11:8]}), .q(repeated_out));
  unused_port #(.L(0),.R(7)) asc_owner(.d(ascending[0:7]), .q());
  unused_port #(.L(11),.R(8)) nonzero(.d(indexed[15 -: 4]), .q());
  unused_port #(.L(3),.R(0)) plus_sel(.d(indexed[8 +: 4]), .q());
  unused_port #(.L(3),.R(0)) array_u(.d(arrays[1][7:4]), .q());
  unused_port constant_u(.d({4'b0,x[11:8]}), .q());
  unused_port mixed_u(.d({choose ? a : b,x[11:8]}), .q());
  unused_port signed_u(.d(signed_x), .q());
  mapped_wrapper nested_u(.d({x[3:0],x[11:8]}), .q(nested_out));
  assign ya = out[3:0];   // CHECK out_low
  assign yb = out[11:8];  // CHECK out_high
  assign za = nested_out[3:0]; // CHECK nested_low
  assign zb = nested_out[7:4]; // CHECK nested_high
endmodule
