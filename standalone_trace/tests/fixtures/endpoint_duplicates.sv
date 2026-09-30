// Repeated occurrences within one source range can have identical full endpoint keys.
module endpoint_duplicate_leaf (
    input logic a,
    input logic [3:0] vec,
    input logic [1:0][1:0] matrix,
    input logic [1:0] idx,
    output logic q, symbolic, multidim, tail
);
  assign q = a ^ a;                              // CASE repeated_plain
  always_comb symbolic = vec[idx] ^ vec[idx];    // CASE repeated_symbolic
  assign multidim = matrix[1][0] ^ matrix[1][0]; // CASE repeated_multidim
  assign tail = q | symbolic | multidim;         // CASE tail
endmodule

// 512 distinct sinks, with two occurrences each: raw loads reach 1024 while
// their full-key unique count is 512. Compaction must see the raw load count.
module endpoint_duplicate_global(input logic clk, output logic [511:0] sinks);
  for (genvar g = 0; g < 512; ++g) begin : rows
    logic sink;
    assign sink = clk ^ clk;
    assign sinks[g] = sink;
  end
endmodule

module endpoint_duplicates (
    input logic a,
    input logic [3:0] vec,
    input logic [1:0][1:0] matrix,
    input logic [1:0] idx,
    output logic tail0, tail1, tail2
);
  logic q0, q1, q2, s0, s1, s2, m0, m1, m2;
  endpoint_duplicate_leaf u0(.a(a), .vec(vec), .matrix(matrix), .idx(idx),
                            .q(q0), .symbolic(s0), .multidim(m0), .tail(tail0));
  endpoint_duplicate_leaf u1(.a(q0), .vec(vec), .matrix(matrix), .idx(idx),
                            .q(q1), .symbolic(s1), .multidim(m1), .tail(tail1));
  endpoint_duplicate_leaf u2(.a(a ^ a), .vec(vec), .matrix(matrix), .idx(idx),
                            .q(q2), .symbolic(s2), .multidim(m2), .tail(tail2));
endmodule
