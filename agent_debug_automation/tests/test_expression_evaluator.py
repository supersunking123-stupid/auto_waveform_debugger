"""Unit tests for expression_evaluator.py — truth tables, multi-bit, X/Z, event-driven."""

import unittest

from agent_debug_automation.expression_parser import parse_expression
from agent_debug_automation.expression_evaluator import (
    LogicValue,
    evaluate_expression,
    iter_virtual_transitions,
    _eval_not,
    _eval_bitwise,
    _eval_logical,
    _eval_comparison,
    _eval_shift,
    _eval_arithmetic,
    _eval_reduction,
    _eval_ternary,
)


class TestLogicValue(unittest.TestCase):
    """LogicValue construction and conversion tests."""

    def test_scalar_0(self):
        v = LogicValue.from_string("0")
        self.assertEqual(v.bits, "0")
        self.assertEqual(v.width, 1)
        self.assertEqual(v.to_int(), 0)

    def test_scalar_1(self):
        v = LogicValue.from_string("1")
        self.assertEqual(v.bits, "1")
        self.assertEqual(v.to_int(), 1)

    def test_scalar_x(self):
        v = LogicValue.from_string("x")
        self.assertTrue(v.is_xz())
        self.assertIsNone(v.to_int())

    def test_scalar_z(self):
        v = LogicValue.from_string("z")
        self.assertTrue(v.is_xz())

    def test_binary_string(self):
        v = LogicValue.from_string("b1010")
        self.assertEqual(v.width, 4)
        self.assertEqual(v.to_int(), 10)

    def test_hex_string(self):
        v = LogicValue.from_string("hFF")
        self.assertEqual(v.width, 8)
        self.assertEqual(v.to_int(), 255)

    def test_decimal_string(self):
        v = LogicValue.from_string("d255")
        self.assertEqual(v.to_int(), 255)

    def test_to_string_scalar(self):
        self.assertEqual(LogicValue.from_string("1").to_string(), "1")

    def test_to_string_multibit(self):
        self.assertEqual(LogicValue.from_string("b1010").to_string(), "b1010")

    def test_is_true(self):
        self.assertTrue(LogicValue.from_string("1").is_true())
        self.assertFalse(LogicValue.from_string("0").is_true())
        self.assertTrue(LogicValue.from_string("b1010").is_true())  # 10 is non-zero
        self.assertFalse(LogicValue.from_string("b0000").is_true())
        self.assertFalse(LogicValue.from_string("x").is_true())

    def test_with_width_extend(self):
        v = LogicValue.from_string("b1010")
        ext = v.with_width(8)
        self.assertEqual(ext.width, 8)
        self.assertEqual(ext.bits, "00001010")

    def test_with_width_truncate(self):
        v = LogicValue.from_string("b11111010")
        trunc = v.with_width(4)
        self.assertEqual(trunc.bits, "1010")

    def test_from_int(self):
        v = LogicValue.from_int(10, 4)
        self.assertEqual(v.bits, "1010")
        self.assertEqual(v.to_int(), 10)


