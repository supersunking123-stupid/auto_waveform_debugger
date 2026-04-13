// Fixture for testing struct member access tracing (Level 1).
// pkt_t: packed struct with 3 members spanning 8 bits total.
//   valid = bit 0, code = bits [3:1], data = bits [7:4]
typedef struct packed {
  logic       valid;
  logic [2:0] code;
  logic [3:0] data;
} pkt_t;

module struct_prod (
    input  logic clk,
    input  logic rst_n,
    output pkt_t  pkt
);
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      pkt.valid <= 1'b0;
      pkt.code  <= 3'h0;
      pkt.data  <= 4'h0;
    end else begin
      pkt.valid <= 1'b1;
      pkt.code  <= pkt.code + 3'h1;
      pkt.data  <= pkt.data + 4'h1;
    end
  end
endmodule

module struct_cons (
    input  pkt_t  pkt,
    output logic  hit,
    output logic [1:0] data_lo
);
  assign hit     = pkt.valid & pkt.code[0];
  assign data_lo = pkt.data[1:0];
endmodule

module struct_top (
    input  logic       clk,
    input  logic       rst_n,
    output logic       hit,
    output logic [1:0] data_lo
);
  pkt_t bus;

  struct_prod u_prod (
    .clk   (clk),
    .rst_n (rst_n),
    .pkt   (bus)
  );

  struct_cons u_cons (
    .pkt     (bus),
    .hit     (hit),
    .data_lo (data_lo)
  );
endmodule
