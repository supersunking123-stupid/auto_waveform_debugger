module concat_source(input wire [2:0] d, output wire [2:0] m_data);
  assign m_data = d;
endmodule
module dangling_concat_loads(input wire [2:0] d, output wire dangling,
                             output wire dangling_chk, output wire y);
  wire used;
  concat_source stage(.d(d), .m_data({dangling_chk, dangling, used}));
  assign y = used;
endmodule
