// top: pt_top
// TODO item 4 risk case: `parameter type`. Matching types (typedef alias of the same packed type) may
// share a canonical body; structs with different layouts must not.
typedef logic [7:0] pt_byte_t;
typedef struct packed {
  logic       v;
  logic [6:0] d;
} pt_pkt_t;
typedef struct packed {
  logic [3:0] x;
  logic       v;
  logic [2:0] y;
} pt_pkt2_t;

module pt_cell #(parameter type T = logic [7:0]) (
    input  logic clk,
    input  T     d,
    output T     q
);
  T r;
  always_ff @(posedge clk) r <= d;
  assign q = r;
endmodule

module pt_sel #(parameter type T = pt_pkt_t) (
    input  logic clk,
    input  T     d,
    output logic v,
    output logic late_v
);
  T r;
  always_ff @(posedge clk) r <= d;
  assign v = d.v;
  assign late_v = r.v;
endmodule

module pt_top (
    input  logic     clk,
    input  logic [7:0] a,
    output logic [7:0] y0,
    output logic [7:0] y1,
    output logic [7:0] y2,
    output logic [7:0] y3,
    output pt_pkt_t  p0,
    output logic [3:0] vs
);
  pt_cell #(.T(logic [7:0])) c0 (.clk(clk), .d(a), .q(y0));
  pt_cell #(.T(pt_byte_t)) c1 (.clk(clk), .d(y0), .q(y1));
  pt_cell #(.T(logic [7:0])) c2 (.clk(clk), .d(y1), .q(y2));
  pt_cell #(.T(pt_pkt_t)) c3 (.clk(clk), .d(pt_pkt_t'(y2)), .q(p0));
  pt_cell c4 (.clk(clk), .d(y2), .q(y3));
  pt_sel #(.T(pt_pkt_t)) s0 (.clk(clk), .d(pt_pkt_t'(a)), .v(vs[0]), .late_v());
  pt_sel #(.T(pt_pkt2_t)) s1 (.clk(clk), .d(pt_pkt2_t'(a)), .v(vs[1]), .late_v());
  pt_sel #(.T(pt_pkt_t)) s2 (.clk(clk), .d(pt_pkt_t'(y0)), .v(vs[2]), .late_v());
  pt_sel #(.T(pt_pkt2_t)) s3 (.clk(clk), .d(pt_pkt2_t'(y1)), .v(vs[3]), .late_v());
endmodule
