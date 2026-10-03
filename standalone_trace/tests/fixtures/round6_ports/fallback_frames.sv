// top: fallback_frames
module fallback_frame_leaf(input logic d, output logic q);
  assign q = d;
endmodule
module fallback_frame_array #(parameter int L=7, R=0)
    (input logic [1:0] a, input int idx, output logic q);
  wire arr [L:R];
  assign arr[L] = a[0];
  assign arr[R] = a[1];
  fallback_frame_leaf u(.d(arr[idx]), .q(q));
endmodule
module fallback_frames(input logic [1:0] a, input int idx, output logic [2:0] q);
  fallback_frame_array #(7,0) descending(.a(a),.idx(idx),.q(q[0]));
  fallback_frame_array #(0,7) ascending(.a(a),.idx(idx),.q(q[1]));
  fallback_frame_array #(11,4) nonzero(.a(a),.idx(idx),.q(q[2]));
endmodule
