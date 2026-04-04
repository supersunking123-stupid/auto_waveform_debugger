"""Integration tests for virtual signals — session CRUD, dependency resolution, caching."""

import tempfile
import unittest
from pathlib import Path

from agent_debug_automation.virtual_signals import (
    VirtualSignalService,
    clear_virtual_cache,
    vs_service,
    _topological_sort,
)
from agent_debug_automation.sessions import (
    _default_session_payload,
    _save_session_payload,
    _normalize_session_payload,
)


# Minimal VCD fixture for testing
_VCD_FIXTURE = """$date
  test fixture
$end
$timescale 1ns $end
$scope module TOP $end
$var wire 1 ! a $end
$var wire 1 " b $end
$var wire 4 # bus[3:0] $end
$upscope $end
$enddefinitions $end
#0
0!
0"
b0000 #
#10
1!
#20
1"
b1010 #
#30
0!
b1111 #
#40
0"
b0000 #
#50
1!
1"
b0101 #
#60
$end
"""


class _TestBase(unittest.TestCase):
    """Base class with temp VCD and session setup."""

    @classmethod
    def setUpClass(cls):
        cls.temp_dir = tempfile.TemporaryDirectory()
        cls.vcd_path = Path(cls.temp_dir.name) / "test.vcd"
        cls.vcd_path.write_text(_VCD_FIXTURE, encoding="ascii")

    def _make_session(self, name="test_session"):
        session = _default_session_payload(str(self.vcd_path), session_name=name)
        return _normalize_session_payload(session)

    def setUp(self):
        clear_virtual_cache()


class TestTopologicalSort(unittest.TestCase):

    def test_single_signal(self):
        created = {"v1": {"expression": "a & b", "dependencies": ["a", "b"]}}
        layers = _topological_sort("v1", created)
        self.assertEqual(layers, [["v1"]])

    def test_chained(self):
        created = {
            "v1": {"expression": "a & b", "dependencies": ["a", "b"]},
            "v2": {"expression": "v1 | c", "dependencies": ["v1", "c"]},
        }
        layers = _topological_sort("v2", created)
        # v1 in layer 0, v2 in layer 1
        self.assertEqual(layers, [["v1"], ["v2"]])

    def test_triple_chain(self):
        created = {
            "v1": {"expression": "a", "dependencies": ["a"]},
            "v2": {"expression": "v1 & b", "dependencies": ["v1", "b"]},
            "v3": {"expression": "v2 | c", "dependencies": ["v2", "c"]},
        }
        layers = _topological_sort("v3", created)
        self.assertEqual(layers, [["v1"], ["v2"], ["v3"]])

    def test_cycle_detection(self):
        created = {
            "v1": {"expression": "v2 & a", "dependencies": ["v2", "a"]},
            "v2": {"expression": "v1 | b", "dependencies": ["v1", "b"]},
        }
        with self.assertRaises(ValueError) as ctx:
            _topological_sort("v1", created)
        self.assertIn("circular", str(ctx.exception).lower())

    def test_depth_overflow(self):
        # Create a chain longer than MAX_VIRTUAL_SIGNAL_DEPTH
        created = {}
        for i in range(20):
            dep = f"v{i-1}" if i > 0 else "a"
            created[f"v{i}"] = {"expression": dep, "dependencies": [dep]}
        with self.assertRaises(ValueError) as ctx:
            _topological_sort("v19", created)
        self.assertIn("depth", str(ctx.exception).lower())


class TestVirtualSignalCRUD(_TestBase):

    def test_create_and_list(self):
        service = VirtualSignalService()
        session = self._make_session()

        session = service.create(session, "and_ab", "a & b", "AND of a and b")
        self.assertIn("and_ab", session["created_signals"])

        virtuals = service.list(session)
        self.assertEqual(len(virtuals), 1)
        self.assertEqual(virtuals[0]["signal_name"], "and_ab")
        self.assertEqual(virtuals[0]["expression"], "a & b")

    def test_create_duplicate_fails(self):
        service = VirtualSignalService()
        session = self._make_session()
        session = service.create(session, "v1", "a & b")
        with self.assertRaises(ValueError) as ctx:
            service.create(session, "v1", "a | b")
        self.assertIn("already exists", str(ctx.exception))

    def test_create_invalid_expression(self):
        service = VirtualSignalService()
        session = self._make_session()
        with self.assertRaises(ValueError) as ctx:
            service.create(session, "v1", "a &")
        self.assertIn("parse error", str(ctx.exception).lower())

    def test_update_expression(self):
        service = VirtualSignalService()
        session = self._make_session()
        session = service.create(session, "v1", "a & b")

        session = service.update(session, "v1", expression="a | b")
        info = service.get_info("v1", session)
        self.assertEqual(info["expression"], "a | b")

    def test_update_description(self):
        service = VirtualSignalService()
        session = self._make_session()
        session = service.create(session, "v1", "a & b")

        session = service.update(session, "v1", description="updated desc")
        info = service.get_info("v1", session)
        self.assertEqual(info["description"], "updated desc")

    def test_delete(self):
        service = VirtualSignalService()
        session = self._make_session()
        session = service.create(session, "v1", "a & b")
        self.assertIn("v1", session["created_signals"])

        session = service.delete(session, "v1")
        self.assertNotIn("v1", session["created_signals"])

    def test_delete_nonexistent(self):
        service = VirtualSignalService()
        session = self._make_session()
        with self.assertRaises(ValueError):
            service.delete(session, "nonexistent")

    def test_is_virtual(self):
        service = VirtualSignalService()
        session = self._make_session()
        self.assertFalse(service.is_virtual("v1", session))
        session = service.create(session, "v1", "a & b")
        self.assertTrue(service.is_virtual("v1", session))

    def test_dependencies(self):
        service = VirtualSignalService()
        session = self._make_session()
        session = service.create(session, "v1", "a & b")
        info = service.get_info("v1", session)
        self.assertIn("a", info["dependencies"])
        self.assertIn("b", info["dependencies"])