class TestBitwiseOps(unittest.TestCase):
    """Bitwise operator truth tables."""

    def _lv(self, val: str) -> LogicValue:
        return LogicValue.from_string(val)

    def test_and_0_0(self):
        r = _eval_bitwise("&", self._lv("0"), self._lv("0"))
        self.assertEqual(r.bits, "0")

    def test_and_0_1(self):
        r = _eval_bitwise("&", self._lv("0"), self._lv("1"))
        self.assertEqual(r.bits, "0")

    def test_and_1_1(self):
        r = _eval_bitwise("&", self._lv("1"), self._lv("1"))
        self.assertEqual(r.bits, "1")

    def test_and_x_0(self):
        r = _eval_bitwise("&", self._lv("x"), self._lv("0"))
        self.assertEqual(r.bits, "0")

    def test_and_x_1(self):
        r = _eval_bitwise("&", self._lv("x"), self._lv("1"))
        self.assertEqual(r.bits, "x")

    def test_or_0_0(self):
        r = _eval_bitwise("|", self._lv("0"), self._lv("0"))
        self.assertEqual(r.bits, "0")

    def test_or_0_1(self):
        r = _eval_bitwise("|", self._lv("0"), self._lv("1"))
        self.assertEqual(r.bits, "1")

    def test_or_1_1(self):
        r = _eval_bitwise("|", self._lv("1"), self._lv("1"))
        self.assertEqual(r.bits, "1")

    def test_or_x_1(self):
        r = _eval_bitwise("|", self._lv("x"), self._lv("1"))
        self.assertEqual(r.bits, "1")

    def test_or_x_0(self):
        r = _eval_bitwise("|", self._lv("x"), self._lv("0"))
        self.assertEqual(r.bits, "x")

    def test_xor_0_0(self):
        r = _eval_bitwise("^", self._lv("0"), self._lv("0"))
        self.assertEqual(r.bits, "0")

    def test_xor_0_1(self):
        r = _eval_bitwise("^", self._lv("0"), self._lv("1"))
        self.assertEqual(r.bits, "1")

    def test_xor_1_1(self):
        r = _eval_bitwise("^", self._lv("1"), self._lv("1"))
        self.assertEqual(r.bits, "0")

    def test_nand_1_1(self):
        r = _eval_bitwise("~&", self._lv("1"), self._lv("1"))
        self.assertEqual(r.bits, "0")

    def test_nand_0_0(self):
        r = _eval_bitwise("~&", self._lv("0"), self._lv("0"))
        self.assertEqual(r.bits, "1")

    def test_nor_0_0(self):
        r = _eval_bitwise("~|", self._lv("0"), self._lv("0"))
        self.assertEqual(r.bits, "1")

    def test_nor_1_0(self):
        r = _eval_bitwise("~|", self._lv("1"), self._lv("0"))
        self.assertEqual(r.bits, "0")

    def test_xnor_1_1(self):
        r = _eval_bitwise("~^", self._lv("1"), self._lv("1"))
        self.assertEqual(r.bits, "1")

    def test_xnor_1_0(self):
        r = _eval_bitwise("~^", self._lv("1"), self._lv("0"))
        self.assertEqual(r.bits, "0")

    def test_multibit_and(self):
        r = _eval_bitwise("&", self._lv("b1010"), self._lv("b1100"))
        self.assertEqual(r.bits, "1000")

    def test_multibit_or(self):
        r = _eval_bitwise("|", self._lv("b1010"), self._lv("b1100"))
        self.assertEqual(r.bits, "1110")

    def test_multibit_xor(self):
        r = _eval_bitwise("^", self._lv("b1010"), self._lv("b1100"))
        self.assertEqual(r.bits, "0110")

    def test_width_mismatch(self):
        r = _eval_bitwise("&", self._lv("b1010"), self._lv("1"))
        self.assertEqual(r.width, 4)


class TestNot(unittest.TestCase):

    def test_not_0(self):
        r = _eval_not(LogicValue.from_string("0"))
        self.assertEqual(r.bits, "1")

    def test_not_1(self):
        r = _eval_not(LogicValue.from_string("1"))
        self.assertEqual(r.bits, "0")

    def test_not_x(self):
        r = _eval_not(LogicValue.from_string("x"))
        self.assertEqual(r.bits, "x")

    def test_not_multibit(self):
        r = _eval_not(LogicValue.from_string("b1010"))
        self.assertEqual(r.bits, "0101")


class TestReduction(unittest.TestCase):

    def test_and_all_ones(self):
        r = _eval_reduction("&", LogicValue.from_string("b1111"))
        self.assertEqual(r.bits, "1")

    def test_and_has_zero(self):
        r = _eval_reduction("&", LogicValue.from_string("b1110"))
        self.assertEqual(r.bits, "0")

    def test_or_all_zeros(self):
        r = _eval_reduction("|", LogicValue.from_string("b0000"))
        self.assertEqual(r.bits, "0")

    def test_or_has_one(self):
        r = _eval_reduction("|", LogicValue.from_string("b0010"))
        self.assertEqual(r.bits, "1")

    def test_xor_even_ones(self):
        r = _eval_reduction("^", LogicValue.from_string("b1010"))
        self.assertEqual(r.bits, "0")

    def test_xor_odd_ones(self):
        r = _eval_reduction("^", LogicValue.from_string("b1011"))
        self.assertEqual(r.bits, "1")

    def test_nand_all_ones(self):
        r = _eval_reduction("~&", LogicValue.from_string("b1111"))
        self.assertEqual(r.bits, "0")

    def test_nor_all_zeros(self):
        r = _eval_reduction("~|", LogicValue.from_string("b0000"))
        self.assertEqual(r.bits, "1")

    def test_xnor_even_ones(self):
        r = _eval_reduction("~^", LogicValue.from_string("b1010"))
        self.assertEqual(r.bits, "1")

    def test_xnor_odd_ones(self):
        r = _eval_reduction("~^", LogicValue.from_string("b1011"))
        self.assertEqual(r.bits, "0")


