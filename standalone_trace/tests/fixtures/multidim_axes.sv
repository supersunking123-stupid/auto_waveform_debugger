// Logical declared-axis metadata for ordinary packed and unpacked arrays.
module axis_leaf(input logic [3:0][3:0] m, output logic y);
  assign y = m[1][0]; // CASE instance_p10
endmodule
module multidim_axes(
    input logic [3:0][3:0] p,
    input logic [0:3][3:0] ascending_outer,
    input logic [8:5][7:4] shifted,
    input logic [-1:-4][3:0] signed_outer,
    input logic [3:0][0:3] ascending_inner,
    input logic [2:0][1:0][3:0] three_axes,
    input logic [7:4] unpacked_desc [3:0],
    input logic [7:4] unpacked_asc [0:3],
    input logic [1:0] sel,
    output logic [63:0] out
);
  localparam int ROW = 2;
  assign out[0] = p[1][0]; // CASE p10
  assign out[1] = p[1][1]; // CASE p11
  assign out[2] = p[2][0]; // CASE p20
  assign out[3] = p[2][1]; // CASE p21
  assign out[7:4] = p[1]; // CASE row1
  assign out[8] = ascending_outer[1][0]; // CASE ascending10
  assign out[9] = ascending_outer[2][0]; // CASE ascending20
  assign out[10] = shifted[6][4]; // CASE shifted64
  assign out[11] = shifted[7][5]; // CASE shifted75
  assign out[12] = signed_outer[-2][0]; // CASE signed20
  assign out[13] = signed_outer[-3][1]; // CASE signed31
  assign out[15:14] = ascending_inner[1][1 +: 2]; // CASE indexed_up
  assign out[17:16] = ascending_inner[2][2 -: 2]; // CASE indexed_down
  assign out[18] = unpacked_desc[1][4]; // CASE unpacked_desc14
  assign out[19] = unpacked_desc[2][5]; // CASE unpacked_desc25
  assign out[20] = unpacked_asc[1][4]; // CASE unpacked_asc14
  assign out[21] = unpacked_asc[2][5]; // CASE unpacked_asc25
  assign out[22] = three_axes[2][1][0]; // CASE three210
  assign out[23] = three_axes[1][0][3]; // CASE three103
  assign out[47:36] = p[3:1]; // CASE range321
  assign out[49:48] = p[1][2:1]; // CASE range1121
  assign out[26] = p[sel][0]; // CASE dynamic_outer0
  assign out[27] = p[1][sel]; // CASE dynamic_inner1
  logic [1:0] unknown_index;
  assign unknown_index = 2'bxx;
  assign out[28] = p[unknown_index][0]; // CASE unknown_outer0
  assign out[29] = p[ROW][1]; // CASE parameter21
  assign out[57:50] = {p[1], p[2]}; // CASE merged_rows12
  assign out[58] = p[sel[0]][0]; // CASE nested_dynamic_outer0
  logic [3:0] dynamic_values[];
  logic [3:0] queue_values[$];
  logic [3:0] associative_values[string];
  typedef struct packed { logic [3:0][3:0] member; logic spare; } packet_t;
  packet_t packet;
  packet_t whole_packet;
  logic [16:0] whole_result;
  assign whole_result = whole_packet; // CASE whole_struct
  logic [7:0] \bus[foo] ;
  logic [7:0] \bus[foo][bar] ;
  assign \bus[foo]  = out[7:0]; // CASE escaped_foo
  assign \bus[foo][bar]  = out[15:8]; // CASE escaped_foo_bar
  logic dynamic_result, queue_result, associative_result, member_result;
  always_comb dynamic_result = dynamic_values[sel][0]; // CASE dynamic_array0
  always_comb queue_result = queue_values[$][0]; // CASE queue_last0
  always_comb associative_result = associative_values["key"][0]; // CASE associative_key0
  always_comb member_result = packet.member[1][0]; // CASE struct_member10
  assign out[30] = dynamic_result;
  assign out[31] = queue_result;
  assign out[32] = associative_result;
  assign out[33] = member_result;
  axis_leaf u[2:1](.m(p), .y(out[35:34]));
endmodule
