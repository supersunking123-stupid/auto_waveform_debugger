"""Unit tests for expression_parser.py — tokenizer, parser, AST utilities."""

import unittest

from agent_debug_automation.expression_parser import (
    ParseError,
    collect_signal_refs,
    parse_expression,
    _tokenize,
    _Token,
    _TT,
)


class TestTokenizer(unittest.TestCase):
    """Token-level tests."""

    def _tokens(self, expr: str) -> list:
        return _tokenize(expr)

    def test_simple_signal(self):
        toks = self._tokens("a")
        self.assertEqual(len(toks), 2)  # signal + EOF
        self.assertEqual(toks[0].tt, _TT.SIGNAL)
        self.assertEqual(toks[0].text, "a")

    def test_dotted_signal(self):
        toks = self._tokens("top.module.sig")
        self.assertEqual(toks[0].tt, _TT.SIGNAL)
        self.assertEqual(toks[0].text, "top.module.sig")

    def test_bracketed_signal(self):
        toks = self._tokens("tb.bus[3:0]")
        self.assertEqual(toks[0].tt, _TT.SIGNAL)
        self.assertEqual(toks[0].text, "tb.bus[3:0]")

    def test_multiple_bracket_suffixes(self):
        toks = self._tokens("top.arr[3][1:0]")
        self.assertEqual(toks[0].tt, _TT.SIGNAL)
        self.assertEqual(toks[0].text, "top.arr[3][1:0]")

    def test_decimal_literal(self):
        toks = self._tokens("42")
        self.assertEqual(toks[0].tt, _TT.LITERAL)
        self.assertEqual(toks[0].text, "42")

    def test_sized_binary_literal(self):
        toks = self._tokens("4'b1010")
        self.assertEqual(toks[0].tt, _TT.LITERAL)
        self.assertEqual(toks[0].text, "4'b1010")

    def test_sized_hex_literal(self):
        toks = self._tokens("8'hFF")
        self.assertEqual(toks[0].tt, _TT.LITERAL)
        self.assertEqual(toks[0].text, "8'b11111111")

    def test_sized_decimal_literal(self):
        toks = self._tokens("8'd255")
        self.assertEqual(toks[0].tt, _TT.LITERAL)
        self.assertEqual(toks[0].text, "8'b11111111")

    def test_unsized_xz(self):
        toks = self._tokens("'x")
        self.assertEqual(toks[0].tt, _TT.LITERAL)
        self.assertEqual(toks[0].text, "'x")

    def test_unsized_z(self):
        toks = self._tokens("'z")
        self.assertEqual(toks[0].tt, _TT.LITERAL)
        self.assertEqual(toks[0].text, "'z")

    def test_multi_char_ops(self):
        expr = "a ~& b ~| c ~^ d ^~ e"
        toks = self._tokens(expr)
        op_texts = [t.text for t in toks if t.tt == _TT.OP]
        self.assertEqual(op_texts, ["~&", "~|", "~^", "^~"])

    def test_shift_ops(self):
        expr = "a << b >>> c <<< d >> e"
        toks = self._tokens(expr)
        op_texts = [t.text for t in toks if t.tt == _TT.OP]
        self.assertEqual(op_texts, ["<<", ">>>", "<<<", ">>"])

    def test_logical_ops(self):
        expr = "a && b || c"
        toks = self._tokens(expr)
        op_texts = [t.text for t in toks if t.tt == _TT.OP]
        self.assertEqual(op_texts, ["&&", "||"])

    def test_comparison_ops(self):
        expr = "a == b != c <= d >= e < f > g"
        toks = self._tokens(expr)
        op_texts = [t.text for t in toks if t.tt == _TT.OP]
        self.assertEqual(op_texts, ["==", "!=", "<=", ">=", "<", ">"])

    def test_arithmetic_ops(self):
        expr = "a + b - c * d / e % f ** g"
        toks = self._tokens(expr)
        op_texts = [t.text for t in toks if t.tt == _TT.OP]
        self.assertEqual(op_texts, ["+", "-", "*", "/", "%", "**"])

    def test_unary_ops(self):
        expr = "~a !b &c |d ^e"
        toks = self._tokens(expr)
        op_texts = [t.text for t in toks if t.tt == _TT.OP]
        self.assertEqual(op_texts, ["~", "!", "&", "|", "^"])

    def test_maximal_munch_le_ge(self):
        """<= and >= are tokenized as single ops, not < followed by =."""
        toks = self._tokens("a <= b")
        op_texts = [t.text for t in toks if t.tt not in (_TT.EOF,)]
        self.assertIn("<=", op_texts)
        self.assertNotIn(["<", "="], [op_texts[i:i+2] for i in range(len(op_texts)-1)])

    def test_unexpected_char(self):
        with self.assertRaises(ParseError):
            self._tokens("a @ b")

    def test_ternary_punctuation(self):
        toks = self._tokens("a ? b : c")
        types = [t.tt for t in toks if t.tt != _TT.EOF]
        self.assertEqual(types, [_TT.SIGNAL, _TT.QUESTION, _TT.SIGNAL, _TT.COLON, _TT.SIGNAL])

    def test_eq_token(self):
        toks = self._tokens("a = b")
        eq_toks = [t for t in toks if t.tt == _TT.EQ]
        self.assertEqual(len(eq_toks), 1)