class TestLogicalOps(unittest.TestCase):

    def test_logical_and_true_true(self):
        r = _eval_logical("&&", LogicValue.from_string("1"), LogicValue.from_string("1"))
        self.assertEqual(r.bits, "1")

    def test_logical_and_true_false(self):
        r = _eval_logical("&&", LogicValue.from_string("1"), LogicValue.from_string("0"))
        self.assertEqual(r.bits, "0")

    def test_logical_or_false_false(self):
        r = _eval_logical("||", LogicValue.from_string("0"), LogicValue.from_string("0"))
        self.assertEqual(r.bits, "0")

    def test_logical_or_true_false(self):
        r = _eval_logical("||", LogicValue.from_string("1"), LogicValue.from_string("0"))
        self.assertEqual(r.bits, "1")

    def test_logical_and_x_true(self):
        r = _eval_logical("&&", LogicValue.from_string("x"), LogicValue.from_string("1"))
        self.assertEqual(r.bits, "x")

    def test_logical_or_x_true(self):
        r = _eval_logical("||", LogicValue.from_string("x"), LogicValue.from_string("1"))
        self.assertEqual(r.bits, "1")

    def test_logical_not_0(self):
        r = evaluate_expression(parse_expression("!a"), {"a": LogicValue.from_string("0")})
        self.assertEqual(r.bits, "1")

    def test_logical_not_1(self):
        r = evaluate_expression(parse_expression("!a"), {"a": LogicValue.from_string("1")})
        self.assertEqual(r.bits, "0")

    def test_vector_truthiness(self):
        self.assertTrue(LogicValue.from_string("b0001").is_true())
        self.assertFalse(LogicValue.from_string("b0000").is_true())

    def test_logical_not_vector_zero(self):
        r = evaluate_expression(parse_expression("!a"), {"a": LogicValue.from_string("b0000")})
        self.assertEqual(r.bits, "1")

    def test_logical_not_vector_nonzero(self):
        r = evaluate_expression(parse_expression("!a"), {"a": LogicValue.from_string("b0001")})
        self.assertEqual(r.bits, "0")


class TestComparison(unittest.TestCase):

    def test_eq_equal(self):
        r = _eval_comparison("==", LogicValue.from_int(5, 4), LogicValue.from_int(5, 4))
        self.assertEqual(r.bits, "1")

    def test_eq_not_equal(self):
        r = _eval_comparison("==", LogicValue.from_int(5, 4), LogicValue.from_int(3, 4))
        self.assertEqual(r.bits, "0")

    def test_ne(self):
        r = _eval_comparison("!=", LogicValue.from_int(5, 4), LogicValue.from_int(3, 4))
        self.assertEqual(r.bits, "1")

    def test_lt(self):
        r = _eval_comparison("<", LogicValue.from_int(3, 4), LogicValue.from_int(5, 4))
        self.assertEqual(r.bits, "1")

    def test_gt(self):
        r = _eval_comparison(">", LogicValue.from_int(5, 4), LogicValue.from_int(3, 4))
        self.assertEqual(r.bits, "1")

    def test_le(self):
        r = _eval_comparison("<=", LogicValue.from_int(5, 4), LogicValue.from_int(5, 4))
        self.assertEqual(r.bits, "1")

    def test_ge(self):
        r = _eval_comparison(">=", LogicValue.from_int(5, 4), LogicValue.from_int(3, 4))
        self.assertEqual(r.bits, "1")

    def test_comparison_with_x(self):
        r = _eval_comparison("==", LogicValue.from_string("x"), LogicValue.from_int(1, 1))
        self.assertEqual(r.bits, "x")


class TestShift(unittest.TestCase):

    def test_shift_left(self):
        r = _eval_shift("<<", LogicValue.from_int(1, 4), LogicValue.from_int(2, 4))
        self.assertEqual(r.bits, "0100")

    def test_shift_right(self):
        r = _eval_shift(">>", LogicValue.from_int(8, 4), LogicValue.from_int(2, 4))
        self.assertEqual(r.bits, "0010")

    def test_arith_shift_right_unsigned(self):
        r = _eval_shift(">>>", LogicValue.from_int(15, 4), LogicValue.from_int(1, 4))
        self.assertEqual(r.bits, "0111")

    def test_arith_shift_right_signed(self):
        """Signed 4'b1111 (-1) >>> 1 = 4'b1111 (sign-extended)."""
        a = LogicValue("1111", 4, signed=True)
        r = _eval_shift(">>>", a, LogicValue.from_int(1, 4))
        self.assertEqual(r.bits, "1111")

    def test_shift_overflow(self):
        r = _eval_shift("<<", LogicValue.from_int(1, 4), LogicValue.from_int(4, 4))
        self.assertEqual(r.bits, "0000")


