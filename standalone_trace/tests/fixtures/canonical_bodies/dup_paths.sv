// top: dp2_top
// TODO item 4 regression (found on Lumion): distinct symbols with the same hierarchical path (loop variables
// of unnamed blocks) appear as duplicate string refs in one endpoint. The baseline keeps the duplicates, so the
// translated refs must not be de-duplicated.
module dp2_leaf (
    input  logic       clk,
    input  logic       en,
    input  logic [3:0] d,
    output logic [3:0] a,
    output logic [3:0] b
);
  always_comb begin
    a = '0;
    b = '0;
    if (en) begin
      for (int i = 0; i < 4; i++) a[i] = d[i];
      for (int i = 0; i < 4; i++) b[i] = a[3 - i];
    end
  end
  logic [3:0] r;
  always_ff @(posedge clk) begin
    for (int k = 0; k < 2; k++) r[k] <= a[k];
    for (int k = 2; k < 4; k++) r[k] <= b[k];
  end
endmodule

module dp2_top (
    input  logic       clk,
    input  logic       en,
    input  logic [3:0] d,
    output logic [3:0] a0,
    output logic [3:0] a1,
    output logic [3:0] b1
);
  dp2_leaf u0 (.clk(clk), .en(en), .d(d), .a(a0), .b());
  dp2_leaf u1 (.clk(clk), .en(~en), .d(a0), .a(a1), .b(b1));
  dp2_leaf u2 (.clk(clk), .en(en), .d(~d), .a(), .b());
endmodule
