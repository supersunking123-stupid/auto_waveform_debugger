# Playbook 05 — Session Management

**Role:** You are a workspace organizer. Your job is to set up, navigate, and maintain debugging sessions so that investigation state (cursors, bookmarks, signal groups) is organized and reusable.

**When to use:** Before starting a debug task (set up the workspace), during a task (save interesting findings), or when switching between different investigation contexts.

---

## Tools available in this playbook

### Session lifecycle

| Tool | Purpose |
|---|---|
| `create_session` | Create a new named session bound to a waveform file |
| `list_sessions` | List all sessions (optionally filtered by waveform) |
| `get_session` | Get details of a session (cursor, bookmarks, signal groups) |
| `switch_session` | Change the active session |
| `delete_session` | Remove a session |

### Cursor

| Tool | Purpose |
|---|---|
| `set_cursor` | Set the cursor to an absolute time or bookmark reference |
| `move_cursor` | Move the cursor by a relative delta |
| `get_cursor` | Read the current cursor position |

### Bookmarks

| Tool | Purpose |
|---|---|
| `create_bookmark` | Save a named time point |
| `delete_bookmark` | Remove a bookmark |
| `list_bookmarks` | List all bookmarks in the active session |

### Signal groups

| Tool | Purpose |
|---|---|
| `create_signal_group` | Save a named list of signals |
| `update_signal_group` | Modify an existing group's signal list or description |
| `delete_signal_group` | Remove a signal group |
| `list_signal_groups` | List all signal groups in the active session |

---

## Key concepts

**Session:** A saved workspace bound to one waveform file. Contains a cursor, bookmarks, and signal groups. Each waveform can have multiple sessions.

**Default_Session:** Created automatically on the first session-aware tool call for a waveform if no session is specified. You don't need to explicitly create it.

**Active session:** A global pointer. When session-aware tools omit `session_name`, the active session is used. When they also omit `vcd_path`, the active session also determines the waveform file.

---

## Common sequences

### Sequence A — Set up a fresh debug workspace

```python
# 1. Create a dedicated session for this investigation
create_session(
    waveform_path="sim_results/test_fail.fsdb",
    session_name="bug_123_investigation",
    description="Investigating FIFO overflow in test_fail"
)

# 2. Set the cursor to the region of interest
set_cursor(time=320000)

# 3. Create signal groups for signals you'll check repeatedly
create_signal_group(
    group_name="fifo_interface",
    signals=["top.u_fifo.wr_en", "top.u_fifo.rd_en", "top.u_fifo.data_in",
             "top.u_fifo.data_out", "top.u_fifo.count", "top.u_fifo.overflow"]
)

# 4. Bookmark the known failure point
create_bookmark(bookmark_name="overflow_event", time=320000,
                description="FIFO overflow asserted")
```

### Sequence B — Use bookmarks and cursor during investigation

```python
# Jump to a bookmark
set_cursor(time="BM_overflow_event")

# Read values at the cursor
get_snapshot(signals=["fifo_interface"], signals_are_groups=True, time="Cursor")

# Move backward by 1000 time units to check earlier state
move_cursor(delta=-1000)
get_snapshot(signals=["fifo_interface"], signals_are_groups=True, time="Cursor")

# Found something interesting — bookmark it
create_bookmark(bookmark_name="rd_en_dropped", time="Cursor",
                description="rd_en went low unexpectedly")
```

### Sequence C — Manage multiple investigations

```python
# List existing sessions
list_sessions()

# Switch to a different investigation
switch_session(session_name="bug_456_timeout")

# Check what bookmarks exist in this session
list_bookmarks()

# Come back to the original investigation
switch_session(session_name="bug_123_investigation")
```

### Sequence D — Evolve signal groups during investigation

```python
# After identifying suspects, create a focused group
create_signal_group(
    group_name="suspects",
    signals=["top.u_consumer.ready", "top.u_consumer.state", "top.u_arbiter.grant"],
    description="Signals suspected of causing FIFO overflow"
)

# Later, narrow it down
update_signal_group(
    group_name="suspects",
    signals=["top.u_consumer.state"],
    description="Confirmed: consumer STALL state is the root cause"
)

# Clean up groups no longer needed
delete_signal_group(group_name="suspects")
```

---

## When to create sessions vs. use Default_Session

| Scenario | Recommendation |
|---|---|
| Quick one-off question about a waveform | Let `Default_Session` be created automatically |
| Investigating a specific bug | Create a named session with a description |
| Comparing behavior across two waveforms | Create a session for each, switch between them |
| Multiple people debugging different issues on the same waveform | Create separate named sessions |

---

## Tips

- **Name sessions after bug IDs or investigation goals**, not waveform filenames. Example: `"bug_123_overflow"` not `"test_fail_session"`.
- **Bookmark liberally.** Bookmarks are cheap, and `"BM_<name>"` references are more readable than raw time values in subsequent tool calls.
- **Signal groups integrate with `get_snapshot`.** Use `signals_are_groups=True` to expand a group name into its full signal list automatically.
- **Cursor is global.** `set_cursor` changes the cursor for the active session. If you are switching between sessions, the cursor context switches with you.
- **Sessions are waveform-bound.** You cannot use a session created for `wave_a.fsdb` to query `wave_b.fsdb`. Create a separate session for each waveform file.
