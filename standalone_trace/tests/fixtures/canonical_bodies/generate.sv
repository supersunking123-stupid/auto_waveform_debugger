// top: gen_top
// TODO item 4 risk case: generate branches selected by parameters, generate loops and instance arrays.
module gen_unit #(parameter int MODE = 0, parameter int N = 4) (
    input  logic         clk,
    input  logic [N-1:0] d,
    output logic [N-1:0] q
);
  if (MODE == 0) begin : g_pass
    assign q = d;
  end else if (MODE == 1) begin : g_reg
    logic [N-1:0] r;
    always_ff @(posedge clk) r <= d;
    assign q = r;
  end else begin : g_inv
    for (genvar i = 0; i < N; i++) begin : g_bit
      logic b;
      assign b = ~d[i];
      assign q[i] = b;
    end
  end
  case (MODE)
    0: begin : c_zero
      logic z;
      assign z = d[0];
    end
    default: begin : c_other
      logic z;
      assign z = d[N-1];
    end
  endcase
endmodule

module gen_top (
    input  logic       clk,
    input  logic [3:0] a,
    output logic [3:0] y0,
    output logic [3:0] y1,
    output logic [3:0] y2,
    output logic [3:0] y3,
    output logic [3:0] y4,
    output logic [3:0] ya [0:3]
);
  gen_unit #(.MODE(0)) u0 (.clk(clk), .d(a), .q(y0));
  gen_unit #(.MODE(1)) u1 (.clk(clk), .d(y0), .q(y1));
  gen_unit #(.MODE(2)) u2 (.clk(clk), .d(y1), .q(y2));
  gen_unit #(.MODE(1)) u3 (.clk(clk), .d(y2), .q(y3));
  gen_unit #(.MODE(2)) u4 (.clk(clk), .d(y3), .q(y4));
  for (genvar k = 0; k < 3; k++) begin : g_arr
    logic [3:0] o;
    gen_unit #(.MODE(k)) u (.clk(clk), .d(a ^ 4'(k)), .q(o));
  end
  gen_unit #(.MODE(2)) ua [0:3] (.clk(clk), .d(a), .q(ya));
endmodule
