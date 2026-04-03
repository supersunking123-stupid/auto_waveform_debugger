"""Session, Cursor, Bookmark, SignalGroup persistence."""

import hashlib
import json
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

from .models import (
    ACTIVE_SESSION_FILE,
    DEFAULT_SESSION_NAME,
    SESSION_STORE_DIR,
    TimeReference,
)
from .mapping import _normalize_waveform_path


def _utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _ensure_session_store() -> None:
    SESSION_STORE_DIR.mkdir(parents=True, exist_ok=True)


def _session_waveform_id(waveform_path: str) -> str:
    normalized = _normalize_waveform_path(waveform_path)
    return hashlib.sha1(normalized.encode("utf-8")).hexdigest()[:16]


def _safe_session_name(session_name: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", session_name.strip())
    return cleaned or "session"


def _session_file_path(waveform_path: str, session_name: str) -> Path:
    prefix = _session_waveform_id(waveform_path)
    return SESSION_STORE_DIR / f"{prefix}__{_safe_session_name(session_name)}.json"


def _read_json_file(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def _write_json_file(path: Path, payload: Dict[str, Any]) -> None:
    tmp_path = path.with_suffix(path.suffix + ".tmp")
    with tmp_path.open("w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2, sort_keys=True, ensure_ascii=False)
        f.write("\n")
    tmp_path.replace(path)


def _validate_session_name(session_name: str) -> str:
    name = session_name.strip()
    if not name:
        raise ValueError("session_name must not be empty")
    return name


def _validate_named_entity(name: str, field_name: str) -> str:
    value = name.strip()
    if not value:
        raise ValueError(f"{field_name} must not be empty")
    if value == "Cursor":
        raise ValueError(f"{field_name} cannot be Cursor")
    return value


def _default_session_payload(
    waveform_path: str,
    session_name: str = DEFAULT_SESSION_NAME,
    description: str = "",
) -> Dict[str, Any]:
    normalized_waveform = _normalize_waveform_path(waveform_path)
    now = _utc_now_iso()
    return {
        "session_name": _validate_session_name(session_name),
        "description": description,
        "waveform_path": normalized_waveform,
        "cursor_time": 0,
        "bookmarks": {},
        "signal_groups": {},
        "created_signals": {},
        "created_at": now,
        "updated_at": now,
    }


def _normalize_session_payload(payload: Dict[str, Any]) -> Dict[str, Any]:
    normalized = dict(payload)
    normalized["session_name"] = _validate_session_name(str(normalized.get("session_name", "")))
    normalized["waveform_path"] = _normalize_waveform_path(str(normalized.get("waveform_path", "")))
    normalized["description"] = str(normalized.get("description", ""))
    normalized["cursor_time"] = int(normalized.get("cursor_time", 0))
    normalized["bookmarks"] = dict(normalized.get("bookmarks", {}))
    normalized["signal_groups"] = dict(normalized.get("signal_groups", {}))
    normalized["created_signals"] = dict(normalized.get("created_signals", {}))
    normalized["created_at"] = str(normalized.get("created_at", _utc_now_iso()))
    normalized["updated_at"] = str(normalized.get("updated_at", normalized["created_at"]))
    return normalized


def _save_session_payload(payload: Dict[str, Any]) -> Dict[str, Any]:
    _ensure_session_store()
    normalized = _normalize_session_payload(payload)
    normalized["updated_at"] = _utc_now_iso()
    path = _session_file_path(normalized["waveform_path"], normalized["session_name"])
    _write_json_file(path, normalized)
    return normalized


def _load_session_payload(waveform_path: str, session_name: str) -> Optional[Dict[str, Any]]:
    path = _session_file_path(waveform_path, session_name)
    if not path.exists():
        return None
    return _normalize_session_payload(_read_json_file(path))


def _list_session_payloads(
    waveform_path: Optional[str] = None,
    diagnostics: Optional[List[Dict[str, str]]] = None,
) -> List[Dict[str, Any]]:
    _ensure_session_store()
    normalized_waveform = None if waveform_path is None else _normalize_waveform_path(waveform_path)
    sessions: List[Dict[str, Any]] = []
    for path in SESSION_STORE_DIR.glob("*.json"):
        if path == ACTIVE_SESSION_FILE:
            continue
        try:
            payload = _normalize_session_payload(_read_json_file(path))
        except Exception as exc:
            if diagnostics is not None:
                diagnostics.append({"path": str(path), "message": str(exc)})
            continue
        if normalized_waveform is not None and payload["waveform_path"] != normalized_waveform:
            continue
        sessions.append(payload)
    sessions.sort(key=lambda item: (item["waveform_path"], item["session_name"]))
    return sessions


def _get_active_session_ref() -> Optional[Dict[str, str]]:
    if not ACTIVE_SESSION_FILE.exists():
        return None
    try:
        payload = _read_json_file(ACTIVE_SESSION_FILE)
    except Exception:
        return None
    session_name = str(payload.get("session_name", "")).strip()
    waveform_path = str(payload.get("waveform_path", "")).strip()
    if not session_name or not waveform_path:
        return None
    return {
        "session_name": session_name,
        "waveform_path": _normalize_waveform_path(waveform_path),
    }


def _set_active_session_ref(waveform_path: str, session_name: str) -> Dict[str, str]:
    _ensure_session_store()
    payload = {
        "session_name": _validate_session_name(session_name),
        "waveform_path": _normalize_waveform_path(waveform_path),
        "updated_at": _utc_now_iso(),
    }
    _write_json_file(ACTIVE_SESSION_FILE, payload)
    return payload


def _clear_active_session_ref() -> None:
    if ACTIVE_SESSION_FILE.exists():
        ACTIVE_SESSION_FILE.unlink()


def _get_or_create_session(
    waveform_path: str,
    session_name: str = DEFAULT_SESSION_NAME,
    description: str = "",
) -> Dict[str, Any]:
    normalized_waveform = _normalize_waveform_path(waveform_path)
    existing = _load_session_payload(normalized_waveform, session_name)
    if existing is not None:
        return existing
    return _save_session_payload(_default_session_payload(normalized_waveform, session_name=session_name, description=description))


def _resolve_session(
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
    create_default: bool = True,
    set_active_on_create: bool = True,
) -> Dict[str, Any]:
    active_ref = _get_active_session_ref()
    normalized_waveform = None if waveform_path is None else _normalize_waveform_path(waveform_path)

    if session_name:
        if normalized_waveform is None:
            if active_ref and active_ref["session_name"] == session_name:
                normalized_waveform = active_ref["waveform_path"]
            else:
                matches = [item for item in _list_session_payloads() if item["session_name"] == session_name]
                if not matches:
                    raise ValueError(f"session not found: {session_name}")
                if len(matches) > 1:
                    raise ValueError(f"session_name is ambiguous without waveform_path: {session_name}")
                normalized_waveform = matches[0]["waveform_path"]
        payload = _load_session_payload(normalized_waveform, session_name)
        if payload is None:
            raise ValueError(f"session not found: {session_name}")
        return payload

    if normalized_waveform is None:
        if not active_ref:
            raise ValueError("waveform_path or an active session is required")
        payload = _load_session_payload(active_ref["waveform_path"], active_ref["session_name"])
        if payload is None:
            raise ValueError("active session reference is stale")
        return payload

    if active_ref and active_ref["waveform_path"] == normalized_waveform:
        payload = _load_session_payload(normalized_waveform, active_ref["session_name"])
        if payload is not None:
            return payload

    if not create_default:
        raise ValueError(f"no active session for waveform: {normalized_waveform}")

    payload = _get_or_create_session(normalized_waveform, DEFAULT_SESSION_NAME)
    if set_active_on_create and not active_ref:
        _set_active_session_ref(normalized_waveform, payload["session_name"])
    return payload


def _session_summary(payload: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "session_name": payload["session_name"],
        "description": payload["description"],
        "waveform_path": payload["waveform_path"],
        "cursor_time": payload["cursor_time"],
        "bookmark_names": sorted(payload["bookmarks"].keys()),
        "signal_group_names": sorted(payload["signal_groups"].keys()),
        "created_signal_names": sorted(payload.get("created_signals", {}).keys()),
        "created_at": payload["created_at"],
        "updated_at": payload["updated_at"],
    }


def _time_resolution_info(input_value: TimeReference, resolved_time: int, session: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "input": input_value,
        "resolved_time": resolved_time,
        "session_name": session["session_name"],
        "waveform_path": session["waveform_path"],
    }


def _resolve_time_reference(
    time_value: TimeReference,
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
) -> Tuple[int, Dict[str, Any], Dict[str, Any]]:
    session = _resolve_session(waveform_path=waveform_path, session_name=session_name)
    if isinstance(time_value, int):
        return int(time_value), _time_resolution_info(time_value, int(time_value), session), session

    raw = str(time_value).strip()
    if not raw:
        raise ValueError("time reference must not be empty")
    if raw == "Cursor":
        resolved = int(session["cursor_time"])
        return resolved, _time_resolution_info(raw, resolved, session), session
    if raw.startswith("BM_"):
        bookmark_name = raw[3:]
        bookmark = session["bookmarks"].get(bookmark_name)
        if bookmark is None:
            raise ValueError(f"bookmark not found: {bookmark_name}")
        resolved = int(bookmark["time"])
        return resolved, _time_resolution_info(raw, resolved, session), session
    try:
        resolved = int(raw)
    except ValueError as exc:
        raise ValueError(f"unsupported time reference: {time_value}") from exc
    return resolved, _time_resolution_info(raw, resolved, session), session


def _resolve_time_range_reference(
    start_time: TimeReference,
    end_time: TimeReference,
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
) -> Tuple[int, int, Dict[str, Any], Dict[str, Any], Dict[str, Any]]:
    start_resolved, start_info, session = _resolve_time_reference(
        start_time,
        waveform_path=waveform_path,
        session_name=session_name,
    )
    end_resolved, end_info, _ = _resolve_time_reference(
        end_time,
        waveform_path=session["waveform_path"],
        session_name=session["session_name"],
    )
    return start_resolved, end_resolved, start_info, end_info, session


def _expand_signal_groups(
    signals: Sequence[str],
    signals_are_groups: bool,
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
) -> Tuple[List[str], Dict[str, Any], Optional[Dict[str, Any]]]:
    if not signals_are_groups:
        return list(signals), {"signals_are_groups": False, "group_names": [], "expanded_signals": list(signals)}, None

    session = _resolve_session(waveform_path=waveform_path, session_name=session_name)
    expanded: List[str] = []
    seen = set()
    group_names: List[str] = []
    for group_name in signals:
        group_key = _validate_named_entity(group_name, "group_name")
        group = session["signal_groups"].get(group_key)
        if group is None:
            raise ValueError(f"signal group not found: {group_key}")
        group_names.append(group_key)
        for signal in group.get("signals", []):
            if signal not in seen:
                seen.add(signal)
                expanded.append(signal)
    return expanded, {
        "signals_are_groups": True,
        "group_names": group_names,
        "expanded_signals": expanded,
    }, session


def _with_session_metadata(
    payload: Dict[str, Any],
    session: Optional[Dict[str, Any]] = None,
    time_context: Optional[Dict[str, Any]] = None,
    signal_context: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    if session is not None:
        payload["session"] = _session_summary(session)
    if time_context is not None:
        payload["resolved_time"] = time_context
    if signal_context is not None:
        payload["resolved_signals"] = signal_context
    return payload


def _require_waveform_path_from_session(
    waveform_path: Optional[str] = None,
    session_name: Optional[str] = None,
) -> Tuple[str, Dict[str, Any]]:
    session = _resolve_session(waveform_path=waveform_path, session_name=session_name)
    return session["waveform_path"], session
