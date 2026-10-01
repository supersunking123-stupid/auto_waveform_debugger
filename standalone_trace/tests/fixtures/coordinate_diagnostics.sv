module coordinate_diagnostics(input logic[3:0][1:0] p, input logic[2:0] idx,
                              input logic[7:0] vector_only, output logic[7:0] y);
  logic[3:0][1:0] unused;
  typedef enum logic[1:0] { E0=0, E1=1 } enum_t;
  enum_t enums[1:0];
  typedef struct packed { logic[1:0] bits; } packet_t;
  packet_t [1:0] aggregates;
  logic[1:0] marker_samples;
  typedef struct packed { logic[3:0][1:0] matrix; logic[7:0] vector; } member_t;
  member_t member_packet;
  logic member_use, scalar_member_use;
  assign member_use = member_packet.matrix[2][1];
  assign scalar_member_use = member_packet.vector[2];
  assign y[0] = p[5][0]; // OOB_ELEMENT
  assign y[3:1] = p[1][4:2]; // OOB_RANGE
  assign y[4] = p[idx][0]; // DYNAMIC
  assign y[5] = enums[1][0]; // ENUM
  assign y[6] = aggregates[1].bits[0]; // AGGREGATE
  assign y[7] = vector_only[5]; // VECTOR
  assign marker_samples[0] = p[2'bxx][0]; // CONSTANT_XZ
  assign marker_samples[1] = p[64'h100000000][0]; // CONSTANT_OVERFLOW
endmodule
