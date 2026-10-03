module dynamic_scalar(input wire a, input wire idx, output logic [0:0] q);
  always_comb q[idx] = a;
endmodule
module scalar_mirror(input wire [7:0] v, input wire idx, output wire [7:0] o);
  wire s[7:0];
  for(genvar i=0;i<8;i++) begin: FOR_LAYER
    dynamic_scalar u(.a(v[i]),.idx(idx),.q(s[i]));
    assign o[i]=s[i];
  end
endmodule

// top: scalar_mirror
