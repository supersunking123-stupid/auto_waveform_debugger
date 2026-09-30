// Fixture for struct member decomposition with nested packed structs.
// Used (with RTL_TRACE_SIGNALS_RESERVE=1) to force the compile-time signal
// vector to reallocate while DecomposePackedStructFields is running.
typedef struct packed {
  logic [3:0] lo;
  logic [3:0] hi;
} inner_t;

typedef struct packed {
  logic       v;
  inner_t     a;
  inner_t     b;
  logic [1:0] c;
} outer_t;

module nested_leaf (
    input  outer_t in,
    output outer_t out
);
  outer_t s1, s2, s3;
  assign s1  = in;
  assign s2  = s1;
  assign s3  = s2;
  assign out = s3;
endmodule

module nested_top (
    input  outer_t in,
    output outer_t out
);
  outer_t t0, t1, t2;
  assign t0 = in;
  assign t1 = t0;
  assign t2 = t1;
  nested_leaf u0 (
      .in (t2),
      .out(out)
  );
endmodule
