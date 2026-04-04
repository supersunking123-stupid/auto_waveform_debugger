"""Verilog expression parser — tokenizer, recursive descent, AST utilities.

Supports the full Verilog operator precedence table plus custom extensions
(binary ~& ~| ~^, = in ternary).  AST nodes are plain dicts for easy JSON
serialization into session files.
"""

from __future__ import annotations

import re
from typing import Any, Dict, List, Optional, Sequence, Tuple

# ---------------------------------------------------------------------------
# Token types
# ---------------------------------------------------------------------------

class _TT:
    """Token type constants (short names keep AST compact)."""
    SIGNAL = "SignalRef"
    LITERAL = "Literal"
    OP = "op"
    LPAREN = "("
    RPAREN = ")"
    QUESTION = "?"
    COLON = ":"
    EQ = "="  # assignment inside ternary
    EOF = "EOF"


class _Token:
    __slots__ = ("tt", "text", "pos")

    def __init__(self, tt: str, text: str, pos: int) -> None:
        self.tt = tt
        self.text = text
        self.pos = pos

    def __repr__(self) -> str:  # pragma: no cover
        return f"Token({self.tt!r}, {self.text!r}, @{self.pos})"


# ---------------------------------------------------------------------------
# Tokenizer
# ---------------------------------------------------------------------------

# Multi-character operators ordered longest-first for maximal munch.
_MULTI_CHAR_OPS = [
    "<<<", ">>>",  # 3-char shifts
    "===", "!==",   # 4-state equality (3-char)
    "<<", ">>",     # 2-char shifts
    "~&", "~|", "~^", "^~",  # 2-char negated bitwise / xnor
    "&&", "||",     # 2-char logical
    "**",           # power
    "==", "!=",     # 2-char equality
    "<=", ">=",     # 2-char relational
]

_SINGLE_CHAR_OPS = set("~!&|^<>+-*/%")

_IDENT_RE = re.compile(r"[A-Za-z_$][A-Za-z0-9_.$]*")
_UNSIGNED_NUM_RE = re.compile(r"[0-9][0-9_]*")
_HEX_DIGIT_RE = re.compile(r"[0-9a-fA-F_]+")
_BIN_DIGIT_RE = re.compile(r"[01_xXzZ_]+")
_OCT_DIGIT_RE = re.compile(r"[0-7_xXzZ_]+")
_DEC_DIGIT_RE = re.compile(r"[0-9_xXzZ_]+")


class ParseError(Exception):
    """Raised on invalid expression syntax."""

    def __init__(self, message: str, pos: int = -1) -> None:
        self.pos = pos
        super().__init__(message)


