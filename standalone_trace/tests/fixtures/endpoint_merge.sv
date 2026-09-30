// Fixture for endpoint bit-range merging (MergeEndpointBitRangesInPlace).
// The four single-bit loads of `a` in the first assign come from one
// assignment (same path/file/line/text/lhs/rhs) and collapse into a single
// "[3:0]" load endpoint; the generate loop's per-bit drivers of `g` collapse
// into "[3:0]" while the separate assign of g[4] stays its own endpoint.
// Multi-dimensional selects of `m` are not single ranges and must be left as
// written (they are neither merged nor rewritten).
module endpoint_merge (
    input  logic [7:0] a,
    input  logic [3:0][3:0] m,
    output logic [3:0] y,
    output logic [3:0] z,
    output logic [3:0] w,
    output logic [4:0] g
);
  assign y = {a[0], a[1], a[2], a[3]};
  assign z = a[7:4];
  assign w = {m[1][0], m[1][1], m[2][0], m[2][1]};
  for (genvar i = 0; i < 4; i++) begin : gen_g
    assign g[i] = a[i];
  end
  assign g[4] = a[4];
endmodule
