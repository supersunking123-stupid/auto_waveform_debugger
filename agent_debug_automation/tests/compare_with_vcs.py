#!/usr/bin/env python3
"""compare_with_vcs.py — Compare evaluator output against VCS-generated VCD.

Usage:
    .venv/bin/python3 agent_debug_automation/tests/compare_with_vcs.py --vcd /path/to/wave.vcd

Reads the VCD file, extracts input signal transitions, evaluates expressions
via the Python evaluator, and compares against VCS-computed output wires.
"""

import argparse
import re
import sys
from typing import Dict, List, Tuple

# Add project root to path
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))

from agent_debug_automation.expression_parser import parse_expression
from agent_debug_automation.expression_evaluator import LogicValue, iter_virtual_transitions


def parse_vcd(path: str) -> Tuple[Dict[str, str], Dict[str, List[Dict]]]:
    """Parse a VCD file. Returns (signal_map, transitions).

    signal_map: {signal_name: var_symbol}
    transitions: {signal_name: [{"t": time, "v": value}, ...]}
    """
    signal_map: Dict[str, str] = {}  # symbol -> signal_name
    symbol_to_name: Dict[str, str] = {}
    transitions: Dict[str, List[Dict]] = {}
    current_time = 0
    scope_stack: List[str] = []

    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue

            # Time
            if line.startswith("#"):
                current_time = int(line[1:])
                continue

            # Scope
            if line.startswith("$scope"):
                parts = line.split()
                if len(parts) >= 3:
                    scope_stack.append(parts[2])
                continue
            if line.startswith("$upscope"):
                if scope_stack:
                    scope_stack.pop()
                continue

            # Variable definition
            if line.startswith("$var"):
                parts = line.split()
                if len(parts) >= 5:
                    var_type = parts[1]
                    size = int(parts[2])
                    symbol = parts[3]
                    name = parts[4]
                    full_path = ".".join(scope_stack + [name])
                    symbol_to_name[symbol] = full_path
                    transitions[full_path] = []
                continue

            # Value change (scalar): 0!, 1", etc.
            m = re.match(r'^([01xzXZ])(\S+)$', line)
            if m:
                val = m.group(1).lower()
                sym = m.group(2)
                name = symbol_to_name.get(sym)
                if name is not None:
                    transitions[name].append({"t": current_time, "v": val})
                continue

            # Value change (vector): b1010 "
            m = re.match(r'^b([01xzXZ_]+)\s+(\S+)$', line)
            if m:
                val = m.group(1).replace("_", "").lower()
                sym = m.group(2)
                name = symbol_to_name.get(sym)
                if name is not None:
                    transitions[name].append({"t": current_time, "v": "b" + val})
                continue

    return symbol_to_name, transitions


def _get_value_at(transitions: List[Dict], t: int) -> str:
    """Get value at time t from sorted transitions."""
    last = "x"
    for tr in transitions:
        if tr["t"] <= t:
            last = tr["v"]
        else:
            break
    return last


# Expression definitions: (output_wire_name, expression, input_signals)
EXPRESSIONS = [
    ("tb_top.and_ab", "a & b", ["tb_top.a", "tb_top.b"]),
    ("tb_top.or_ab", "a | b", ["tb_top.a", "tb_top.b"]),
    ("tb_top.xor_ab", "a ^ b", ["tb_top.a", "tb_top.b"]),
    ("tb_top.not_a", "~a", ["tb_top.a"]),
    ("tb_top.land_ab", "a && b", ["tb_top.a", "tb_top.b"]),
    ("tb_top.lor_ab", "a || b", ["tb_top.a", "tb_top.b"]),
    ("tb_top.lnot_a", "!a", ["tb_top.a"]),
    ("tb_top.eq_ab", "a == b", ["tb_top.a", "tb_top.b"]),
    ("tb_top.ne_ab", "a != b", ["tb_top.a", "tb_top.b"]),
    ("tb_top.bus_and", "bus_a & bus_b", ["tb_top.bus_a", "tb_top.bus_b"]),
    ("tb_top.bus_or", "bus_a | bus_b", ["tb_top.bus_a", "tb_top.bus_b"]),
    ("tb_top.bus_xor", "bus_a ^ bus_b", ["tb_top.bus_a", "tb_top.bus_b"]),
    ("tb_top.complex", "(^bus_a) & a | (bus_b == 0)", ["tb_top.bus_a", "tb_top.bus_b", "tb_top.a"]),
]


def compare(vcd_path: str) -> bool:
    """Compare evaluator output against VCS VCD. Returns True if all match."""
    symbol_to_name, vcd_transitions = parse_vcd(vcd_path)

    all_pass = True
    for output_wire, expression, input_signals in EXPRESSIONS:
        # Get VCS reference transitions
        ref_trans = vcd_transitions.get(output_wire, [])
        if not ref_trans:
            print(f"SKIP {output_wire}: no reference data in VCD")
            continue

        # Map input signals to VCD transitions
        operand_transitions = {}
        for sig in input_signals:
            if sig in vcd_transitions:
                operand_transitions[sig] = vcd_transitions[sig]
            else:
                print(f"WARN {output_wire}: input signal {sig} not found in VCD")
                operand_transitions[sig] = []

        # Compute time range
        all_times = [tr["t"] for tr in ref_trans]
        for trans_list in operand_transitions.values():
            all_times.extend(tr["t"] for tr in trans_list)
        if not all_times:
            continue
        start_time = min(all_times)
        end_time = max(all_times) + 1

        # Evaluate
        ast = parse_expression(expression)
        computed = list(iter_virtual_transitions(ast, operand_transitions, start_time, end_time))

        # Compare at each reference time point
        mismatches = 0
        for ref in ref_trans:
            t = ref["t"]
            expected = ref["v"]
            # Find computed value at this time
            computed_val = "x"
            for ct in computed:
                if ct["t"] <= t:
                    computed_val = ct["v"]
                else:
                    break

            # Normalize for comparison
            exp_lv = LogicValue.from_string(expected)
            comp_lv = LogicValue.from_string(computed_val)

            if exp_lv != comp_lv:
                mismatches += 1
                if mismatches <= 3:
                    print(f"  MISMATCH at t={t}: expected={expected} got={computed_val}")

        if mismatches > 0:
            print(f"FAIL {output_wire} ({expression}): {mismatches} mismatches")
            all_pass = False
        else:
            print(f"PASS {output_wire} ({expression})")

    return all_pass


def main():
    parser = argparse.ArgumentParser(description="Compare evaluator against VCS VCD")
    parser.add_argument("--vcd", required=True, help="Path to VCS-generated VCD file")
    args = parser.parse_args()

    success = compare(args.vcd)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
