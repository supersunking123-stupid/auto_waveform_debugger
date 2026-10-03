// top: generated_fallback
module generated_source(input wire [7:0] a, output wire [7:0] data);
  for (genvar lane=0; lane<8; lane++) begin: lanes
    assign data[lane]=a[lane];
  end
endmodule
module generated_sink(input wire d, output wire q);
  assign q=d;
endmodule
module generated_wrapper(input wire [7:0] data, enable, output wire [7:0] q);
  for (genvar lane=0; lane<8; lane++) begin: lanes
    generated_sink u(.d(data[lane]&enable[lane]),.q(q[lane]));
  end
endmodule
module generated_fallback(input wire [7:0] a, enable, output wire [7:0] q,
                          output wire exact_q);
  wire [7:0] data;
  generated_source producer(.a(a),.data(data));
  generated_wrapper wrapper(.data(data),.enable(enable),.q(q));
  generated_sink exact_neighbor(.d(data[2]),.q(exact_q));
endmodule
