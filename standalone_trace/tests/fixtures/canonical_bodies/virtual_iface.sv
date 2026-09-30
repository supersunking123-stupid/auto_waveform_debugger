// top: vi_top
// TODO item 4 risk case: virtual interfaces inside shared module bodies.
interface vi_if;
  logic [3:0] data;
  logic       valid;
endinterface

module vi_drv (
    input logic       clk,
    input logic [3:0] d
);
  vi_if loc ();
  virtual vi_if vif;
  logic [3:0] snap;
  initial vif = loc;
  assign loc.data = d;
  always_ff @(posedge clk) loc.valid <= |d;
  always @(posedge clk) snap <= vif.data;
endmodule

// Placeholder created two levels down (inside a generate block of a shared body), and a vif read that feeds a
// port so that traces leave the shared body.
module vi_wrap (
    input  logic       clk,
    input  logic [3:0] d,
    output logic [3:0] q
);
  vi_drv u_d (.clk(clk), .d(d));
  if (1) begin : g
    vi_if gl ();
    virtual vi_if gv;
    initial gv = gl;
    assign gl.data = ~d;
    always_comb q = gv.data ^ u_d.snap;
  end
endmodule

module vi_top (
    input  logic       clk,
    input  logic [3:0] a,
    input  logic [3:0] b,
    output logic [3:0] y0,
    output logic [3:0] y1
);
  vi_drv u0 (.clk(clk), .d(a));
  vi_drv u1 (.clk(clk), .d(b));
  vi_drv u2 (.clk(clk), .d(a ^ b));
  vi_wrap w0 (.clk(clk), .d(a), .q(y0));
  vi_wrap w1 (.clk(clk), .d(b), .q(y1));
endmodule