class TestArithmetic(unittest.TestCase):

    def test_add(self):
        r = _eval_arithmetic("+", LogicValue.from_int(3, 4), LogicValue.from_int(5, 4))
        self.assertEqual(r.to_int(), 8)

    def test_sub(self):
        r = _eval_arithmetic("-", LogicValue.from_int(5, 4), LogicValue.from_int(3, 4))
        self.assertEqual(r.to_int(), 2)

    def test_mul(self):
        r = _eval_arithmetic("*", LogicValue.from_int(3, 4), LogicValue.from_int(4, 4))
        self.assertEqual(r.to_int(), 12)

    def test_div(self):
        r = _eval_arithmetic("/", LogicValue.from_int(10, 8), LogicValue.from_int(3, 8))
        self.assertEqual(r.to_int(), 3)

    def test_mod(self):
        r = _eval_arithmetic("%", LogicValue.from_int(10, 8), LogicValue.from_int(3, 8))
        self.assertEqual(r.to_int(), 1)

    def test_power(self):
        r = _eval_arithmetic("**", LogicValue.from_int(2, 8), LogicValue.from_int(3, 8))
        self.assertEqual(r.to_int(), 8)

    def test_div_by_zero(self):
        r = _eval_arithmetic("/", LogicValue.from_int(10, 8), LogicValue.from_int(0, 8))
        self.assertTrue(r.is_xz())

    def test_overflow_wrap(self):
        r = _eval_arithmetic("+", LogicValue.from_int(15, 4), LogicValue.from_int(1, 4))
        self.assertEqual(r.to_int(), 0)  # 15+1=16 -> wraps to 0 in 4 bits


class TestTernary(unittest.TestCase):

    def test_true_branch(self):
        r = _eval_ternary(LogicValue.from_string("1"), LogicValue.from_int(5, 4), LogicValue.from_int(3, 4))
        self.assertEqual(r.to_int(), 5)

    def test_false_branch(self):
        r = _eval_ternary(LogicValue.from_string("0"), LogicValue.from_int(5, 4), LogicValue.from_int(3, 4))
        self.assertEqual(r.to_int(), 3)

    def test_x_merge(self):
        r = _eval_ternary(LogicValue.from_string("x"), LogicValue.from_string("b1010"), LogicValue.from_string("b0101"))
        self.assertEqual(r.bits, "xxxx")

    def test_x_merge_agree(self):
        r = _eval_ternary(LogicValue.from_string("x"), LogicValue.from_string("b1001"), LogicValue.from_string("b1001"))
        self.assertEqual(r.bits, "1001")


class TestEvaluateExpression(unittest.TestCase):
    """End-to-end evaluation via AST."""

    def test_simple_and(self):
        ast = parse_expression("a & b")
        r = evaluate_expression(ast, {"a": LogicValue.from_string("1"), "b": LogicValue.from_string("1")})
        self.assertEqual(r.bits, "1")

    def test_simple_or(self):
        ast = parse_expression("a | b")
        r = evaluate_expression(ast, {"a": LogicValue.from_string("0"), "b": LogicValue.from_string("1")})
        self.assertEqual(r.bits, "1")

    def test_precedence(self):
        """a | b & c  =  a | (b & c)"""
        ast = parse_expression("a | b & c")
        vals = {"a": LogicValue.from_string("0"), "b": LogicValue.from_string("0"), "c": LogicValue.from_string("1")}
        r = evaluate_expression(ast, vals)
        self.assertEqual(r.bits, "0")  # 0 | (0 & 1) = 0 | 0 = 0

    def test_parens(self):
        ast = parse_expression("(a | b) & c")
        vals = {"a": LogicValue.from_string("0"), "b": LogicValue.from_string("1"), "c": LogicValue.from_string("1")}
        r = evaluate_expression(ast, vals)
        self.assertEqual(r.bits, "1")

    def test_ternary(self):
        ast = parse_expression("a ? b : c")
        r = evaluate_expression(ast, {
            "a": LogicValue.from_string("1"),
            "b": LogicValue.from_int(42, 8),
            "c": LogicValue.from_int(0, 8),
        })
        self.assertEqual(r.to_int(), 42)

    def test_ternary_with_assignment(self):
        """Custom: a ? b = c : d  treated as  a ? c : d."""
        ast = parse_expression("a ? b = c : d")
        r = evaluate_expression(ast, {
            "a": LogicValue.from_string("1"),
            "b": LogicValue.from_int(99, 8),
            "c": LogicValue.from_int(42, 8),
            "d": LogicValue.from_int(0, 8),
        })
        self.assertEqual(r.to_int(), 42)

    def test_nested_expression(self):
        ast = parse_expression("((^(a & b | c)) << 3)")
        vals = {
            "a": LogicValue.from_string("b1010"),
            "b": LogicValue.from_string("b1100"),
            "c": LogicValue.from_string("b0011"),
        }
        r = evaluate_expression(ast, vals)
        # Just verify it doesn't crash and returns something
        self.assertIsNotNone(r)

    def test_missing_signal(self):
        ast = parse_expression("a & b")
        r = evaluate_expression(ast, {"a": LogicValue.from_string("1")})
        # b is missing -> x & 1 = x
        self.assertEqual(r.bits, "x")

    def test_literal_in_expression(self):
        ast = parse_expression("a & 4'b1010")
        r = evaluate_expression(ast, {"a": LogicValue.from_string("b1111")})
        self.assertEqual(r.bits, "1010")


