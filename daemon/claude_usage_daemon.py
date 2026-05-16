#!/usr/bin/env python3
"""Claude Code usage tracker daemon — macOS edition.

Reads the Claude Code OAuth token (from the macOS Keychain on Darwin, or the
~/.claude/.credentials.json file as a fallback for Linux), polls Anthropic's
/v1/messages to get rate-limit headers, and forwards them — along with the
current session slug and the two most recent tasks — over BLE GATT to the
"Claude Controller" ESP32 peripheral.

Polling strategy
----------------
- Anthropic API: adaptive. 60s when util ≥ 60% or status != "allowed",
  300s otherwise. Stops entirely when no Claude Code session is "busy"
  (to avoid spending tokens just to observe self), and trivially also
  stops while BLE is disconnected (the loop is gated on is_connected).
- Local files (~/.claude/sessions, ~/.claude/tasks, and the per-session
  projects/<slug>/<id>/subagents directory): scanned every 2s while a
  busy session exists, 10s otherwise. Cheap file IO, payload is only
  written to BLE when its bytes actually change.

Splash bottom rows
------------------
The two task rows (t1/t2) show the running sub-agent fleet when any
sub-agent is active, falling back to the session's in-progress todos
otherwise. Claude Code keeps no running/finished flag on disk, so
"active" is inferred from the agent transcript's mtime freshness — this
tracks the CLI's FleetView closely for a glanceable desk display.

Install:
    pip3 install bleak

Run:
    python3 daemon/claude_usage_daemon.py

First run on a fresh Mac: macOS will pop a Keychain access dialog the moment
the daemon reads the OAuth token. Click "Always Allow" — that grants this
Python binary access to *only* the Claude Code-credentials Keychain item, not
the rest of your Keychain.
"""
from __future__ import annotations

import asyncio
import json
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from bleak import BleakClient, BleakScanner

DEVICE_NAME    = "Claude Controller"
SERVICE_UUID   = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID   = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID  = "4c41555a-4465-7669-6365-000000000004"

# Anthropic API poll cadence (adaptive)
POLL_HOT_S   = 60     # util ≥ 60% OR status != "allowed"
POLL_WARM_S  = 300    # default
# Local file scan cadence (sessions + tasks). BLE write only on diff.
SCAN_BUSY_S  = 2.0
SCAN_IDLE_S  = 10.0

KEYCHAIN_SERVICE = "Claude Code-credentials"
TOKEN_PATH = Path.home() / ".claude" / ".credentials.json"
CACHE_DIR  = Path.home() / "Library" / "Application Support" / "claude-usage-monitor"
CACHE_FILE = CACHE_DIR / "ble-address"

CLAUDE_DIR    = Path.home() / ".claude"
SESSIONS_DIR  = CLAUDE_DIR / "sessions"
TASKS_DIR     = CLAUDE_DIR / "tasks"
PROJECTS_DIR  = CLAUDE_DIR / "projects"

# A sub-agent counts as "active" if its transcript .jsonl was written
# within this window. There is no running/finished flag on disk, so
# freshness is the only cheap signal. Generous on purpose: an agent
# stuck in one long tool call (a build, a big grep) stays quiet for a
# while, and we don't want it flickering off the splash mid-run. The
# flip side — a genuinely finished agent lingers up to this long before
# dropping off. Tune here if that balance feels wrong.
AGENT_FRESH_S = 180

# Task-status glyphs. ASCII so the firmware can switch the splash task lines
# to the Anthropic Styrene font (which lacks dingbat coverage) without
# rendering boxes. "v" reads as a checkmark, "*" as activity, " " as idle.
STATUS_GLYPH = {
    "completed":   "v",
    "in_progress": "*",
    "pending":     " ",
}


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


# ---------------------------------------------------------------- token

def _walk_for_token(obj) -> str | None:
    if isinstance(obj, dict):
        if "accessToken" in obj and isinstance(obj["accessToken"], str):
            return obj["accessToken"]
        for v in obj.values():
            t = _walk_for_token(v)
            if t:
                return t
    elif isinstance(obj, list):
        for v in obj:
            t = _walk_for_token(v)
            if t:
                return t
    return None