def _tokenize(expr: str) -> List[_Token]:
    """Split *expr* into a list of tokens."""
    tokens: List[_Token] = []
    i = 0
    n = len(expr)

    while i < n:
        ch = expr[i]

        # whitespace
        if ch in " \t\r\n":
            i += 1
            continue

        # parenthesised Verilog constant: 'x, 'z, '0, '1
        if ch == "'" and i + 1 < n and expr[i + 1] in "xzXZ01":
            val_char = expr[i + 1].lower()
            tokens.append(_Token(_TT.LITERAL, f"'{val_char}", i))
            i += 2
            continue

        # sized constant: 4'b1010, 32'hFF, 8'd255, 16'o77
        if ch.isdigit():
            m = _UNSIGNED_NUM_RE.match(expr, i)
            if m:
                num_str = m.group()
                after = m.end()
                # Check for 'b, 'o, 'd, 'h, 'B, 'O, 'D, 'H
                if after < n and expr[after] == "'" and after + 1 < n and expr[after + 1].lower() in "bodh":
                    radix_ch = expr[after + 1].lower()
                    size = int(num_str.replace("_", ""))
                    digit_start = after + 2
                    digit_patterns = {"b": _BIN_DIGIT_RE, "o": _OCT_DIGIT_RE, "d": _DEC_DIGIT_RE, "h": _HEX_DIGIT_RE}
                    dm = digit_patterns[radix_ch].match(expr, digit_start)
                    if not dm:
                        raise ParseError(f"invalid digits after {size}'{radix_ch}", digit_start)
                    digit_str = dm.group()
                    end = dm.end()
                    # Convert to canonical form
                    value = _sized_constant_to_value(size, radix_ch, digit_str)
                    tokens.append(_Token(_TT.LITERAL, value, i))
                    i = end
                    continue
                # Unsized decimal number
                tokens.append(_Token(_TT.LITERAL, num_str.replace("_", ""), i))
                i = after
                continue
            # single digit (shouldn't happen if regex above works)
            tokens.append(_Token(_TT.LITERAL, ch, i))
            i += 1
            continue

        # identifier / signal path
        if ch.isalpha() or ch == "_" or ch == "$":
            m = _IDENT_RE.match(expr, i)
            if not m:
                raise ParseError(f"invalid identifier at position {i}", i)
            tokens.append(_Token(_TT.SIGNAL, m.group(), i))
            i = m.end()
            continue

        # multi-char operators (maximal munch)
        found_multi = False
        for op in _MULTI_CHAR_OPS:
            if expr[i:i + len(op)] == op:
                tokens.append(_Token(_TT.OP, op, i))
                i += len(op)
                found_multi = True
                break
        if found_multi:
            continue

        # single-char operators and punctuation
        if ch in _SINGLE_CHAR_OPS:
            tokens.append(_Token(_TT.OP, ch, i))
            i += 1
            continue

        if ch == "(":
            tokens.append(_Token(_TT.LPAREN, "(", i))
            i += 1
            continue
        if ch == ")":
            tokens.append(_Token(_TT.RPAREN, ")", i))
            i += 1
            continue
        if ch == "?":
            tokens.append(_Token(_TT.QUESTION, "?", i))
            i += 1
            continue
        if ch == ":":
            tokens.append(_Token(_TT.COLON, ":", i))
            i += 1
            continue
        if ch == "=":
            tokens.append(_Token(_TT.EQ, "=", i))
            i += 1
            continue

        raise ParseError(f"unexpected character '{ch}' at position {i}", i)

    tokens.append(_Token(_TT.EOF, "", n))
    return tokens


def _sized_constant_to_value(size: int, radix: str, digits: str) -> str:
    """Convert a sized Verilog constant to internal canonical value string."""
    digits_clean = digits.replace("_", "").lower()
    # Replace x/z with lowercase
    if radix == "b":
        bits = digits_clean
    elif radix == "o":
        bits = _oct_to_bits(digits_clean)
    elif radix == "h":
        bits = _hex_to_bits(digits_clean)
    elif radix == "d":
        bits = _dec_to_bits(digits_clean, size)
    else:
        raise ParseError(f"unknown radix '{radix}'")

    # Pad or truncate to size
    if len(bits) < size:
        # sign-extend-like: pad with 0 for unsigned, leading bit for signed
        bits = bits.rjust(size, "0")
    elif len(bits) > size:
        bits = bits[-size:]  # truncate MSBs

    return f"{size}'b{bits}"


def _hex_to_bits(hex_str: str) -> str:
    """Convert hex string to binary bit string, preserving x/z."""
    _HEX_MAP = {
        "0": "0000", "1": "0001", "2": "0010", "3": "0011",
        "4": "0100", "5": "0101", "6": "0110", "7": "0111",
        "8": "1000", "9": "1001", "a": "1010", "b": "1011",
        "c": "1100", "d": "1101", "e": "1110", "f": "1111",
        "x": "xxxx", "z": "zzzz",
    }
    bits = []
    for ch in hex_str:
        if ch in _HEX_MAP:
            bits.append(_HEX_MAP[ch])
        else:
            raise ParseError(f"invalid hex digit '{ch}'")
    return "".join(bits)


def _oct_to_bits(oct_str: str) -> str:
    """Convert octal string to binary bit string, preserving x/z."""
    _OCT_MAP = {
        "0": "000", "1": "001", "2": "010", "3": "011",
        "4": "100", "5": "101", "6": "110", "7": "111",
        "x": "xxx", "z": "zzz",
    }
    bits = []
    for ch in oct_str:
        if ch in _OCT_MAP:
            bits.append(_OCT_MAP[ch])
        else:
            raise ParseError(f"invalid octal digit '{ch}'")
    return "".join(bits)


