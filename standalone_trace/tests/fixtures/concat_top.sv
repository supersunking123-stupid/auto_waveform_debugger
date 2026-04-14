// Fixture for testing multi-member port connections with struct fields.
// The concat expression {pkt.valid, pkt.code[0]} feeds a single output port.
// Both members must survive tracing.
typedef struct packed {
  logic       valid;
  logic [2:0] code;
  logic [3:0] data;
} pkt_t;

module concat_src (
    input  pkt_t  pkt,
    output logic [1:0] out
);
  assign out = {pkt.valid, pkt.code[0]};
endmodule

module concat_top (
    input  logic       clk,
    output logic [1:0] out
);
  pkt_t bus;
  assign bus.valid = 1'b1;
  assign bus.code  = 3'h5;
  assign bus.data  = 4'hA;

  concat_src u_src (.pkt(bus), .out(out));
endmodule
