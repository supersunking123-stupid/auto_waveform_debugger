"""Expression evaluator — 4-state Verilog logic evaluation engine.

Evaluates AST nodes produced by expression_parser.py against concrete signal
values.  Supports all standard Verilog bitwise/logical/arithmetic/relational
operators plus custom binary ~& ~| ~^ and = in ternary.

The evaluation is event-driven: `iter_virtual_transitions` merges operand
transition timelines and only evaluates at change points.
"""

from __future__ import annotations

import heapq
from bisect import bisect_right
from typing import Any, Dict, Iterator, List, Optional, Sequence, Tuple

# ---------------------------------------------------------------------------
# LogicValue — 4-state Verilog value
# ---------------------------------------------------------------------------


class LogicValue:
    """Represents a Verilog 4-state value (0, 1, x, z).

    *bits* is MSB-first string of '0','1','x','z' characters.
    *width* is the declared bit width.
    *signed* indicates signed interpretation (affects >>> and comparisons).
    """

    __slots__ = ("_bits", "_width", "_signed")

    def __init__(self, bits: str, width: int, signed: bool = False) -> None:
        if len(bits) != width:
            # Zero-extend if shorter
            if len(bits) < width:
                bits = "0" * (width - len(bits)) + bits
            else:
                bits = bits[-width:]  # truncate MSBs
        self._bits = bits
        self._width = width
        self._signed = signed

    # ---- factories ----

    @staticmethod
    def from_string(v: str, signed: bool = False) -> "LogicValue":
        """Parse waveform backend value string."""
        v = v.strip()
        if v in ("0", "1", "x", "z"):
            return LogicValue(v, 1, signed)
        if v.startswith("b"):
            bits = v[1:]
            return LogicValue(bits, len(bits), signed)
        if v.startswith("h") or v.startswith("H"):
            bits = _hex_to_bits(v[1:])
            return LogicValue(bits, len(bits), signed)
        if v.startswith("d") or v.startswith("D"):
            n = int(v[1:])
            bits = bin(n)[2:]
            return LogicValue(bits, len(bits) if len(bits) > 1 else 1, signed)
        # Sized constant format: 4'b1010
        if "'" in v:
            return _parse_sized_constant(v, signed)
        # Fallback: try as integer
        try:
            n = int(v)
            bits = bin(n)[2:] if n >= 0 else bin(n & ((1 << 32) - 1))[2:]
            return LogicValue(bits, max(1, len(bits)), signed)
        except ValueError:
            return LogicValue("x", 1, signed)

    @staticmethod
    def from_int(n: int, width: int = 1, signed: bool = False) -> "LogicValue":
        """Create from Python integer."""
        if n < 0:
            n = n & ((1 << width) - 1)
        bits = bin(n)[2:].rjust(width, "0")
        return LogicValue(bits[-width:], width, signed)

    @staticmethod
    def x_val(width: int = 1) -> "LogicValue":
        return LogicValue("x" * width, width)

    @staticmethod
    def z_val(width: int = 1) -> "LogicValue":
        return LogicValue("z" * width, width)

    @staticmethod
    def zero(width: int = 1) -> "LogicValue":
        return LogicValue("0" * width, width)

    @staticmethod
    def one(width: int = 1) -> "LogicValue":
        return LogicValue("0" * (width - 1) + "1", width) if width > 1 else LogicValue("1", 1)

    # ---- accessors ----

    @property
    def width(self) -> int:
        return self._width

    @property
    def signed(self) -> bool:
        return self._signed

    @property
    def bits(self) -> str:
        return self._bits

    def is_xz(self) -> bool:
        return "x" in self._bits or "z" in self._bits

    def to_int(self) -> Optional[int]:
        """Return integer value, or None if contains x/z."""
        if self.is_xz():
            return None
        return int(self._bits, 2)

    def to_uint(self) -> int:
        """Return unsigned integer value (x/z treated as 0)."""
        clean = self._bits.replace("x", "0").replace("z", "0")
        return int(clean, 2) if clean else 0

    def to_string(self) -> str:
        """Convert back to waveform backend string format."""
        if self._width == 1:
            return self._bits
        return "b" + self._bits

    def is_true(self) -> bool:
        """Verilog truthiness: non-zero = true, x/z = ambiguous (returns False for safety)."""
        if self.is_xz():
            return False  # ambiguous
        return self.to_int() != 0

    def bit(self, idx: int) -> str:
        """Get bit at index (0 = LSB)."""
        if idx < 0 or idx >= self._width:
            return "x"
        return self._bits[self._width - 1 - idx]

    def with_width(self, new_width: int) -> "LogicValue":
        """Resize: zero-extend or truncate."""
        if new_width == self._width:
            return self
        if new_width < self._width:
            return LogicValue(self._bits[-new_width:], new_width, self._signed)
        pad = self._bits[0] if self._signed and self._bits else "0"
        return LogicValue(pad * (new_width - self._width) + self._bits, new_width, self._signed)

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, LogicValue):
            return NotImplemented
        return self._bits == other._bits and self._width == other._width

    def __repr__(self) -> str:  # pragma: no cover
        return f"LogicValue({self._bits!r}, w={self._width}, s={self._signed})"


