// top: multiple_fallbacks
module multi_empty #(parameter int W=2)(input wire [W-1:0] d); endmodule
module multi_used #(parameter int W=2)(input wire [W-1:0] d, output wire [W-1:0] q);
  assign q=d;
endmodule
module multiple_fallbacks(input wire [7:0] a,b,c,input wire [2:0] ai,bi,ci,
                          output wire [2:0] q);
  wire [7:0] x,y,z;
  assign x=a;
  assign y=b;
  assign z=c;
  multi_empty #(2) two(.d({x[ai],y[bi]}));
  multi_empty #(3) three(.d({x[ai],y[bi],z[ci]}));
  multi_empty #(3) neighbor(.d({x[ai],y[bi],z[0]}));
  multi_used #(3) used(.d({x[ai],y[bi],z[0]}),.q(q));
endmodule
