// top: full_local_bridge_cases
module bridge_dyn_input(input logic [3:0] d,input logic [2:0] index,output wire q);
  assign q=d[index];
endmodule
module bridge_forward_leaf(input logic [3:0] d,output wire q);
  assign q=d[0];
endmodule
`define BRIDGE_READ(DATA,INDEX) DATA[INDEX+:1]
module bridge_mixed_input(input logic [3:0] d,input logic [2:0] index,output wire [2:0] q);
  assign q[0]=d[0];
  assign q[1]=`BRIDGE_READ(d,index);
  bridge_forward_leaf child(.d(d),.q(q[2]));
endmodule
module bridge_static_input(input logic [3:0] d,output wire [3:0] q);
  for(genvar i=0;i<4;i=i+1) begin:g
    assign q[i]=d[i];
  end
endmodule
module bridge_full_plus_dynamic(input logic [3:0] d,input logic [2:0] index,output wire [3:0] q,output wire r);
  assign q=d;
  assign r=d[index];
endmodule
module bridge_nonzero_input(input logic [7:4] d,input logic [2:0] index,output wire q);
  assign q=d[index];
endmodule
module bridge_dyn_output(input logic value,input logic [2:0] index,output logic [3:0] q);
  always @* q[index]=value;
endmodule
module bridge_mixed_output(input logic value,input logic [2:0] index,output logic [3:0] q);
  always @* begin
    q[0]=value;
    q[index]=value;
  end
endmodule
module full_local_bridge_cases(input logic clk,input logic rst,input logic [15:0] seed,
                              input logic [2:0] index,output wire [3:0] sink0,output wire [3:0] sink1,
                              output wire [3:0] neighbor0,output wire [3:0] neighbor1);
  logic [15:0] x;
  always @(posedge clk) begin
    if(rst) x[11:8]<=0; // CHECK chosen_reset
    else x[11:8]<=seed[11:8]; // CHECK chosen_data
    x[7:4]<=seed[7:4]; // CHECK neighbor_data
  end
  wire [3:0] z;
  assign z=seed[3:0]; // CHECK other_data
  wire a,b,e;
  wire [2:0] c;
  wire [3:0] f,g;
  bridge_dyn_input dyn_u(.d(x[11:8]),.index(index),.q(a));
  bridge_dyn_input other_u(.d(z),.index(index),.q(b));
  bridge_mixed_input mixed_u(.d(x[11:8]),.index(index),.q(c));
  bridge_static_input static_u(.d(x[11:8]),.q(f));
  bridge_full_plus_dynamic complete_u(.d(x[11:8]),.index(index),.q(g),.r());
  bridge_nonzero_input nonzero_u(.d(x[11:8]),.index(index),.q(e));
  wire [15:0] w0,w1;
  bridge_dyn_output dyn_out(.value(seed[0]),.index(index),.q(w0[11:8]));
  bridge_mixed_output mixed_out(.value(seed[1]),.index(index),.q(w1[11:8]));
  assign sink0=w0[11:8]; // CHECK consumer0
  assign sink1=w1[11:8]; // CHECK consumer1
  assign neighbor0=w0[7:4]; // CHECK neighbor_consumer0
  assign neighbor1=w1[7:4]; // CHECK neighbor_consumer1
endmodule
