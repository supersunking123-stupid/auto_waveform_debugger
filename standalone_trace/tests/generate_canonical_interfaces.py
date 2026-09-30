#!/usr/bin/env python3
"""Generate an interface-heavy benchmark outside the regression fixture directory.

Example (from repository root):
  .venv/bin/python3 standalone_trace/tests/generate_canonical_interfaces.py /tmp/interfaces.sv
The default generates 4096 identical modules connected to distinct scalar and
array interfaces. Output is deterministic and contains no absolute file paths.
"""

import argparse
from pathlib import Path


def generate(count):
    return '''// top: canonical_interfaces_top
// Generated interface benchmark; keep large outputs outside routine fixtures.
interface bench_if;
  logic [7:0] data;
  logic valid;
  modport source(output data, valid);
endinterface
module bench_leaf(bench_if.source scalar, bench_if.source lanes[1:0],
                  input logic [7:0] d, output logic [7:0] q);
  assign scalar.data = d;
  assign scalar.valid = |d;
  assign lanes[0].data = ~d;
  assign lanes[1].data = d ^ 8'h5a;
  assign lanes[0].valid = &d;
  assign lanes[1].valid = ^d;
  assign q = d + 8'h01;
endmodule
module canonical_interfaces_top(input logic [7:0] d, output logic [7:0] q[0:COUNT_MINUS_ONE]);
  for (genvar n = 0; n < COUNT; n++) begin : g
    bench_if scalar();
    bench_if lanes[1:0]();
    bench_leaf leaf(scalar, lanes, d ^ 8'(n), q[n]);
  end
endmodule
'''.replace('COUNT_MINUS_ONE', str(count - 1)).replace('COUNT', str(count))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--count', type=int, default=4096)
    args = parser.parse_args()
    if args.count < 2:
        parser.error('--count must be at least 2 to exercise canonical sharing')
    fixture_dir = Path(__file__).resolve().parent / 'fixtures'
    if args.output.resolve().is_relative_to(fixture_dir):
        parser.error('write benchmark designs outside the routine fixtures directory')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generate(args.count))


if __name__ == '__main__':
    main()