class TestIterTransitions(unittest.TestCase):
    """Event-driven transition iteration tests."""

    def test_simple_and_transitions(self):
        ast = parse_expression("a & b")
        a_trans = [{"t": 0, "v": "0"}, {"t": 10, "v": "1"}, {"t": 20, "v": "0"}]
        b_trans = [{"t": 0, "v": "0"}, {"t": 5, "v": "1"}, {"t": 15, "v": "0"}]
        trans = list(iter_virtual_transitions(ast, {"a": a_trans, "b": b_trans}, 0, 25))
        times = [t["t"] for t in trans]
        vals = [t["v"] for t in trans]
        # At 0: 0&0=0, at 5: 0&1=0(no change), at 10: 1&1=1, at 15: 1&0=0, at 20: 0&0=0(no change)
        self.assertIn(10, times)
        idx10 = times.index(10)
        self.assertEqual(vals[idx10], "1")

    def test_no_change_suppression(self):
        ast = parse_expression("a & b")
        a_trans = [{"t": 0, "v": "1"}, {"t": 10, "v": "1"}]  # no actual change
        b_trans = [{"t": 0, "v": "0"}, {"t": 5, "v": "0"}]
        trans = list(iter_virtual_transitions(ast, {"a": a_trans, "b": b_trans}, 0, 20))
        vals = [t["v"] for t in trans]
        # All should be 0
        for v in vals:
            self.assertEqual(v, "0")

    def test_same_timestamp_batching(self):
        """Both operands change at same time — single evaluation."""
        ast = parse_expression("a | b")
        a_trans = [{"t": 0, "v": "0"}, {"t": 10, "v": "1"}]
        b_trans = [{"t": 0, "v": "0"}, {"t": 10, "v": "1"}]
        trans = list(iter_virtual_transitions(ast, {"a": a_trans, "b": b_trans}, 0, 20))
        # At 0: 0|0=0, at 10: 1|1=1
        self.assertEqual(len(trans), 2)

    def test_start_time_seeding(self):
        """Value at start_time uses last transition before start."""
        ast = parse_expression("a & b")
        a_trans = [{"t": 0, "v": "1"}, {"t": 100, "v": "0"}]
        b_trans = [{"t": 0, "v": "1"}, {"t": 50, "v": "0"}]
        # Start at 25 — a=1, b=1 => seed is 1
        trans = list(iter_virtual_transitions(ast, {"a": a_trans, "b": b_trans}, 25, 200))
        self.assertEqual(trans[0]["v"], "1")
        # b changes to 0 at 50 => 1&0=0
        self.assertTrue(any(t["t"] == 50 and t["v"] == "0" for t in trans))

    def test_empty_transitions(self):
        ast = parse_expression("a & b")
        trans = list(iter_virtual_transitions(ast, {"a": [], "b": []}, 0, 100))
        # Should yield seed value (x & x = x)
        self.assertEqual(len(trans), 1)
        self.assertEqual(trans[0]["v"], "x")


if __name__ == "__main__":
    unittest.main()
