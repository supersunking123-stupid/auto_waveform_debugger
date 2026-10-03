module lo4(input logic [3:0] a, output logic [3:0] q);
  assign q[1:0] = a[1:0]; // L q_lo
  assign q[3:2] = a[3:2]; // L q_hi
endmodule
module lo8(input logic [7:0] a, output logic [7:0] q);
  assign q[3:0] = a[3:0]; // L q8_lo
  assign q[7:4] = a[7:4]; // L q8_hi
endmodule
module los(input logic signed [3:0] a, output logic signed [3:0] q);
  assign q = a; // L qs
endmodule
module owide(input logic [7:0] i, output logic [7:0] y0, y1, y2, output logic [1:0] y3);
  logic [7:0] w8; logic [3:0] w4; logic [7:0] ws; logic [7:0] wc;
  lo4 u4 (.a(i[3:0]), .q(w8));          // 4-bit output into 8-bit actual
  lo8 u8 (.a(i), .q(w4));               // 8-bit output into 4-bit actual
  los us (.a(i[3:0]), .q(ws));          // signed 4 -> 8 actual (sign extended? no: output assigns actual = q)
  lo4 uc (.a(i[7:4]), .q({wc[1:0], wc[7:6]}));
  assign y0 = w8; assign y1 = {4'b0, w4}; assign y2 = ws; assign y3 = wc[7:6];
  // Q drivers w8[1] => q_lo
  // Q drivers w8[3] => q_hi
  // Q drivers w8[6] =>
  // Q drivers w4[3] => q8_lo
  // Q drivers ws[1] => qs
  // Q drivers ws[7] => qs
  // Q drivers wc[7] => q_hi
  // Q drivers wc[6] => q_hi
  // Q drivers wc[1] => q_lo
  // Q drivers wc[3] =>
endmodule

// top: owide
