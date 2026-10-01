// E4c: display intersections on merged root output copies only.
typedef struct packed { logic [3:0] hi; logic [3:0] lo; } clip_packet_t;
module clip_leaf(input logic [7:0] a, output logic [7:0] p);
  for (genvar i=0; i<8; i++) begin:g
    assign p[i] = a[i]; // CASE leaf_merged
  end
endmodule
module merged_range_clip(
    input logic [15:0] z,
    input logic [7:0] input_bits,
    input logic [2:0] sel,
    input logic [3:0][3:0] m,
    output logic [7:0] x, single, port_output, logical_rows,
    output logic [11:0] overlap,
    output logic [15:0] duplicate,
    output logic [15:0] repeated_slice,
    output logic [7:0] single_slice,
    output logic [3:0] nibble,
    output logic symbolic, logical_point);
  logic [15:0] y;
  clip_packet_t packet;
  for (genvar i=0; i<8; i++) begin:g
    assign x[i] = y[i+8]; // CASE root_merged
    assign y[i+8] = z[i+8]; // CASE deeper_merged
    assign packet[i] = input_bits[i]; // CASE member_merged
  end
  assign single = input_bits[7:0]; // CASE unmerged
  assign symbolic = input_bits[sel]; // CASE symbolic
  assign overlap = {m[1][0], m[2][0]}; // CASE distinct_original_keys
  assign duplicate = {m[1][0], m[1][0]}; // CASE identical_original_keys
  assign nibble = input_bits[3:0]; // CASE nibble_unmerged
  assign logical_rows = {m[1], m[2]}; // CASE logical_rows
  assign logical_point = m[1][0]; // CASE logical_point
  assign repeated_slice = {input_bits[7:0], input_bits[7:0]}; // CASE repeated_slice
  assign single_slice = input_bits[7:0]; // CASE single_slice
  clip_leaf u(.a(input_bits), .p(port_output));
endmodule
