"""agent_debug_automation — modular MCP toolkit for agent-guided RTL debug."""

# Import submodules in dependency order:
# 1. models (no deps)
# 2. mapping (depends on models)
# 3. clients (depends on models, mapping)
# 4. sessions (depends on models, mapping)
# 5. ranking (depends on models, late-imports clients/mapping)
# 6. expression_parser (no deps)
# 7. expression_evaluator (depends on expression_parser)
# 8. virtual_signals (depends on expression_parser, expression_evaluator, clients, sessions)
# 9. server (creates FastMCP instance)
# 10. tools (depends on everything, registers @mcp.tool handlers)
from . import models  # noqa: F401
from . import mapping  # noqa: F401
from . import clients  # noqa: F401
from . import sessions  # noqa: F401
from . import ranking  # noqa: F401
from . import expression_parser  # noqa: F401
from . import expression_evaluator  # noqa: F401
from . import virtual_signals  # noqa: F401
from . import server  # noqa: F401
from . import tools  # noqa: F401
