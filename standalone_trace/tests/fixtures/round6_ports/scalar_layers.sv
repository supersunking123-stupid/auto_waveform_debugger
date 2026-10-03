module layer(input wire a, output wire q); assign q=a; endmodule
module scalar_layers(input wire [7:0] v, output wire o[7:0], output wire oa[0:7], output wire [7:0] packed_o);
  wire s[7:0]; wire sa[0:7]; wire [0:0] sv[7:0];
  for(genvar i=0;i<8;i++) begin: FOR_LAYER
    layer u(.a(v[i]),.q(s[i]));
    layer ua(.a(v[i]),.q(sa[i]));
    layer uv(.a(v[i]),.q(sv[i]));
    assign packed_o[i]=s[i];
  end
  assign o=s;
  assign oa=sa;
endmodule

// top: scalar_layers
