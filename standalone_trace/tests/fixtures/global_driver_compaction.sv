// Clock/reset loads exceed the 1024-load compaction threshold. Registers
// retain their own writers even when their names match clock/reset names.
module global_driver_leaf (input logic clk, input logic core_rst_n, input logic d);
  logic first_vf_offset, tx_eq_rx_preset_hint;
  logic pclk, aclk, rst_n, rstn, resetn, core_rst_n_int;
  always_ff @(posedge clk or negedge core_rst_n) begin
    if (!core_rst_n) begin
      first_vf_offset <= 1'b0;
      tx_eq_rx_preset_hint <= 1'b0;
      pclk <= 1'b0;
      aclk <= 1'b0;
      rst_n <= 1'b0;
      rstn <= 1'b0;
      resetn <= 1'b0;
      core_rst_n_int <= 1'b0;
    end else begin
      first_vf_offset <= d;
      tx_eq_rx_preset_hint <= d;
      pclk <= d;
      aclk <= d;
      rst_n <= d;
      rstn <= d;
      resetn <= d;
      core_rst_n_int <= d;
    end
  end
endmodule

module global_driver_top (input logic clk, input logic core_rst_n, input logic d);
  for (genvar i = 0; i < 1100; i++) begin : g
    global_driver_leaf u (.clk(clk), .core_rst_n(core_rst_n), .d(d));
  end
endmodule