class TestParserBasic(unittest.TestCase):
    """Basic parsing tests for each operator."""

    def test_simple_signal_ref(self):
        ast = parse_expression("a")
        self.assertEqual(ast, {"type": "SignalRef", "path": "a"})

    def test_bracketed_signal_ref(self):
        ast = parse_expression("tb.bus[3:0]")
        self.assertEqual(ast, {"type": "SignalRef", "path": "tb.bus[3:0]"})

    def test_simple_literal(self):
        ast = parse_expression("1")
        self.assertEqual(ast["type"], "Literal")
        self.assertEqual(ast["value"], "1")

    def test_bitwise_and(self):
        ast = parse_expression("a & b")
        self.assertEqual(ast, {
            "type": "BinaryOp", "op": "&",
            "left": {"type": "SignalRef", "path": "a"},
            "right": {"type": "SignalRef", "path": "b"},
        })

    def test_bitwise_or(self):
        ast = parse_expression("a | b")
        self.assertEqual(ast["op"], "|")

    def test_bitwise_xor(self):
        ast = parse_expression("a ^ b")
        self.assertEqual(ast["op"], "^")

    def test_binary_nand(self):
        ast = parse_expression("a ~& b")
        self.assertEqual(ast, {
            "type": "BinaryOp", "op": "~&",
            "left": {"type": "SignalRef", "path": "a"},
            "right": {"type": "SignalRef", "path": "b"},
        })

    def test_binary_nor(self):
        ast = parse_expression("a ~| b")
        self.assertEqual(ast["op"], "~|")

    def test_binary_xnor(self):
        ast = parse_expression("a ~^ b")
        self.assertEqual(ast["op"], "~^")

    def test_xnor_alias(self):
        """^~ is normalized to ~^."""
        ast = parse_expression("a ^~ b")
        self.assertEqual(ast["op"], "~^")

    def test_logical_and(self):
        ast = parse_expression("a && b")
        self.assertEqual(ast["op"], "&&")

    def test_logical_or(self):
        ast = parse_expression("a || b")
        self.assertEqual(ast["op"], "||")

    def test_equality(self):
        ast = parse_expression("a == b")
        self.assertEqual(ast["op"], "==")

    def test_inequality(self):
        ast = parse_expression("a != b")
        self.assertEqual(ast["op"], "!=")

    def test_less_than(self):
        ast = parse_expression("a < b")
        self.assertEqual(ast["op"], "<")

    def test_less_equal(self):
        ast = parse_expression("a <= b")
        self.assertEqual(ast["op"], "<=")

    def test_greater_than(self):
        ast = parse_expression("a > b")
        self.assertEqual(ast["op"], ">")

    def test_greater_equal(self):
        ast = parse_expression("a >= b")
        self.assertEqual(ast["op"], ">=")

    def test_shift_left(self):
        ast = parse_expression("a << 2")
        self.assertEqual(ast["op"], "<<")

    def test_shift_right(self):
        ast = parse_expression("a >> 2")
        self.assertEqual(ast["op"], ">>")

    def test_arith_shift_left(self):
        ast = parse_expression("a <<< 2")
        self.assertEqual(ast["op"], "<<<")

    def test_arith_shift_right(self):
        ast = parse_expression("a >>> 2")
        self.assertEqual(ast["op"], ">>>")

    def test_addition(self):
        ast = parse_expression("a + b")
        self.assertEqual(ast["op"], "+")

    def test_subtraction(self):
        ast = parse_expression("a - b")
        self.assertEqual(ast["op"], "-")

    def test_multiplication(self):
        ast = parse_expression("a * b")
        self.assertEqual(ast["op"], "*")

    def test_division(self):
        ast = parse_expression("a / b")
        self.assertEqual(ast["op"], "/")

    def test_modulo(self):
        ast = parse_expression("a % b")
        self.assertEqual(ast["op"], "%")

    def test_power(self):
        ast = parse_expression("a ** b")
        self.assertEqual(ast["op"], "**")

    def test_unary_not(self):
        ast = parse_expression("~a")
        self.assertEqual(ast, {"type": "UnaryOp", "op": "~", "operand": {"type": "SignalRef", "path": "a"}})

    def test_unary_logical_not(self):
        ast = parse_expression("!a")
        self.assertEqual(ast["op"], "!")

    def test_unary_reduction_and(self):
        ast = parse_expression("&a")
        self.assertEqual(ast, {"type": "UnaryOp", "op": "&", "operand": {"type": "SignalRef", "path": "a"}})

    def test_unary_reduction_or(self):
        ast = parse_expression("|a")
        self.assertEqual(ast["op"], "|")

    def test_unary_reduction_xor(self):
        ast = parse_expression("^a")
        self.assertEqual(ast["op"], "^")

    def test_unary_reduction_nand(self):
        ast = parse_expression("~&a")
        self.assertEqual(ast["op"], "~&")

    def test_unary_reduction_nor(self):
        ast = parse_expression("~|a")
        self.assertEqual(ast["op"], "~|")

    def test_unary_reduction_xnor(self):
        ast = parse_expression("~^a")
        self.assertEqual(ast["op"], "~^")

    def test_unary_xnor_alias(self):
        ast = parse_expression("^~a")
        self.assertEqual(ast["op"], "~^")

    def test_ternary(self):
        ast = parse_expression("a ? b : c")
        self.assertEqual(ast["type"], "TernaryOp")
        self.assertEqual(ast["cond"], {"type": "SignalRef", "path": "a"})
        self.assertEqual(ast["true_val"], {"type": "SignalRef", "path": "b"})
        self.assertEqual(ast["false_val"], {"type": "SignalRef", "path": "c"})

    def test_ternary_with_assignment(self):
        """Custom extension: cond ? a = b : c treated as cond ? b : c."""
        ast = parse_expression("a ? b = c : d")
        self.assertEqual(ast["type"], "TernaryOp")
        self.assertEqual(ast["true_val"], {"type": "SignalRef", "path": "c"})

    def test_parenthesized(self):
        ast = parse_expression("(a | b) & c")
        self.assertEqual(ast["op"], "&")
        self.assertEqual(ast["left"]["op"], "|")

    def test_unary_minus(self):
        ast = parse_expression("-a")
        self.assertEqual(ast, {"type": "UnaryOp", "op": "unary-", "operand": {"type": "SignalRef", "path": "a"}})

    def test_unary_plus(self):
        ast = parse_expression("+a")
        self.assertEqual(ast, {"type": "SignalRef", "path": "a"})