# ---------------------------------------------------------------------------
# Bit conversion helpers
# ---------------------------------------------------------------------------

_HEX_MAP = {
    "0": "0000", "1": "0001", "2": "0010", "3": "0011",
    "4": "0100", "5": "0101", "6": "0110", "7": "0111",
    "8": "1000", "9": "1001", "a": "1010", "b": "1011",
    "c": "1100", "d": "1101", "e": "1110", "f": "1111",
    "x": "xxxx", "z": "zzzz",
}


def _hex_to_bits(hex_str: str) -> str:
    bits = []
    for ch in hex_str.lower():
        mapped = _HEX_MAP.get(ch)
        if mapped:
            bits.append(mapped)
        else:
            bits.append("xxxx")  # unknown
    return "".join(bits)


def _parse_sized_constant(v: str, signed: bool = False) -> LogicValue:
    """Parse '4'b1010' style constants."""
    idx = v.index("'")
    size = int(v[:idx])
    radix_ch = v[idx + 1].lower()
    digits = v[idx + 2:].replace("_", "").lower()

    if radix_ch == "b":
        bits = digits
    elif radix_ch == "h":
        bits = _hex_to_bits(digits)
    elif radix_ch == "o":
        _OCT = {"0": "000", "1": "001", "2": "010", "3": "011", "4": "100", "5": "101", "6": "110", "7": "111", "x": "xxx", "z": "zzz"}
        bits = "".join(_OCT.get(c, "xxx") for c in digits)
    elif radix_ch == "d":
        n = int(digits)
        bits = bin(n)[2:]
    else:
        return LogicValue.x_val(size)

    if len(bits) < size:
        bits = bits.rjust(size, "0")
    elif len(bits) > size:
        bits = bits[-size:]
    return LogicValue(bits, size, signed)


# ---------------------------------------------------------------------------
# 4-state bitwise truth tables
# ---------------------------------------------------------------------------

# Standard Verilog 4-state truth tables (IEEE 1364-2001)
_AND_TABLE = {
    ("0", "0"): "0", ("0", "1"): "0", ("0", "x"): "0", ("0", "z"): "0",
    ("1", "0"): "0", ("1", "1"): "1", ("1", "x"): "x", ("1", "z"): "x",
    ("x", "0"): "0", ("x", "1"): "x", ("x", "x"): "x", ("x", "z"): "x",
    ("z", "0"): "0", ("z", "1"): "x", ("z", "x"): "x", ("z", "z"): "x",
}

