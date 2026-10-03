module leaf(input logic [3:0] d, output logic [3:0] q); endmodule
module gv4(input logic [3:0] x, output logic [3:0] y);
  leaf u0 (.d(x), .q(y));
endmodule

// top: gv4