class TestSessionPersistence(_TestBase):
    """Verify created_signals survive session save/load."""

    def test_persist_and_reload(self):
        service = VirtualSignalService()
        session = self._make_session("persist_test")
        session = service.create(session, "v1", "a & b", "test signal")
        saved = _save_session_payload(session)

        # Reload from the same session file
        reloaded = _normalize_session_payload(saved)
        self.assertIn("v1", reloaded["created_signals"])
        self.assertEqual(reloaded["created_signals"]["v1"]["expression"], "a & b")


class TestChainedDependencies(_TestBase):

    def test_chained_create(self):
        service = VirtualSignalService()
        session = self._make_session()

        session = service.create(session, "v1", "a & b")
        session = service.create(session, "v2", "v1 | a")
        virtuals = service.list(session)
        self.assertEqual(len(virtuals), 2)

    def test_chained_cycle_rejected(self):
        service = VirtualSignalService()
        session = self._make_session()

        session = service.create(session, "v1", "a & b")
        with self.assertRaises(ValueError) as ctx:
            service.create(session, "v2", "v1 & v2")  # self-reference
        self.assertIn("circular", str(ctx.exception).lower())

    def test_update_invalidates_dependents(self):
        service = VirtualSignalService()
        session = self._make_session()

        session = service.create(session, "v1", "a & b")
        session = service.create(session, "v2", "v1 | a")

        # Update v1 should succeed (cache invalidation happens internally)
        session = service.update(session, "v1", expression="a | b")
        info = service.get_info("v1", session)
        self.assertEqual(info["expression"], "a | b")


class TestExpressionValidation(_TestBase):

    def test_complex_expression(self):
        service = VirtualSignalService()
        session = self._make_session()
        session = service.create(session, "complex", "((^(a & b)) << 3)")
        info = service.get_info("complex", session)
        self.assertIn("a", info["dependencies"])
        self.assertIn("b", info["dependencies"])

    def test_ternary_expression(self):
        service = VirtualSignalService()
        session = self._make_session()
        session = service.create(session, "mux", "a ? b : bus")
        info = service.get_info("mux", session)
        self.assertEqual(len(info["dependencies"]), 3)

    def test_literal_expression(self):
        service = VirtualSignalService()
        session = self._make_session()
        session = service.create(session, "masked", "a & 4'b1010")
        info = service.get_info("masked", session)
        self.assertIn("a", info["dependencies"])

    def test_all_binary_ops_create(self):
        """Verify all supported binary operators can create signals."""
        service = VirtualSignalService()
        session = self._make_session()
        ops = [
            ("band", "a & b"), ("bor", "a | b"), ("bxor", "a ^ b"),
            ("bnand", "a ~& b"), ("bnor", "a ~| b"), ("bxnor", "a ~^ b"),
            ("land", "a && b"), ("lor", "a || b"),
            ("eq", "a == b"), ("ne", "a != b"),
            ("lt", "a < b"), ("gt", "a > b"), ("le", "a <= b"), ("ge", "a >= b"),
            ("shl", "a << 2"), ("shr", "a >> 1"),
            ("ashl", "a <<< 2"), ("ashr", "a >>> 1"),
            ("add", "a + b"), ("sub", "a - b"),
            ("mul", "a * 2"), ("div", "a / 2"), ("mod", "a % 2"),
            ("pow", "a ** 2"),
        ]
        for name, expr in ops:
            session = service.create(session, name, expr)
        virtuals = service.list(session)
        self.assertEqual(len(virtuals), len(ops))

    def test_all_unary_ops_create(self):
        """Verify all supported unary operators can create signals."""
        service = VirtualSignalService()
        session = self._make_session()
        ops = [
            ("not", "~a"), ("lnot", "!a"),
            ("red_and", "&a"), ("red_or", "|a"), ("red_xor", "^a"),
            ("red_nand", "~&a"), ("red_nor", "~|a"), ("red_xnor", "~^a"),
        ]
        for name, expr in ops:
            session = service.create(session, name, expr)
        virtuals = service.list(session)
        self.assertEqual(len(virtuals), len(ops))


class TestVirtualSignalEvaluation(_TestBase):

    def test_eval_after_operand_transition_uses_raw_seed(self):
        service = VirtualSignalService()
        session = self._make_session()
        session = service.create(session, "and_ab", "a & b")

        self.assertEqual(service.eval_at_time("and_ab", session, 21), "1")
        self.assertEqual(
            list(service.iter_transitions("and_ab", session, 21, 22)),
            [{"t": 21, "v": "1"}],
        )

    def test_multibit_passthrough_preserves_width_after_transition(self):
        service = VirtualSignalService()
        session = self._make_session()
        session = service.create(session, "bus_passthru", "bus")

        self.assertEqual(service.eval_at_time("bus_passthru", session, 21), "b1010")


if __name__ == "__main__":
    unittest.main()