_OR_TABLE = {
    ("0", "0"): "0", ("0", "1"): "1", ("0", "x"): "x", ("0", "z"): "x",
    ("1", "0"): "1", ("1", "1"): "1", ("1", "x"): "1", ("1", "z"): "1",
    ("x", "0"): "x", ("x", "1"): "1", ("x", "x"): "x", ("x", "z"): "x",
    ("z", "0"): "x", ("z", "1"): "1", ("z", "x"): "x", ("z", "z"): "x",
}

_XOR_TABLE = {
    ("0", "0"): "0", ("0", "1"): "1", ("0", "x"): "x", ("0", "z"): "x",
    ("1", "0"): "1", ("1", "1"): "0", ("1", "x"): "x", ("1", "z"): "x",
    ("x", "0"): "x", ("x", "1"): "x", ("x", "x"): "x", ("x", "z"): "x",
    ("z", "0"): "x", ("z", "1"): "x", ("z", "x"): "x", ("z", "z"): "x",
}

_NOT_TABLE = {"0": "1", "1": "0", "x": "x", "z": "x"}


def _bit_op(a: str, b: str, table: dict) -> str:
    return table.get((a, b), "x")


# ---------------------------------------------------------------------------
# Operator evaluation functions
# ---------------------------------------------------------------------------

def _eval_not(a: LogicValue) -> LogicValue:
    """Bitwise NOT (~)."""
    bits = "".join(_NOT_TABLE[c] for c in a.bits)
    return LogicValue(bits, a.width, a.signed)


def _eval_logical_not(a: LogicValue) -> LogicValue:
    """Logical NOT (!). Returns 1-bit."""
    if a.is_xz():
        # If any bit is x/z and result is ambiguous
        has_01 = any(c in "01" for c in a.bits)
        only_xz = all(c in "xz" for c in a.bits)
        if only_xz:
            return LogicValue("x", 1)
        # Mixed: ambiguous
        return LogicValue("x", 1)
    return LogicValue("1" if a.to_int() == 0 else "0", 1)


def _eval_reduction(op: str, a: LogicValue) -> LogicValue:
    """Unary reduction: &, |, ^, ~&, ~|, ~^."""
    if a.is_xz():
        # Reduction on x/z bits
        bits_set = [c for c in a.bits]
        has_0 = "0" in bits_set
        has_1 = "1" in bits_set
        has_x = "x" in bits_set or "z" in bits_set
        if op in ("&", "~&"):
            if has_0:
                result = "0"
            elif has_x:
                result = "x"
            else:
                result = "1"
            if op == "~&":
                result = _NOT_TABLE[result]
        elif op in ("|", "~|"):
            if has_1:
                result = "1"
            elif has_x:
                result = "x"
            else:
                result = "0"
            if op == "~|":
                result = _NOT_TABLE[result]
        else:  # ^, ~^
            ones = bits_set.count("1")
            if has_x:
                result = "x"
            else:
                result = "1" if ones % 2 == 1 else "0"
            if op == "~^":
                result = _NOT_TABLE[result]
        return LogicValue(result, 1)

    n = a.to_int()
    if op == "&":
        return LogicValue("1" if n == (1 << a.width) - 1 else "0", 1)
    elif op == "|":
        return LogicValue("1" if n != 0 else "0", 1)
    elif op == "^":
        result = bin(n).count("1") % 2
        return LogicValue(str(result), 1)
    elif op == "~&":
        return LogicValue("0" if n == (1 << a.width) - 1 else "1", 1)
    elif op == "~|":
        return LogicValue("0" if n != 0 else "1", 1)
    elif op == "~^":
        result = bin(n).count("1") % 2
        return LogicValue(str(1 - result), 1)
    return LogicValue("x", 1)


