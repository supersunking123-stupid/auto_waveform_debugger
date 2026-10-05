// A 2-bit clock vector with more than 1024 loads, so it is compacted.
module gleaf(input logic clk, input logic d, output logic q);
  always_ff @(posedge clk) q <= d;
endmodule
module gclk(input logic [1:0] clk_v, input logic d, output logic [1199:0] q);
  for (genvar i = 0; i < 1200; i++) begin : g
    gleaf u(.clk(clk_v[i % 2]), .d(d), .q(q[i]));
  end
  // Q loads clk_v[0] => only the 600 even g[i].u instances (not all 1200)
endmodule
