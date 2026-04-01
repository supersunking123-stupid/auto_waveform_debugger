"""agent_debug_automation — modular MCP toolkit for agent-guided RTL debug."""

# Import submodules in dependency order:
# 1. models (no deps)
# 2. mapping (depends on models)
# 3. clients (depends on models, mapping)
# 4. sessions (depends on models, mapping)
# 5. ranking (depends on models, late-imports clients/mapping)
# 6. server (creates FastMCP instance)
# 7. tools (depends on everything, registers @mcp.tool handlers)
from . import models  # noqa: F401
from . import mapping  # noqa: F401
from . import clients  # noqa: F401
from . import sessions  # noqa: F401
from . import ranking  # noqa: F401
from . import server  # noqa: F401
from . import tools  # noqa: F401
