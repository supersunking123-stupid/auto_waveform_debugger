// top: fallback_contract
module fallback_scalar(input logic a, input logic idx, output logic q);
  always_comb q = a;
endmodule
module fallback_vector(input logic [3:0] d, output logic [3:0] q);
  always_comb q = d;
endmodule
module fallback_contract(input logic [3:0] a, b, input logic choose,
                         input logic [2:0] idx, output wire [7:0] o,
                         output logic [3:0] y);
  logic arr [7:0];
  logic z;
  fallback_scalar u(.a(arr[idx]), .idx(choose), .q(z));
  for (genvar i=0; i<8; i++) begin: G
    assign arr[i] = a[i%4];
    assign o[i] = arr[i];
  end
  fallback_vector v(.d(choose ? a : b), .q(y));
endmodule
