module leaf #(parameter int W = 1) ();
endmodule

module child #(parameter int WIDTH = 8, parameter type T = logic [7:0]) ();
  if (WIDTH > 4) begin : g_big
    leaf #(.W(WIDTH)) u_leaf ();
  end
endmodule

module top ();
  child #(.WIDTH(8), .T(logic [15:0])) u0 ();
  child #(.WIDTH(4), .T(logic [3:0])) u1 ();
  leaf #(.W(2)) u_arr [0:1] ();
endmodule