class TestPrecedence(unittest.TestCase):
    """Verify operator precedence matches Verilog standard."""

    def _binop(self, op, left, right):
        return {"type": "BinaryOp", "op": op, "left": left, "right": right}

    def _sig(self, name):
        return {"type": "SignalRef", "path": name}

    def test_and_binds_tighter_than_or(self):
        """a | b & c  =>  a | (b & c)"""
        ast = parse_expression("a | b & c")
        self.assertEqual(ast["op"], "|")
        self.assertEqual(ast["right"]["op"], "&")

    def test_xor_binds_tighter_than_or(self):
        ast = parse_expression("a | b ^ c")
        self.assertEqual(ast["op"], "|")
        self.assertEqual(ast["right"]["op"], "^")

    def test_and_binds_tighter_than_xor(self):
        ast = parse_expression("a ^ b & c")
        self.assertEqual(ast["op"], "^")
        self.assertEqual(ast["right"]["op"], "&")

    def test_equality_binds_tighter_than_and(self):
        ast = parse_expression("a & b == c")
        self.assertEqual(ast["op"], "&")
        self.assertEqual(ast["right"]["op"], "==")

    def test_relational_binds_tighter_than_equality(self):
        ast = parse_expression("a == b < c")
        self.assertEqual(ast["op"], "==")
        self.assertEqual(ast["right"]["op"], "<")

    def test_shift_binds_tighter_than_relational(self):
        ast = parse_expression("a < b << c")
        self.assertEqual(ast["op"], "<")
        self.assertEqual(ast["right"]["op"], "<<")

    def test_additive_binds_tighter_than_shift(self):
        ast = parse_expression("a << b + c")
        self.assertEqual(ast["op"], "<<")
        self.assertEqual(ast["right"]["op"], "+")

    def test_mult_binds_tighter_than_add(self):
        ast = parse_expression("a + b * c")
        self.assertEqual(ast["op"], "+")
        self.assertEqual(ast["right"]["op"], "*")

    def test_power_binds_tighter_than_mult(self):
        ast = parse_expression("a * b ** c")
        self.assertEqual(ast["op"], "*")
        self.assertEqual(ast["right"]["op"], "**")

    def test_logical_or_lowest_binary(self):
        ast = parse_expression("a || b & c")
        self.assertEqual(ast["op"], "||")

    def test_logical_and_binds_tighter_than_or(self):
        ast = parse_expression("a || b && c")
        self.assertEqual(ast["op"], "||")
        self.assertEqual(ast["right"]["op"], "&&")


