// top: nc_top
// TODO item 4 risk case: multi-level sharing (core -> lane -> bit), port follows through several skipped
// levels, struct outputs, generate-loop instances, instance arrays, escaped instance names and sibling
// names that share a textual prefix (c1 / c10) for the path translation.
typedef struct packed {
  logic       v;
  logic [2:0] d;
} nc_pkt_t;

module nc_bit (
    input  logic    clk,
    input  logic    d,
    output logic    q,
    output nc_pkt_t pk
);
  logic r;
  always_ff @(posedge clk) r <= d;
  assign q  = r;
  assign pk = '{v: r, d: {3{d}}};
endmodule

module nc_lane #(parameter int N = 4) (
    input  logic         clk,
    input  logic [N-1:0] d,
    output logic [N-1:0] q,
    output logic         any_v
);
  nc_pkt_t pks[N];
  logic [N-1:0] vv;
  for (genvar i = 0; i < N; i++) begin : g_b
    nc_bit u_bit (.clk(clk), .d(d[i]), .q(q[i]), .pk(pks[i]));
    assign vv[i] = pks[i].v;
  end
  assign any_v = |vv;
endmodule

module nc_core (
    input  logic       clk,
    input  logic [7:0] d,
    output logic [7:0] q,
    output logic [1:0] av
);
  nc_pkt_t tap;
  nc_lane #(4) l0 (.clk(clk), .d(d[3:0]), .q(q[3:0]), .any_v(av[0]));
  nc_lane #(4) l1 (.clk(clk), .d(d[7:4]), .q(q[7:4]), .any_v(av[1]));
  nc_lane #(2) l2 (.clk(clk), .d(d[1:0]), .q(), .any_v());
  nc_bit b_tap (.clk(clk), .d(d[0]), .q(), .pk(tap));
endmodule

module nc_top (
    input  logic       clk,
    input  logic [7:0] d,
    output logic [7:0] q0,
    output logic [7:0] q1,
    output logic [7:0] q2,
    output logic [7:0] q3,
    output logic [1:0] av0,
    output logic [1:0] av1
);
  nc_core c0 (.clk(clk), .d(d), .q(q0), .av(av0));
  nc_core c1 (.clk(clk), .d(q0), .q(q1), .av(av1));
  nc_core c10 (.clk(clk), .d(~d), .q(q2), .av());
  nc_core \c.x (.clk(clk), .d(q1), .q(q3), .av());
  nc_bit arr[3:0] (.clk(clk), .d(d[3:0]), .q(), .pk());
endmodule
