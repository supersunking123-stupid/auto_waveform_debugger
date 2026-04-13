// Fixture for testing severity remapping (--compat vcs).
// Triggers IndexOOB diagnostic: arr[8] is out of bounds for logic [3:0].
// Without --compat vcs: IndexOOB is Error → blocks compile.
// With --compat vcs:    IndexOOB is Warning → compile succeeds.
module compat_severity_top (
    output logic [7:0] data
);
  logic [3:0] arr;
  assign data[3:0] = arr;
  assign data[7:4] = arr[8];
endmodule
