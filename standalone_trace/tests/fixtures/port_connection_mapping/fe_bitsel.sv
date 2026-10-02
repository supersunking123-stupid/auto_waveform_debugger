module child #(parameter EN = 0) (input logic [3:0] d, output logic [3:0] q);
  if (EN) begin : g_on
    assign q = d;
  end
endmodule
module top(input logic [3:0] a, b, c, e, output logic [3:0] o0, o1, o2, o3);
  logic [15:0] x, w;
  assign x[3:0] = a;
  assign x[7:4] = b;
  assign x[11:8] = c;
  assign x[15:12] = e;
  for (genvar g = 0; g < 4; g++) begin : L
    child u(.d(x[g*4+:4]), .q(w[g*4+:4]));
  end
  assign o0 = w[3:0];
  assign o1 = w[7:4];
  assign o2 = w[11:8];
  assign o3 = w[15:12];
endmodule
