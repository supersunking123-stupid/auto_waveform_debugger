typedef struct packed {
 logic lm_insert_enderror_for_pkt_in_flight_at_ld;
 logic [21:0] reserved;
 logic expect_enderror_wo_end;
 logic expect_discarded_end;
 logic expect_bus_valid_without_tlp;
 logic expect_gaps_before_start;
 logic valid_can_go_down_in_between_a_packet;
 logic vaid_data_can_change_without_ready;
 logic valid_may_not_get_ready;
 logic data_can_be_x_without_valid;
 logic expect_masked_sop_to_discard_pkt;
} hlsif_chk_dbg_k_controls_t;
module struct_leaf(input logic clk, input logic rst_n, input hlsif_chk_dbg_k_controls_t k_controls,
 output logic observed);
 logic pkt_in_flight_q;
 always @(posedge clk or negedge rst_n)
   if(!rst_n) pkt_in_flight_q<=0;
   else if(k_controls.lm_insert_enderror_for_pkt_in_flight_at_ld) pkt_in_flight_q<=1;
 wire skip_first_eop=k_controls.expect_discarded_end & pkt_in_flight_q;
 assign observed=skip_first_eop;
endmodule
module struct_boundary_repro(input logic clk,rst_n,input logic[7:0] input_pattern,input logic[1:0] k_controls,input logic es_link_down_reset_i,output wire observed);
 localparam NUM_PORTS=1;
 localparam BUFFER_WRITE__VARIANT=1;
 wire [7:0] k_traffic_pattern=input_pattern;
 wire [7:0] k_traffic_pattern_array[0:0];
 assign k_traffic_pattern_array[0]=k_traffic_pattern;
 if(1)begin:sva
  for(genvar gv_x=0;gv_x<NUM_PORTS;gv_x=gv_x+1)begin:port_in__gen
   struct_leaf in_port_dbgchk(.clk(clk),.rst_n(rst_n),.observed(observed),
    .k_controls({26'h0,(BUFFER_WRITE__VARIANT?1'h1:1'b0),
      ((BUFFER_WRITE__VARIANT || k_controls[0] || es_link_down_reset_i)?1'h1:1'b0),
      k_traffic_pattern_array[gv_x][2],1'h0,1'h0,~k_traffic_pattern_array[gv_x][3]}));
  end
 end
endmodule
