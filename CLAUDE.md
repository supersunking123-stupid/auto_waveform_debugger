# auto_waveform_debugger — Agent Instructions

## Python Runtime

**Always use `.venv/bin/python3` for every Python command in this project.**

Never use bare `python3` or `python` — the system Python does not have the project dependencies installed.

```bash
# Correct
.venv/bin/python3 -m unittest waveform_explorer.tests.test_signal_overview
.venv/bin/python3 agent_debug_automation/agent_debug_automation_mcp.py

# Wrong — do not do this
python3 -m unittest ...
python ...
```

To run the MCP server:

```bash
cd agent_debug_automation && ../.venv/bin/python3 agent_debug_automation_mcp.py
```

## Standard Test Commands

```bash
# standalone_trace (C++ ctest)
cd standalone_trace && ctest --test-dir build --output-on-failure

# waveform_explorer Python tests
.venv/bin/python3 -m unittest waveform_explorer.tests.test_signal_overview
.venv/bin/python3 -m unittest waveform_explorer.tests.test_waveform_commands

# agent_debug_automation cross-link tests
.venv/bin/python3 -m unittest agent_debug_automation.tests.test_cross_linking
```

## Key References

- `docs/PROJ_DESC.md` — architecture overview
- `docs/MCP_SIGNATURES.md` — MCP tool API contract (treat as immutable during refactor)
- `agent_debug_textbook/CLAUDE.md` — mandatory instructions for RTL debug tasks