def _eval_bitwise(op: str, a: LogicValue, b: LogicValue) -> LogicValue:
    """Binary bitwise: &, |, ^, ~&, ~|, ~^."""
    w = max(a.width, b.width)
    a_ext = a.with_width(w)
    b_ext = b.with_width(w)

    if op == "&":
        table = _AND_TABLE
    elif op == "|":
        table = _OR_TABLE
    elif op == "^":
        table = _XOR_TABLE
    elif op == "~&":
        bits = "".join(_NOT_TABLE[_bit_op(ac, bc, _AND_TABLE)] for ac, bc in zip(a_ext.bits, b_ext.bits))
        return LogicValue(bits, w)
    elif op == "~|":
        bits = "".join(_NOT_TABLE[_bit_op(ac, bc, _OR_TABLE)] for ac, bc in zip(a_ext.bits, b_ext.bits))
        return LogicValue(bits, w)
    elif op == "~^":
        bits = "".join(_NOT_TABLE[_bit_op(ac, bc, _XOR_TABLE)] for ac, bc in zip(a_ext.bits, b_ext.bits))
        return LogicValue(bits, w)
    else:
        return LogicValue.x_val(w)

    bits = "".join(_bit_op(ac, bc, table) for ac, bc in zip(a_ext.bits, b_ext.bits))
    return LogicValue(bits, w)


def _eval_logical(op: str, a: LogicValue, b: LogicValue) -> LogicValue:
    """Logical: &&, ||. Returns 1-bit."""
    a_true = a.is_true() if not a.is_xz() else None
    b_true = b.is_true() if not b.is_xz() else None

    if op == "&&":
        if a_true is False or b_true is False:
            return LogicValue("0", 1)
        if a_true is None or b_true is None:
            return LogicValue("x", 1)
        return LogicValue("1" if (a_true and b_true) else "0", 1)
    elif op == "||":
        if a_true is True or b_true is True:
            return LogicValue("1", 1)
        if a_true is None or b_true is None:
            return LogicValue("x", 1)
        return LogicValue("0", 1)
    return LogicValue("x", 1)


def _eval_comparison(op: str, a: LogicValue, b: LogicValue) -> LogicValue:
    """Comparison: ==, !=, ===, !==, <, >, <=, >=. Returns 1-bit."""
    # === and !== are 4-state identity operators (x === x is true)
    if op == "===":
        return LogicValue("1" if a.bits == b.bits and a.width == b.width else "0", 1)
    if op == "!==":
        return LogicValue("1" if not (a.bits == b.bits and a.width == b.width) else "0", 1)

    if a.is_xz() or b.is_xz():
        return LogicValue("x", 1)

    # Sign-extend if needed for comparison
    w = max(a.width, b.width)
    if a.signed or b.signed:
        a_val = a.to_int() if not a.signed else _to_signed(a.to_uint(), a.width)
        b_val = b.to_int() if not b.signed else _to_signed(b.to_uint(), b.width)
    else:
        a_val = a.to_uint()
        b_val = b.to_uint()

    if op == "==":
        return LogicValue("1" if a_val == b_val else "0", 1)
    elif op == "!=":
        return LogicValue("1" if a_val != b_val else "0", 1)
    elif op == "<":
        return LogicValue("1" if a_val < b_val else "0", 1)
    elif op == ">":
        return LogicValue("1" if a_val > b_val else "0", 1)
    elif op == "<=":
        return LogicValue("1" if a_val <= b_val else "0", 1)
    elif op == ">=":
        return LogicValue("1" if a_val >= b_val else "0", 1)
    return LogicValue("x", 1)


def _to_signed(val: int, width: int) -> int:
    """Convert unsigned value to signed interpretation."""
    mask = 1 << (width - 1)
    if val & mask:
        return val - (1 << width)
    return val


def _eval_shift(op: str, a: LogicValue, b: LogicValue) -> LogicValue:
    """Shift: <<, >>, <<<, >>>. Width = left operand width."""
    if a.is_xz() or b.is_xz():
        return LogicValue.x_val(a.width)

    w = a.width
    shift_amt = b.to_uint()

    if shift_amt >= w:
        return LogicValue.zero(w)

    if op == "<<":
        return LogicValue.from_int(a.to_uint() << shift_amt, w)
    elif op == ">>":
        return LogicValue.from_int(a.to_uint() >> shift_amt, w)
    elif op == "<<<":
        # Arithmetic left shift = logical left shift
        return LogicValue.from_int(a.to_uint() << shift_amt, w)
    elif op == ">>>":
        val = a.to_uint()
        if a.signed and (val & (1 << (w - 1))):
            # Sign-extend
            result = val >> shift_amt
            sign_bits = ((1 << shift_amt) - 1) << (w - shift_amt)
            result |= sign_bits
            return LogicValue.from_int(result & ((1 << w) - 1), w, signed=True)
        return LogicValue.from_int(val >> shift_amt, w)
    return LogicValue.x_val(w)


