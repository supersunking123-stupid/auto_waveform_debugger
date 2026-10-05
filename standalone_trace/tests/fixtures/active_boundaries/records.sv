// Proposed minimization only. Frozen MAIN/bdb behavior is NOT yet executed.
// Query: drivers active_boundary_repro.lut.FOR_LAYER[7].layer.FOR_SLICES[2].slice.req_entry_index
module req_slice #(parameter IDX_WD=7,SLICE_IDX=0,RECORDS_W_DATA_REGS=0)(input wire [IDX_WD-1:0] req_entry_index,
                                     output wire observed);
  reg [0:0] data_r[127:0],next_data[127:0];
 generate
  if (|RECORDS_W_DATA_REGS) begin : IF_GEN_DATA_REGS
   for(genvar j=0;j<RECORDS_W_DATA_REGS;j++)begin:FOR_DATA
    always @(*) begin
     next_data[j]=data_r[j];
     if(req_entry_index == j[IDX_WD-1:0]) next_data[j]=1'b1;
    end
   end
   assign observed=next_data[0];
  end else begin:ELSE_GEN_DATA_REGS
   assign observed=1'b0;
  end
 endgenerate
endmodule
module req_layer #(parameter SLICE_WD=128, LAYER_WD=4,LAYER_IDX=0,RECORDS_W_DATA_REGS=0,
                   localparam S_IDX_WD=$clog2(SLICE_WD),
                   localparam IDX_WD=$clog2(LAYER_WD)+S_IDX_WD)
                  (input wire [IDX_WD-1:0] req_entry_index, output wire [LAYER_WD-1:0] observed);
  for(genvar gv_x=0;gv_x<LAYER_WD;gv_x++)begin:FOR_SLICES
    req_slice #(.IDX_WD(S_IDX_WD),.SLICE_IDX(LAYER_IDX*LAYER_WD+gv_x),.RECORDS_W_DATA_REGS((RECORDS_W_DATA_REGS>gv_x*128)?RECORDS_W_DATA_REGS-gv_x*128:0)) slice(.req_entry_index(req_entry_index[S_IDX_WD-1:0]),.observed(observed[gv_x]));
  end
endmodule
module req_lut #(parameter KMAX_LUT_INDEX_WD=12, LAYERS=8,
                 localparam LIDX_WD=KMAX_LUT_INDEX_WD-$clog2(LAYERS))
               (input wire [KMAX_LUT_INDEX_WD-1:0] req_entry_index,output wire [LAYERS-1:0] observed);
  for(genvar i=0;i<LAYERS;i++)begin:FOR_LAYER
    wire [3:0] local_observed;
    req_layer #(.LAYER_IDX(i),.RECORDS_W_DATA_REGS((i==0)?32:0)) layer(.req_entry_index(req_entry_index[LIDX_WD-1:0]),.observed(local_observed));
    assign observed[i]=^local_observed;
  end
endmodule
module req_writer(input wire [11:0] input_index,output wire [11:0] np_wr_req_entry_index);
  wire [11:0] req_entry_index_r=input_index;
  assign np_wr_req_entry_index = req_entry_index_r;
endmodule
module active_boundary_repro(input wire [11:0] input_index,output wire [7:0] observed);
  wire [11:0] np_wr_req_entry_index;
  req_writer split(.input_index(input_index),.np_wr_req_entry_index(np_wr_req_entry_index));
  req_lut lut(.req_entry_index(np_wr_req_entry_index),.observed(observed));
endmodule