def _dec_to_bits(dec_str: str, size: int) -> str:
    """Convert decimal string to binary bit string. x/z not supported in decimal."""
    if any(c in "xz" for c in dec_str):
        raise ParseError("x/z not supported in decimal literals")
    n = int(dec_str)
    bits = bin(n)[2:]
    return bits.rjust(size, "0")


# ---------------------------------------------------------------------------
# Recursive-descent parser
# ---------------------------------------------------------------------------

class _Parser:
    """Recursive descent parser following Verilog operator precedence.

    Precedence (lowest to highest):
      14  ?: (ternary, right-assoc)
      13  || (logical OR)
      12  && (logical AND)
      11  |  (bitwise OR)
      10  ^  ~^ ^~ (bitwise XOR/XNOR)
       9  &  (bitwise AND)
       8  == != (equality)
       7  < <= > >= (relational)
       6  << >> <<< >>> (shift)
       5  + - (additive)
       4  * / % (multiplicative)
       3  ** (power, right-assoc)
       2  unary: ! ~ ~& ~| ~^ ^~ & | ^  (prefix)
       1  primary: literal | signal | (expr)
    """

    # Unary prefix operators (some overlap with binary — context decides).
    _UNARY_OPS = frozenset(["!", "~", "~&", "~|", "~^", "^~", "&", "|", "^"])

    def __init__(self, tokens: List[_Token]) -> None:
        self._tokens = tokens
        self._pos = 0

    def _peek(self) -> _Token:
        return self._tokens[self._pos]

    def _advance(self) -> _Token:
        tok = self._tokens[self._pos]
        self._pos += 1
        return tok

    def _expect(self, tt: str, text: Optional[str] = None) -> _Token:
        tok = self._peek()
        if tok.tt != tt or (text is not None and tok.text != text):
            raise ParseError(
                f"expected {tt}{' ' + text if text else ''} but got {tok.tt} '{tok.text}' at position {tok.pos}",
                tok.pos,
            )
        return self._advance()

    # ---- entry point ----

    def parse(self) -> Dict[str, Any]:
        node = self._parse_ternary()
        if self._peek().tt != _TT.EOF:
            tok = self._peek()
            raise ParseError(f"unexpected token '{tok.text}' at position {tok.pos}", tok.pos)
        return node

    # ---- precedence levels ----

    def _parse_ternary(self) -> Dict[str, Any]:
        """Level 14: ?: (right-associative)."""
        node = self._parse_logical_or()
        if self._peek().tt == _TT.QUESTION:
            self._advance()  # consume '?'
            # true branch: may contain '=' as custom extension
            true_val = self._parse_ternary_inner()
            self._expect(_TT.COLON)
            false_val = self._parse_ternary()
            return {"type": "TernaryOp", "cond": node, "true_val": true_val, "false_val": false_val}
        return node

    def _parse_ternary_inner(self) -> Dict[str, Any]:
        """Parse true-branch of ternary, allowing '=' as assignment."""
        node = self._parse_logical_or()
        if self._peek().tt == _TT.EQ:
            # Custom extension: a = b inside ternary -> just return b
            self._advance()  # consume '='
            return self._parse_logical_or()
        if self._peek().tt == _TT.QUESTION:
            self._advance()
            true_val = self._parse_ternary_inner()
            self._expect(_TT.COLON)
            false_val = self._parse_ternary()
            return {"type": "TernaryOp", "cond": node, "true_val": true_val, "false_val": false_val}
        return node

    def _parse_logical_or(self) -> Dict[str, Any]:
        """Level 13: || (left-associative)."""
        node = self._parse_logical_and()
        while self._peek().tt == _TT.OP and self._peek().text == "||":
            self._advance()
            right = self._parse_logical_and()
            node = {"type": "BinaryOp", "op": "||", "left": node, "right": right}
        return node

    def _parse_logical_and(self) -> Dict[str, Any]:
        """Level 12: && (left-associative)."""
        node = self._parse_bitwise_or()
        while self._peek().tt == _TT.OP and self._peek().text == "&&":
            self._advance()
            right = self._parse_bitwise_or()
            node = {"type": "BinaryOp", "op": "&&", "left": node, "right": right}
        return node

    def _parse_bitwise_or(self) -> Dict[str, Any]:
        """Level 11: | ~| (left-associative).  ~| is custom binary NOR."""
        node = self._parse_bitwise_xor()
        while self._peek().tt == _TT.OP and self._peek().text in ("|", "~|"):
            op = self._advance().text
            right = self._parse_bitwise_xor()
            node = {"type": "BinaryOp", "op": op, "left": node, "right": right}
        return node

    def _parse_bitwise_xor(self) -> Dict[str, Any]:
        """Level 10: ^ ~^ ^~ (left-associative)."""
        node = self._parse_bitwise_and()
        while self._peek().tt == _TT.OP and self._peek().text in ("^", "~^", "^~"):
            op = self._advance().text
            # Normalize ^~ to ~^
            if op == "^~":
                op = "~^"
            right = self._parse_bitwise_and()
            node = {"type": "BinaryOp", "op": op, "left": node, "right": right}
        return node

    def _parse_bitwise_and(self) -> Dict[str, Any]:
        """Level 9: & ~& (left-associative).  ~& is custom binary NAND."""
        node = self._parse_equality()
        while self._peek().tt == _TT.OP and self._peek().text in ("&", "~&"):
            op = self._advance().text
            right = self._parse_equality()
            node = {"type": "BinaryOp", "op": op, "left": node, "right": right}
        return node

    def _parse_equality(self) -> Dict[str, Any]:
        """Level 8: == != === !== (left-associative)."""
        node = self._parse_relational()
        while self._peek().tt == _TT.OP and self._peek().text in ("==", "!=", "===", "!=="):
            op = self._advance().text
            right = self._parse_relational()
            node = {"type": "BinaryOp", "op": op, "left": node, "right": right}
        return node

    def _parse_relational(self) -> Dict[str, Any]:
        """Level 7: < <= > >= (left-associative)."""
        node = self._parse_shift()
        while self._peek().tt == _TT.OP and self._peek().text in ("<", "<=", ">", ">="):
            op = self._advance().text
            right = self._parse_shift()
            node = {"type": "BinaryOp", "op": op, "left": node, "right": right}
        return node

    def _parse_shift(self) -> Dict[str, Any]:
        """Level 6: << >> <<< >>> (left-associative)."""
        node = self._parse_additive()
        while self._peek().tt == _TT.OP and self._peek().text in ("<<", ">>", "<<<", ">>>"):
            op = self._advance().text
            right = self._parse_additive()
            node = {"type": "BinaryOp", "op": op, "left": node, "right": right}
        return node

    def _parse_additive(self) -> Dict[str, Any]:
        """Level 5: + - (left-associative)."""
        node = self._parse_multiplicative()
        while self._peek().tt == _TT.OP and self._peek().text in ("+", "-"):
            op = self._advance().text
            right = self._parse_multiplicative()
            node = {"type": "BinaryOp", "op": op, "left": node, "right": right}
        return node

    def _parse_multiplicative(self) -> Dict[str, Any]:
        """Level 4: * / % (left-associative)."""
        node = self._parse_power()
        while self._peek().tt == _TT.OP and self._peek().text in ("*", "/", "%"):
            op = self._advance().text
            right = self._parse_power()
            node = {"type": "BinaryOp", "op": op, "left": node, "right": right}
        return node

    def _parse_power(self) -> Dict[str, Any]:
        """Level 3: ** (right-associative)."""
        node = self._parse_unary()
        if self._peek().tt == _TT.OP and self._peek().text == "**":
            self._advance()
            right = self._parse_power()  # right-recursive for right-associativity
            node = {"type": "BinaryOp", "op": "**", "left": node, "right": right}
        return node

    def _parse_unary(self) -> Dict[str, Any]:
        """Level 2: unary prefix operators."""
        tok = self._peek()
        if tok.tt == _TT.OP and tok.text in self._UNARY_OPS:
            op = self._advance().text
            # Normalize ^~ to ~^ for unary too
            if op == "^~":
                op = "~^"
            operand = self._parse_unary()  # right-recursive
            return {"type": "UnaryOp", "op": op, "operand": operand}
        if tok.tt == _TT.OP and tok.text == "-":
            # unary minus
            self._advance()
            operand = self._parse_unary()
            return {"type": "UnaryOp", "op": "unary-", "operand": operand}
        if tok.tt == _TT.OP and tok.text == "+":
            # unary plus (no-op)
            self._advance()
            return self._parse_unary()
        return self._parse_primary()

    def _parse_primary(self) -> Dict[str, Any]:
        """Level 1: literal | signal reference | parenthesized expression."""
        tok = self._peek()

        if tok.tt == _TT.LPAREN:
            self._advance()  # consume '('
            node = self._parse_ternary()
            self._expect(_TT.RPAREN)
            return node

        if tok.tt == _TT.LITERAL:
            self._advance()
            return _literal_node(tok.text)

        if tok.tt == _TT.SIGNAL:
            self._advance()
            return {"type": "SignalRef", "path": tok.text}

        raise ParseError(
            f"unexpected token '{tok.text}' at position {tok.pos}",
            tok.pos,
        )


