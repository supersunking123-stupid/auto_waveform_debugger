// top: inactive_top
// Item 6: two actual instances of each parameter class force canonical redirection.
module inactive_child(input logic d, output logic q);
  assign q = d; // CASE child_active
endmodule
module inactive_unit #(parameter bit EN = 0, parameter int N = 0)
    (input logic [15:0] a, output logic [15:0] q);
  assign q[0] = a[0]; if (0) begin : g_same_line assign q[1] = a[1]; end assign q[2] = a[2]; // CASE same_line
  if (EN) begin : g_enabled
    inactive_child u_child(.d(a[3]), .q(q[3])); // CASE enabled_child
  end else begin : g_disabled
    assign q[4] = a[4]; // CASE disabled_class_active
  end
  for (genvar k = 0; k < N; ++k) begin : g_loop
    assign q[5+k] = a[5+k]; // CASE loop
  end
  case (EN)
    0: begin : g_case_zero
      assign q[7] = a[7]; // CASE case_zero
    end
    1: begin : g_case_one
      assign q[8] = a[8]; // CASE case_one
    end
    default: begin : g_case_default
      assign q[9] = a[9]; // CASE case_default
    end
  endcase
  if (EN) begin : g_outer_on
    if (0) begin : g_inner_off
      assign q[9] = a[9]; // CASE nested_inner_off
    end
    assign q[10] = a[10]; // CASE outer_on
  end else begin : g_outer_off
    if (1) begin : g_inner_looks_active
      assign q[11] = a[11]; // CASE outer_off
    end
  end
  if (0) begin : g_always_off
    if (1) begin : g_inner_looks_active
      assign q[12] = a[12]; // CASE nested_outer_off
    end
  end
  if (EN) assign q[13] = a[13]; else assign q[14] = a[14]; // CASE unnamed_arms
  always_comb begin
    if (1'b0) q[15] = a[15]; else q[15] = 1'b0; // CASE procedural_dead_arm_retained
  end
endmodule
module inactive_top(input logic [15:0] a, output logic [15:0] y0, y1, y2, y3);
  inactive_unit #(.EN(1), .N(2)) u_on0(.a(a), .q(y0));
  inactive_unit #(.EN(1), .N(2)) u_on1(.a(a), .q(y1));
  inactive_unit #(.EN(0), .N(0)) u_off0(.a(a), .q(y2));
  inactive_unit #(.EN(0), .N(0)) u_off1(.a(a), .q(y3));
endmodule
