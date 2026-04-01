"""FastMCP app init."""

try:
    from mcp.server.fastmcp import FastMCP
except ImportError:
    class FastMCP:  # type: ignore[override]
        def __init__(self, name: str):
            self.name = name

        def tool(self):
            def decorator(func):
                return func

            return decorator

        def run(self):
            raise RuntimeError("mcp.server.fastmcp is not installed")


mcp = FastMCP("Agent Debug Automation")