class TestAssociativity(unittest.TestCase):

    def test_add_left_associative(self):
        """a + b + c  =>  (a + b) + c"""
        ast = parse_expression("a + b + c")
        self.assertEqual(ast["op"], "+")
        self.assertEqual(ast["left"]["op"], "+")

    def test_sub_left_associative(self):
        ast = parse_expression("a - b - c")
        self.assertEqual(ast["op"], "-")
        self.assertEqual(ast["left"]["op"], "-")

    def test_power_right_associative(self):
        """a ** b ** c  =>  a ** (b ** c)"""
        ast = parse_expression("a ** b ** c")
        self.assertEqual(ast["op"], "**")
        self.assertEqual(ast["right"]["op"], "**")

    def test_ternary_right_associative(self):
        """a ? b : c ? d : e  =>  a ? b : (c ? d : e)"""
        ast = parse_expression("a ? b : c ? d : e")
        self.assertEqual(ast["type"], "TernaryOp")
        self.assertEqual(ast["false_val"]["type"], "TernaryOp")


class TestCollectSignalRefs(unittest.TestCase):

    def test_single_ref(self):
        refs = collect_signal_refs(parse_expression("a"))
        self.assertEqual(refs, ["a"])

    def test_binary_refs(self):
        refs = collect_signal_refs(parse_expression("a & b"))
        self.assertEqual(refs, ["a", "b"])

    def test_nested_refs(self):
        refs = collect_signal_refs(parse_expression("(a & b) | c"))
        self.assertEqual(refs, ["a", "b", "c"])

    def test_dedup(self):
        refs = collect_signal_refs(parse_expression("a & a | a"))
        self.assertEqual(refs, ["a"])

    def test_ternary_refs(self):
        refs = collect_signal_refs(parse_expression("a ? b : c"))
        self.assertEqual(refs, ["a", "b", "c"])

    def test_unary_refs(self):
        refs = collect_signal_refs(parse_expression("~a"))
        self.assertEqual(refs, ["a"])

    def test_no_refs_for_literal_only(self):
        refs = collect_signal_refs(parse_expression("42"))
        self.assertEqual(refs, [])

    def test_dotted_path(self):
        refs = collect_signal_refs(parse_expression("top.module.sig"))
        self.assertEqual(refs, ["top.module.sig"])

    def test_bracketed_path(self):
        refs = collect_signal_refs(parse_expression("tb.bus[3:0] & top.arr[1]"))
        self.assertEqual(refs, ["tb.bus[3:0]", "top.arr[1]"])

    def test_manual_bus_op_nodes(self):
        refs = collect_signal_refs({
            "type": "ConcatOp",
            "operands": [
                {
                    "type": "SliceOp",
                    "operand": {"type": "SignalRef", "path": "bus"},
                    "msb": 3,
                    "lsb": 2,
                },
                {
                    "type": "ReverseOp",
                    "operand": {"type": "SignalRef", "path": "other_bus"},
                },
            ],
        })
        self.assertEqual(refs, ["bus", "other_bus"])


