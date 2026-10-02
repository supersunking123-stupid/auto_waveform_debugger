module member_loads(input logic a, b, input logic [1:0] idx,
                    output logic [10:0] y);
  typedef struct packed {
    logic [3:0][1:0] matrix;
    logic [7:0] vector;
  } packet_t;
  packet_t packet, whole;
  assign packet.matrix[2][1] = a;
  assign packet.vector[2] = b;
  assign y[0] = packet.matrix[2][1];
  assign y[1] = packet.vector[2];
  assign whole = '0;
  assign y[2] = whole.matrix[2][1];
  assign y[3] = whole.vector[2];

  // A scalar member at a nonzero parent offset tests local-to-parent selection.
  typedef struct packed {
    logic [7:0] vector;
    logic [3:0][1:0] matrix;
  } reversed_t;
  reversed_t reversed_packet;
  assign reversed_packet.vector[2] = b;
  assign reversed_packet.matrix[2][1] = a;
  assign y[4] = reversed_packet.vector[2];

  // Unpacked structs do not yet have Level-2 member signal rows. Preserve the
  // whole-signal query and explicit missing-member behavior in both modes.
  typedef struct {
    logic [3:0][1:0] matrix;
    logic [7:0] vector;
  } unpacked_packet_t;
  unpacked_packet_t unpacked_packet;
  assign unpacked_packet.matrix[2][1] = a;
  assign unpacked_packet.vector[2] = b;
  assign y[5] = unpacked_packet.vector[2];
  logic [1:0] unpacked_matrix [3:0];
  assign unpacked_matrix[2][1] = a;
  assign unpacked_matrix[3][0] = b;
  assign y[6] = unpacked_matrix[2][1];
  assign y[7] = unpacked_matrix[idx][0];

  // Repeated field names expose legacy nested rows made from the root type.
  // The actual nested matrix bit below is root bit 13, despite those rows.
  typedef struct packed { logic [3:0][1:0] a; logic [3:0] b; } inner_t;
  typedef struct packed { inner_t a; logic [7:0] b; } outer_t;
  outer_t nested_packet;
  assign nested_packet.a.a[0][1] = a;
  assign nested_packet.a.b[1] = b;
  assign nested_packet.b[1] = a;
  assign y[8] = nested_packet.a.a[0][1];
  assign y[9] = nested_packet.a.b[1];
  assign y[10] = nested_packet.b[1];
endmodule
