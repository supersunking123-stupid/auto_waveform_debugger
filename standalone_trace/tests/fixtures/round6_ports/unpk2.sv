module lay(input wire clk, input wire [3:0] rec, output wire req_index_atop);
  reg r;
  always @(posedge clk) r <= ^rec;
  assign req_index_atop = r;
endmodule
module top #(parameter W = 8) (input wire clk, input wire [W-1:0] v, input wire [3:0] k_l_records [W-1:0], output wire [W-1:0] o);
  wire l_index_atop[W-1:0];
  genvar i;
  generate
    for (i = 0; i < W; i = i + 1) begin : FOR_LAYER
      lay u(.clk(clk), .rec(k_l_records[i]), .req_index_atop(l_index_atop[i]));
      assign o[i] = v[i] && l_index_atop[i];
    end
  endgenerate
endmodule

// top: top