class TestComplexExpressions(unittest.TestCase):

    def test_nested_parens(self):
        ast = parse_expression("((^(a & b | c)) << 3)")
        refs = collect_signal_refs(ast)
        self.assertIn("a", refs)
        self.assertIn("b", refs)
        self.assertIn("c", refs)

    def test_mixed_operators(self):
        ast = parse_expression("a & b | c ^ d && e")
        # Should parse without error
        self.assertIn("type", ast)

    def test_complex_ternary(self):
        ast = parse_expression("a == 1 ? b & c : d | e")
        self.assertEqual(ast["type"], "TernaryOp")
        self.assertEqual(ast["cond"]["op"], "==")

    def test_sized_literal_in_expression(self):
        ast = parse_expression("a & 4'b1010")
        self.assertEqual(ast["right"]["type"], "Literal")
        self.assertEqual(ast["right"]["width"], 4)


class TestErrorHandling(unittest.TestCase):

    def test_empty_expression(self):
        with self.assertRaises(ParseError):
            parse_expression("")

    def test_mismatched_parens(self):
        with self.assertRaises(ParseError):
            parse_expression("(a & b")

    def test_extra_closing_paren(self):
        with self.assertRaises(ParseError):
            parse_expression("a & b)")

    def test_missing_operand(self):
        with self.assertRaises(ParseError):
            parse_expression("a &")

    def test_double_operator(self):
        """a & &b is valid: bitwise AND of a with reduction AND of b."""
        ast = parse_expression("a & &b")
        self.assertEqual(ast["op"], "&")
        self.assertEqual(ast["right"]["op"], "&")  # unary reduction AND
        self.assertEqual(ast["right"]["type"], "UnaryOp")

    def test_invalid_sized_constant(self):
        with self.assertRaises(ParseError):
            parse_expression("4'x1010")

    def test_unmatched_bracket(self):
        with self.assertRaises(ParseError):
            parse_expression("tb.bus[3:0")

    def test_empty_bracket_suffix(self):
        with self.assertRaises(ParseError):
            parse_expression("tb.bus[]")


class TestLiteralParsing(unittest.TestCase):

    def test_sized_binary(self):
        ast = parse_expression("4'b1010")
        self.assertEqual(ast["width"], 4)
        self.assertEqual(ast["value"], "4'b1010")

    def test_sized_hex(self):
        ast = parse_expression("8'hAF")
        self.assertEqual(ast["width"], 8)

    def test_sized_decimal(self):
        ast = parse_expression("8'd255")
        self.assertEqual(ast["width"], 8)

    def test_plain_decimal(self):
        ast = parse_expression("42")
        self.assertEqual(ast["type"], "Literal")
        self.assertIn("value", ast)

    def test_x_literal(self):
        ast = parse_expression("'x")
        self.assertEqual(ast["value"], "x")
        self.assertEqual(ast["width"], 1)

    def test_z_literal(self):
        ast = parse_expression("'z")
        self.assertEqual(ast["value"], "z")
        self.assertEqual(ast["width"], 1)


if __name__ == "__main__":
    unittest.main()
