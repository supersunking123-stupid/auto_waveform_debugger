// Fixture for endpoint bit-range merging (MergeEndpointBitRangesInPlace).
// The four single-bit loads of `a` in the first assign come from one
// assignment (same path/file/line/text/lhs/rhs) and should collapse into a
// single "a[3:0]" load endpoint when merging works (see
// RTL_TRACE_FIX_ENDPOINT_MERGE in standalone_trace/docs).
module endpoint_merge (
    input  logic [7:0] a,
    output logic [3:0] y,
    output logic [3:0] z
);
  assign y = {a[0], a[1], a[2], a[3]};
  assign z = a[7:4];
endmodule
