module producer (
    input  logic       clk,
    input  logic       rst_n,
    output logic [7:0] data
);
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      data <= 8'h00;
    end else begin
      data <= data + 8'h01;
    end
  end
endmodule

module consumer (
    input  logic [7:0] in_bus,
    output logic       hit,
    output logic [3:0] hi_nibble
);
  assign hit = in_bus[3] & in_bus[1];
  assign hi_nibble = in_bus[7:4];
endmodule

module semantic_top (
    input  logic       clk,
    input  logic       rst_n,
    output logic       hit,
    output logic [3:0] hi_nibble,
    output logic       flag
);
  logic [7:0] data;

  producer u_prod (
      .clk (clk),
      .rst_n(rst_n),
      .data (data)
  );

  consumer u_cons (
      .in_bus   (data),
      .hit      (hit),
      .hi_nibble(hi_nibble)
  );

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      flag <= 1'b0;
    end else if (data[0]) begin
      flag <= hit;
    end
  end
endmodule
