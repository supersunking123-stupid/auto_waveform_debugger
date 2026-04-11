"""Shared data models, type aliases, enums, and constants."""

from pathlib import Path
from typing import List, Literal, Optional, TypedDict

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

ROOT_DIR = Path(__file__).resolve().parent.parent
DEFAULT_RTL_TRACE_BIN = ROOT_DIR / "standalone_trace" / "build" / "rtl_trace"
DEFAULT_WAVE_CLI = ROOT_DIR / "waveform_explorer" / "build" / "wave_agent_cli"

# ---------------------------------------------------------------------------
# Numeric defaults
# ---------------------------------------------------------------------------

DEFAULT_RANK_WINDOW_BEFORE = 1000
DEFAULT_RANK_WINDOW_AFTER = 1000
DEFAULT_EXPLAIN_WINDOW_AFTER = 0
DEFAULT_TRANSITION_LIMIT = 256
DEFAULT_VIRTUAL_LEAF_MAX_LIMIT = 10000
DEFAULT_BACKEND_READ_TIMEOUT_SEC = 300
MAX_VIRTUAL_SIGNAL_DEPTH = 16
MIN_CLOCK_LIKE_TRANSITIONS = 5

# ---------------------------------------------------------------------------
# Session store paths
# ---------------------------------------------------------------------------

SESSION_STORE_DIR = ROOT_DIR / "agent_debug_automation" / ".session_store"
ACTIVE_SESSION_FILE = SESSION_STORE_DIR / "active_session.json"
DEFAULT_SESSION_NAME = "Default_Session"

# ---------------------------------------------------------------------------
# Type aliases & TypedDicts
# ---------------------------------------------------------------------------


class TraceOptions(TypedDict, total=False):
    cone_level: int
    depth: int
    max_nodes: int
    include: str | List[str]
    exclude: str | List[str]
    stop_at: str | List[str]
    prefer_port_hop: bool


EdgeType = Literal["posedge", "negedge", "anyedge"]
EdgeTypeAlias = Literal["rise", "rising", "risingedge", "fall", "falling", "fallingedge", "edge", "any"]
Direction = Literal["forward", "backward"]
BoundaryPolicy = Literal["inclusive", "exclusive"]
TraceMode = Literal["drivers", "loads"]
TimeReference = int | str
ResolutionReference = int | str


def normalize_virtual_leaf_max_limit(max_limit: Optional[int]) -> int:
    """Normalize optional virtual leaf limits and reject non-positive values."""
    if max_limit is None:
        return DEFAULT_VIRTUAL_LEAF_MAX_LIMIT
    try:
        limit = int(max_limit)
    except (TypeError, ValueError) as exc:
        raise ValueError("virtual_leaf_max_limit must be an integer > 0") from exc
    if limit <= 0:
        raise ValueError("virtual_leaf_max_limit must be > 0")
    return limit
