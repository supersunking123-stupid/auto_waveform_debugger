module cellm #(parameter int W = 2, parameter int O = 0) (input logic [7:0] d, output logic [7:0] q);
  inner #(.W(W)) i (.d(d[O +: W]), .q(q[O +: W]));
endmodule
module inner #(parameter int W = 2) (input logic [W-1:0] d, output logic [W-1:0] q); endmodule
module canon(input logic [3:0] i0, i1, output logic [7:0] y0, y1, y2);
  logic [7:0] x;
  assign x[1:0] = i0[1:0]; // L b0
  assign x[3:2] = i0[3:2]; // L b1
  assign x[5:4] = i1[1:0]; // L b2
  assign x[7:6] = i1[3:2]; // L b3
  cellm #(.W(2), .O(0)) c0 (.d(x), .q(y0));
  cellm #(.W(2), .O(4)) c1 (.d(x), .q(y1));
  cellm #(.W(4), .O(2)) c2 (.d({x[3:0], x[7:4]}), .q(y2));
  // Q drivers c0.i.d => b0
  // Q drivers c1.i.d => b2
  // Q drivers c2.i.d[0] => b3
  // Q drivers c2.i.d[3] => b0
  // Q drivers c2.i.d => b0 b3
  // Q loads x[4] => 
endmodule