def _eval_arithmetic(op: str, a: LogicValue, b: LogicValue) -> LogicValue:
    """Arithmetic: +, -, *, /, %, **."""
    w = max(a.width, b.width)
    if a.is_xz() or b.is_xz():
        return LogicValue.x_val(w)

    a_val = a.to_uint()
    b_val = b.to_uint()
    mask = (1 << w) - 1

    if op == "+":
        return LogicValue.from_int((a_val + b_val) & mask, w)
    elif op == "-":
        return LogicValue.from_int((a_val - b_val) & mask, w)
    elif op == "*":
        return LogicValue.from_int((a_val * b_val) & mask, w)
    elif op == "/":
        if b_val == 0:
            return LogicValue.x_val(w)
        return LogicValue.from_int((a_val // b_val) & mask, w)
    elif op == "%":
        if b_val == 0:
            return LogicValue.x_val(w)
        return LogicValue.from_int((a_val % b_val) & mask, w)
    elif op == "**":
        try:
            result = pow(a_val, b_val)
            return LogicValue.from_int(result & mask, w)
        except (ValueError, OverflowError):
            return LogicValue.x_val(w)
    return LogicValue.x_val(w)


def _eval_ternary(cond: LogicValue, true_val: LogicValue, false_val: LogicValue) -> LogicValue:
    """Ternary: cond ? true_val : false_val.

    IEEE 1364 X-merge: if cond is x/z, compute both branches and bit-merge.
    """
    if cond.is_xz():
        # X-merge: for each bit, if both branches agree use that value, else x
        w = max(true_val.width, false_val.width)
        t = true_val.with_width(w)
        f = false_val.with_width(w)
        bits = "".join(
            tc if tc == fc else "x"
            for tc, fc in zip(t.bits, f.bits)
        )
        return LogicValue(bits, w)

    if cond.is_true():
        return true_val
    return false_val


# ---------------------------------------------------------------------------
# AST evaluator — single time point
# ---------------------------------------------------------------------------

def evaluate_expression(
    ast: Dict[str, Any],
    operand_values: Dict[str, LogicValue],
) -> LogicValue:
    """Evaluate AST for a single time point.

    *operand_values* maps signal paths to their current LogicValue.
    """
    node_type = ast["type"]

    if node_type == "Literal":
        val = ast.get("value", "0")
        width = ast.get("width", 1)
        signed = ast.get("signed", False)
        if "'" in val:
            return _parse_sized_constant(val, signed)
        # Plain bits or single char
        if val in ("0", "1", "x", "z"):
            return LogicValue(val, 1, signed)
        # Bit string
        return LogicValue(val, width if width > 1 else max(1, len(val)), signed)

    if node_type == "SignalRef":
        path = ast["path"]
        if path not in operand_values:
            return LogicValue.x_val(1)
        return operand_values[path]

    if node_type == "UnaryOp":
        op = ast["op"]
        operand = evaluate_expression(ast["operand"], operand_values)
        if op == "~":
            return _eval_not(operand)
        if op == "!":
            return _eval_logical_not(operand)
        if op == "unary-":
            w = operand.width
            return LogicValue.from_int((-operand.to_uint()) & ((1 << w) - 1), w)
        # Reduction operators
        if op in ("&", "|", "^", "~&", "~|", "~^"):
            return _eval_reduction(op, operand)
        return LogicValue.x_val(operand.width)

    if node_type == "BinaryOp":
        op = ast["op"]
        left = evaluate_expression(ast["left"], operand_values)
        right = evaluate_expression(ast["right"], operand_values)
        if op in ("&", "|", "^", "~&", "~|", "~^"):
            return _eval_bitwise(op, left, right)
        if op in ("&&", "||"):
            return _eval_logical(op, left, right)
        if op in ("==", "!=", "===", "!==", "<", ">", "<=", ">="):
            return _eval_comparison(op, left, right)
        if op in ("<<", ">>", "<<<", ">>>"):
            return _eval_shift(op, left, right)
        if op in ("+", "-", "*", "/", "%", "**"):
            return _eval_arithmetic(op, left, right)
        return LogicValue.x_val(max(left.width, right.width))

    if node_type == "TernaryOp":
        cond = evaluate_expression(ast["cond"], operand_values)
        true_val = evaluate_expression(ast["true_val"], operand_values)
        false_val = evaluate_expression(ast["false_val"], operand_values)
        return _eval_ternary(cond, true_val, false_val)

    return LogicValue.x_val(1)


# ---------------------------------------------------------------------------
# Event-driven transition computation (lazy iterator)
# ---------------------------------------------------------------------------

def iter_virtual_transitions(
    ast: Dict[str, Any],
    operand_transitions: Dict[str, List[Dict[str, Any]]],
    start_time: int,
    end_time: int,
    signal_widths: Optional[Dict[str, int]] = None,
) -> Iterator[Dict[str, Any]]:
    """Lazy generator yielding virtual signal transitions.

    *operand_transitions* maps signal path -> sorted list of {"t": int, "v": str}.
    Transitions are assumed sorted by time.

    The iterator:
    1. Seeds with value at start_time (last operand transition before start)
    2. Merges all operand event times
    3. Batches same-timestamp events
    4. Evaluates at each batch, suppresses no-change
    """
    signal_widths = signal_widths or {}

    # Build per-signal (time, value) pairs for binary-search lookup
    sig_times: Dict[str, List[int]] = {}
    sig_vals: Dict[str, List[str]] = {}
    for path, trans in operand_transitions.items():
        times = [t["t"] for t in trans]
        vals = [t["v"] for t in trans]
        sig_times[path] = times
        sig_vals[path] = vals

    def value_at(path: str, t: int) -> LogicValue:
        times = sig_times.get(path, [])
        vals = sig_vals.get(path, [])
        if not times:
            w = signal_widths.get(path, 1)
            return LogicValue.x_val(w)
        idx = bisect_right(times, t) - 1
        if idx < 0:
            w = signal_widths.get(path, 1)
            return LogicValue.x_val(w)
        return LogicValue.from_string(vals[idx])

    # Collect all event times in [start_time, end_time] using heap-based k-way merge
    # This avoids materializing all event times upfront
    import heapq
    event_heap: List[Tuple[int, int, int]] = []  # (time, signal_idx, trans_idx)
    for sig_idx, (path, trans) in enumerate(operand_transitions.items()):
        for tr_idx, tr in enumerate(trans):
            t = tr["t"]
            if start_time <= t <= end_time:
                heapq.heappush(event_heap, (t, sig_idx, tr_idx))

    # Seed: compute value at start_time
    seed_values = {path: value_at(path, start_time) for path in sig_times}
    prev_result = evaluate_expression(ast, seed_values)
    prev_str = prev_result.to_string()
    yield {"t": start_time, "v": prev_str}

    # Iterate through events lazily via heap, batching same-timestamp changes
    while event_heap:
        current_t = event_heap[0][0]
        # Drain all events at current_t from heap
        while event_heap and event_heap[0][0] == current_t:
            heapq.heappop(event_heap)

        # Evaluate with updated values
        current_values = {path: value_at(path, current_t) for path in sig_times}
        result = evaluate_expression(ast, current_values)
        result_str = result.to_string()

        if result_str != prev_str:
            yield {"t": current_t, "v": result_str}
            prev_str = result_str