def _read_keychain_blob() -> str | None:
    if sys.platform != "darwin":
        return None
    try:
        result = subprocess.run(
            ["security", "find-generic-password", "-s", KEYCHAIN_SERVICE, "-w"],
            capture_output=True, text=True, timeout=10,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip() or None


def read_token() -> str:
    blob = _read_keychain_blob()
    if blob:
        try:
            tok = _walk_for_token(json.loads(blob))
            if tok:
                return tok
        except json.JSONDecodeError:
            pass

    if TOKEN_PATH.exists():
        tok = _walk_for_token(json.loads(TOKEN_PATH.read_text()))
        if tok:
            return tok

    raise RuntimeError(
        f"Claude Code OAuth token not found. Looked in macOS Keychain "
        f"(service='{KEYCHAIN_SERVICE}') and {TOKEN_PATH}. Run `claude /login` "
        f"and grant Keychain access on first prompt."
    )


# ---------------------------------------------------------------- API poll

def poll_api(token: str) -> dict:
    body = json.dumps({
        "model":      "claude-haiku-4-5-20251001",
        "max_tokens": 1,
        "messages":   [{"role": "user", "content": "hi"}],
    }).encode()
    req = urllib.request.Request(
        "https://api.anthropic.com/v1/messages",
        data=body,
        headers={
            "Authorization":     f"Bearer {token}",
            "anthropic-version": "2023-06-01",
            "anthropic-beta":    "oauth-2025-04-20",
            "Content-Type":      "application/json",
            "User-Agent":        "claude-code/2.1.5",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return {k.lower(): v for k, v in resp.headers.items()}
    except urllib.error.HTTPError as e:
        return {k.lower(): v for k, v in (e.headers or {}).items()}


def parse_rate(headers: dict) -> dict:
    now = int(time.time())

    def fnum(name: str, default: float = 0.0) -> float:
        try:
            return float(headers.get(name, default))
        except (TypeError, ValueError):
            return default

    s5_util  = fnum("anthropic-ratelimit-unified-5h-utilization")
    s5_reset = fnum("anthropic-ratelimit-unified-5h-reset")
    s7_util  = fnum("anthropic-ratelimit-unified-7d-utilization")
    s7_reset = fnum("anthropic-ratelimit-unified-7d-reset")
    status   = headers.get("anthropic-ratelimit-unified-5h-status", "unknown")

    return {
        "s":  round(s5_util * 100),
        "sr": max(0, int((s5_reset - now) / 60)),
        "w":  round(s7_util * 100),
        "wr": max(0, int((s7_reset - now) / 60)),
        "st": status,
        "ok": True,
    }


# ---------------------------------------------------------------- session/task scan

def _load_json(p: Path) -> dict | None:
    try:
        return json.loads(p.read_text())
    except (json.JSONDecodeError, OSError):
        return None


def find_active_session() -> tuple[str | None, str, bool]:
    """Pick the session to display.

    Preference: most recently updated session whose status == "busy".
    Fallback: most recently updated session, period. (Use of "busy" tracks
    whether Claude Code is actively working — this is the daemon's idle
    signal for API polling.)

    Returns (sessionId, name, is_busy).
    """
    if not SESSIONS_DIR.exists():
        return None, "", False

    busy: list[dict] = []
    any_: list[dict] = []
    for f in SESSIONS_DIR.glob("*.json"):
        data = _load_json(f)
        if not data or data.get("kind") != "interactive":
            continue
        any_.append(data)
        if data.get("status") == "busy":
            busy.append(data)

    pool = busy or any_
    if not pool:
        return None, "", False
    pick = max(pool, key=lambda c: c.get("updatedAt", 0))
    return pick.get("sessionId"), pick.get("name") or "", bool(busy)


def read_recent_tasks(session_id: str | None, limit: int = 2) -> list[dict]:
    """Return up to `limit` most-recent IN-PROGRESS tasks, in start order
    (oldest first). completed/pending/deleted are excluded — the splash
    only surfaces what's currently being worked on."""
    if not session_id:
        return []
    dirp = TASKS_DIR / session_id
    if not dirp.exists():
        return []
    tasks: list[dict] = []
    for f in dirp.glob("*.json"):
        data = _load_json(f)
        if not data or data.get("status") != "in_progress":
            continue
        try:
            data["_id_int"] = int(data.get("id", "0"))
        except (TypeError, ValueError):
            data["_id_int"] = 0
        tasks.append(data)
    tasks.sort(key=lambda t: t["_id_int"], reverse=True)
    recent = tasks[:limit]
    recent.sort(key=lambda t: t["_id_int"])  # oldest of the slice on top
    return recent


def find_subagents_dir(session_id: str | None) -> Path | None:
    """Locate ~/.claude/projects/<slug>/<sessionId>/subagents.

    Claude Code derives <slug> from the session cwd with its own
    path-mangling rules; rather than re-implement (and risk drifting
    from) that, glob over the project dirs and match the sessionId UUID,
    which is globally unique. ~16 dirs, one stat each — trivially cheap."""
    if not session_id or not PROJECTS_DIR.exists():
        return None
    for proj in PROJECTS_DIR.iterdir():
        if not proj.is_dir():
            continue
        d = proj / session_id / "subagents"
        if d.is_dir():
            return d
    return None


def read_active_agents(session_id: str | None, now: float,
                       limit: int = 2) -> list[str]:
    """Display names of sub-agents whose transcript was touched within
    AGENT_FRESH_S, ordered freshest LAST.

    The last-element-is-freshest ordering mirrors read_recent_tasks: the
    splash assigns t1=oldest (dim row) and t2=newest (accent row), so the
    most recently active agent lands on the highlighted bottom row."""
    d = find_subagents_dir(session_id)
    if not d:
        return []
    found: list[tuple[float, str]] = []
    for meta in d.glob("agent-*.meta.json"):
        stem = meta.name[: -len(".meta.json")]
        try:
            mtime = (d / f"{stem}.jsonl").stat().st_mtime
        except OSError:
            continue
        if now - mtime > AGENT_FRESH_S:
            continue
        data = _load_json(meta) or {}
        name = (data.get("name") or data.get("agentType") or "").strip()
        if not name:
            continue
        found.append((mtime, name))
    found.sort(key=lambda x: x[0], reverse=True)  # freshest first
    recent = found[:limit]
    recent.sort(key=lambda x: x[0])                # freshest last
    return [name for _, name in recent]


def sanitize_for_display(s: str) -> str:
    """Allowed character classes the firmware can render: ASCII printable,
    Hangul syllables (U+AC00..U+D7A3), CJK punctuation, fullwidth ASCII,
    plus the spinner dingbats the bundled fonts include. Hangul rendering
    depends on the Pretendard fallback wired in firmware/src/font_styrene_*.c
    (.fallback pointer). When that fallback is absent, Hangul glyphs render
    as blank boxes — the daemon still ships them; the firmware draws what
    its fonts can."""
    if not s:
        return ""
    out_chars = []
    for c in s:
        cp = ord(c)
        if 0x20 <= cp <= 0x7E:                 out_chars.append(c)  # ASCII printable
        elif 0xAC00 <= cp <= 0xD7A3:           out_chars.append(c)  # Hangul Syllables
        elif 0x3000 <= cp <= 0x303F:           out_chars.append(c)  # CJK punctuation
        elif 0xFF01 <= cp <= 0xFF5E:           out_chars.append(c)  # fullwidth ASCII
        elif cp in (0x00B7, 0x2026, 0x2722, 0x2733, 0x2736, 0x273B, 0x273D):
            out_chars.append(c)
        else:
            out_chars.append("?")
    return "".join(out_chars)


def format_task_line(task: dict) -> str:
    glyph = STATUS_GLYPH.get(task.get("status"), " ")
    subj = sanitize_for_display(task.get("subject") or "")
    return f"{glyph} {subj}"


def format_agent_line(name: str) -> str:
    # An active agent is, by definition, in progress — reuse the
    # in_progress glyph so the fleet rows read consistently with the
    # task rows the firmware already knows how to render.
    return f"{STATUS_GLYPH['in_progress']} {sanitize_for_display(name)}"


def current_active_form(tasks: list[dict]) -> str:
    """activeForm of the most recently started in-progress task (highest ID).
    That matches the visual bottom row of the splash task list, so the
    usage screen's spinner reads as 'what I just started doing'."""
    candidates = [t for t in tasks if t.get("status") == "in_progress"]
    if not candidates:
        return ""
    candidates.sort(key=lambda t: t["_id_int"], reverse=True)
    t = candidates[0]
    txt = (t.get("activeForm") or t.get("subject") or "").strip()
    return sanitize_for_display(txt)


# ---------------------------------------------------------------- payload

def build_payload(rate: dict, session_name: str, tasks: list[dict],
                  agents: list[str], is_busy: bool) -> bytes:
    p = dict(rate)
    # `idle` is the splash/rabbit sleep gate. find_active_session() falls back
    # to the most recently updated session even when none is busy, so without
    # this flag the firmware would keep showing the stale session name + a
    # non-sleeping rabbit after the user closes Claude Code.
    p["idle"] = not is_busy
    # When no busy session, blank everything so the splash goes properly quiet
    # (sleeping rabbit, no stale label below).
    p["sn"] = sanitize_for_display(session_name) if is_busy else ""
    # Fleet beats todos: if any sub-agent is running, the two rows are
    # "what my agents are doing" — that's the more useful glance than a
    # stale todo list while work is delegated out. Falls back to the
    # in-progress task lines when no agent is active.
    if is_busy and agents:
        p["t1"] = format_agent_line(agents[0]) if len(agents) >= 1 else ""
        p["t2"] = format_agent_line(agents[1]) if len(agents) >= 2 else ""
    else:
        p["t1"] = format_task_line(tasks[0]) if (is_busy and len(tasks) >= 1) else ""
        p["t2"] = format_task_line(tasks[1]) if (is_busy and len(tasks) >= 2) else ""
    # Usage spinner: prefer the latest in-progress task's activeForm; then
    # the freshest running agent's name (so it isn't a bare "Working"
    # while agents are clearly busy); then a generic "Working" so the
    # device still shows liveness. CC's own CLI spinner phrases
    # ("Spelunking…", etc.) are ephemeral and never hit disk, so the
    # daemon can't mirror them verbatim.
    active = current_active_form(tasks) if is_busy else ""
    if is_busy and not active and agents:
        active = sanitize_for_display(agents[-1])
    if is_busy and not active:
        active = "Working"
    p["ta"] = active
    return json.dumps(p, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def current_interval(rate: dict) -> int:
    """Hot when busy near limits or status flagged; warm otherwise."""
    s = rate.get("s", 0) or 0
    w = rate.get("w", 0) or 0
    st = rate.get("st", "allowed") or "allowed"
    if s >= 60 or w >= 60 or st != "allowed":
        return POLL_HOT_S
    return POLL_WARM_S


# ---------------------------------------------------------------- discovery

async def discover() -> str | None:
    if CACHE_FILE.exists():
        addr = CACHE_FILE.read_text().strip()
        if addr:
            log(f"Using cached address: {addr}")
            return addr

    log(f"Scanning for '{DEVICE_NAME}' (8s)...")
    devices = await BleakScanner.discover(timeout=8.0)
    for d in devices:
        if d.name == DEVICE_NAME:
            CACHE_DIR.mkdir(parents=True, exist_ok=True)
            CACHE_FILE.write_text(d.address)
            log(f"Found: {d.address}")
            return d.address
    return None


def drop_cache() -> None:
    if CACHE_FILE.exists():
        CACHE_FILE.unlink()


# ---------------------------------------------------------------- session loop

async def session(addr: str) -> None:
    refresh = asyncio.Event()

    def on_req(_handle, data: bytearray) -> None:
        if data and data[0] == 0x01:
            log("Refresh requested by device")
            refresh.set()

    async with BleakClient(addr) as client:
        if not client.is_connected:
            log("Failed to connect")
            drop_cache()
            return
        log(f"Connected to {addr}")

        await asyncio.sleep(0.5)
        try:
            await client.start_notify(REQ_CHAR_UUID, on_req)
        except Exception as e:
            log(f"REQ subscribe skipped: {e}")

        rate: dict = {"s": 0, "sr": 0, "w": 0, "wr": 0, "st": "unknown", "ok": False}
        last_payload = b""
        last_poll_ts = 0.0
        is_busy = False
        first_send = True

        while client.is_connected:
            now = time.time()

            # Local snapshot: cheap, always.
            session_id, session_name, busy_now = find_active_session()
            tasks = read_recent_tasks(session_id)
            agents = read_active_agents(session_id, now)
            is_busy = busy_now

            # API poll: only when busy and interval elapsed (or refresh asked).
            should_poll = False
            if refresh.is_set():
                refresh.clear()
                should_poll = True
            elif is_busy and (now - last_poll_ts) >= current_interval(rate):
                should_poll = True

            if should_poll:
                try:
                    token = read_token()
                    headers = poll_api(token)
                    rate = parse_rate(headers)
                    last_poll_ts = now
                except Exception as e:
                    log(f"Poll error: {e}")

            # Build + write only on change.
            payload = build_payload(rate, session_name, tasks, agents, busy_now)
            if payload != last_payload or first_send:
                try:
                    await client.write_gatt_char(RX_CHAR_UUID, payload, response=True)
                    last_payload = payload
                    first_send = False
                    log(f"Sent: {payload.decode('utf-8', errors='replace')[:140]}")
                except Exception as e:
                    log(f"Write error: {e}")

            # Sleep until next scan tick or a refresh interrupt.
            tick = SCAN_BUSY_S if is_busy else SCAN_IDLE_S
            try:
                await asyncio.wait_for(refresh.wait(), timeout=tick)
            except asyncio.TimeoutError:
                pass

        log("Disconnected")


async def main() -> None:
    log("=== Claude Usage Daemon (macOS, adaptive) ===")
    backoff = 2
    while True:
        addr = await discover()
        if not addr:
            log(f"Device not found, retry in {backoff}s")
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 60)
            continue
        try:
            await session(addr)
            backoff = 2
        except Exception as e:
            log(f"Session error: {e}")
            drop_cache()
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 60)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log("Stopped")
        sys.exit(0)
