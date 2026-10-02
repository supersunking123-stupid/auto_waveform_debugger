// top: shared_clock_top
module shared_clock_leaf(input wire clk,input wire rst_n,input wire[4:0]d);
  reg[4:0]meta;
  for(genvar i=0;i<5;i++)begin:gen_sync
    reg[1:0]chain;
    always @(posedge clk or negedge rst_n)begin
      if(!rst_n)begin meta[i]<=0;chain<=0;end
      else begin meta[i]<=d[i];chain<={chain[0],meta[i]};end
    end
  end
endmodule
module shared_clock_top(input wire clk,input wire rst_n,input wire[4:0]d);
  shared_clock_leaf u(.clk(clk),.rst_n(rst_n),.d(d));
endmodule
module compact_shared_clock_top(input wire clk,input wire rst_n,input wire[4:0]d);
  for(genvar i=0;i<250;i++)begin:L
    shared_clock_leaf u(.clk(clk),.rst_n(rst_n),.d(d));
  end
endmodule