def _literal_node(raw: str) -> Dict[str, Any]:
    """Build a Literal AST node from a raw token string."""
    # Sized constant: 4'b1010, 32'hFF, etc.
    if "'" in raw:
        if raw.startswith("'"):
            # Unsized: 'x, 'z, '0, '1
            val = raw[1]
            if val in "xz":
                return {"type": "Literal", "value": val, "width": 1, "signed": False}
            # '0 and '1 fill the context width — use 1-bit for now
            return {"type": "Literal", "value": val, "width": 1, "signed": False}
        # sized: "4'b1010"
        return {"type": "Literal", "value": raw, "width": _parse_literal_width(raw), "signed": False}
    # Plain decimal
    try:
        n = int(raw)
        bits = bin(n)[2:] if n >= 0 else bin(n & 0xFFFFFFFF)[2:]
        return {"type": "Literal", "value": bits, "width": max(1, len(bits)), "signed": n < 0}
    except ValueError:
        raise ParseError(f"invalid literal '{raw}'")


def _parse_literal_width(raw: str) -> int:
    """Extract width from sized constant like '4'b1010'."""
    idx = raw.index("'")
    return int(raw[:idx])


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def parse_expression(expr: str) -> Dict[str, Any]:
    """Parse a Verilog-like expression string into an AST.

    Returns the root AST node (a plain dict).
    Raises ParseError on invalid syntax.
    """
    tokens = _tokenize(expr)
    parser = _Parser(tokens)
    return parser.parse()


def collect_signal_refs(ast: Dict[str, Any]) -> List[str]:
    """Walk the AST and collect all signal reference paths (deduplicated, order-preserving)."""
    refs: List[str] = []
    seen: set = set()
    _collect_refs_recursive(ast, refs, seen)
    return refs


def _collect_refs_recursive(node: Dict[str, Any], refs: List[str], seen: set) -> None:
    node_type = node.get("type")
    if node_type == "SignalRef":
        path = node["path"]
        if path not in seen:
            seen.add(path)
            refs.append(path)
    elif node_type == "UnaryOp":
        _collect_refs_recursive(node["operand"], refs, seen)
    elif node_type == "BinaryOp":
        _collect_refs_recursive(node["left"], refs, seen)
        _collect_refs_recursive(node["right"], refs, seen)
    elif node_type == "TernaryOp":
        _collect_refs_recursive(node["cond"], refs, seen)
        _collect_refs_recursive(node["true_val"], refs, seen)
        _collect_refs_recursive(node["false_val"], refs, seen)
    # Literal: no refs to collect
