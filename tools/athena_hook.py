#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Local Athena hook: poll MiniHub, fix in a worktree, reply with the MR URL.

Athena has no outbound webhook. This process polls the MiniHub project and
POSTs new tasks and comments to ``http://127.0.0.1:8766/hook``. One job runs
at a time. The fixer checks out ``origin/master`` in a separate git worktree,
asks the Cursor Python SDK to edit that tree, then commits, pushes, and opens
a GitLab merge request. The MR's full URL is posted back as an Athena comment.

Secrets stay in the environment: an existing ``athena login`` session and
``CURSOR_API_KEY``. Push and the merge request use the repo's SSH remote.
Seen node ids are stored in
``.agent/private/athena_hook_state.json``, which is not committed.

Usage:
  python3 tools/athena_hook.py              # hook, worker, and poller
  python3 tools/athena_hook.py --hook-only  # accept POST /hook, do not fix
  python3 tools/athena_hook.py --once       # one poll; first run only baselines
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import queue
import re
import subprocess
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

log = logging.getLogger("athena_hook")

REPO = Path(__file__).resolve().parents[1]
STATE_PATH = REPO / ".agent" / "private" / "athena_hook_state.json"
PROJECT_NAME = "MiniHub"
MARKER = "athena-hook"
HOST = "127.0.0.1"
DEFAULT_PORT = 8766
DEFAULT_INTERVAL = 60
GITLAB_HOST = "https://gitlab.espressif.cn:6688"
GITLAB_PROJECT = "ae_group/minihub/esp-minihub"
TARGET_BRANCH = "master"
MR_URL_RE = re.compile(r"https://\S+/-/merge_requests/(\d+)")
DENY_NAMES = {".env", "credentials.json", "id_rsa", "id_ed25519"}
CURSOR_TRAILER_MARKS = (
    "Co-authored-by: Cursor",
    "Co-authored-by: cursoragent@cursor.com",
    "Made-with: Cursor",
)

state_lock = threading.Lock()


class App:
    """Process-wide hook queue. One worker drains it."""

    def __init__(self) -> None:
        self.queue: queue.Queue[dict] = queue.Queue()
        self.queued_ids: set[str] = set()
        self.running: str | None = None
        self.persist_seen = True
        self.stop = threading.Event()
        self.port = DEFAULT_PORT
        self.interval = DEFAULT_INTERVAL


app = App()


def load_state() -> dict:
    if not STATE_PATH.exists():
        return {"baselined": False, "seen": []}
    with STATE_PATH.open(encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        return {"baselined": False, "seen": []}
    seen = data.get("seen")
    data["seen"] = [str(item) for item in seen] if isinstance(seen, list) else []
    data["baselined"] = bool(data.get("baselined"))
    return data


def save_state(data: dict) -> None:
    STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
    tmp = STATE_PATH.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    tmp.replace(STATE_PATH)


def remember(node_id: str) -> None:
    """Record a node id so a later poll does not enqueue it again."""
    if not app.persist_seen or not node_id:
        return
    with state_lock:
        state = load_state()
        seen = set(state["seen"])
        if node_id in seen:
            return
        seen.add(node_id)
        state["seen"] = sorted(seen)
        save_state(state)


def already_seen(node_id: str) -> bool:
    if node_id in app.queued_ids or app.running == node_id:
        return True
    if not app.persist_seen:
        return False
    with state_lock:
        return node_id in set(load_state()["seen"])


def plain_text(value: object) -> str:
    """Turn a plain string or a BlockNote ``name`` into readable text."""
    if not value:
        return ""
    text = str(value)
    prefix = "<!--blocknote-->"
    if not text.startswith(prefix):
        return text.strip()
    try:
        data = json.loads(text[len(prefix):])
    except json.JSONDecodeError:
        return text.strip()
    parts: list[str] = []

    def walk(node: object) -> None:
        if isinstance(node, list):
            for item in node:
                walk(item)
            return
        if not isinstance(node, dict):
            return
        leaf = node.get("text")
        if isinstance(leaf, str) and "content" not in node:
            parts.append(leaf)
        if "content" in node:
            walk(node["content"])
        if "children" in node:
            walk(node["children"])

    walk(data)
    return "\n".join(part for part in parts if part).strip()


def contains_marker(*parts: object) -> bool:
    blob = "\n".join(str(part or "") for part in parts)
    return MARKER in blob


MR_REPLY_RE = re.compile(r"merge_requests/\d+")


def contains_mr_reply(*parts: object) -> bool:
    blob = "\n".join(str(part or "") for part in parts)
    return MR_REPLY_RE.search(blob) is not None


def node_has_mr_reply(node: object) -> bool:
    """True when this node or one of its comments already links a merge request."""
    if contains_mr_reply(getattr(node, "name", ""), getattr(node, "description", "")):
        return True
    for child in getattr(node, "children", None) or []:
        if contains_mr_reply(getattr(child, "name", ""), getattr(child, "description", "")):
            return True
    return False


SHORTLINK_RE = re.compile(r"\[~>([A-Za-z]+-\d+)\]")
BARE_SHORTLINK_RE = re.compile(r"[A-Za-z]+-\d+")
BUG_TITLE_HINTS = ("bug", "crash", "缺陷", "故障", "报错", "回归")


def parse_shortlink(value: str) -> str:
    """``[~>MHUB-71267]`` or ``MHUB-71267`` becomes ``MHUB-71267``."""
    text = (value or "").strip()
    wrapped = SHORTLINK_RE.fullmatch(text)
    if wrapped:
        return wrapped.group(1)
    if BARE_SHORTLINK_RE.fullmatch(text):
        return text
    return ""


def branch_kind(title: str, node_type: str) -> str:
    if (node_type or "").strip().lower() in {"bug", "defect"}:
        return "bugfix"
    text = (title or "").lower()
    if any(hint in text for hint in BUG_TITLE_HINTS):
        return "bugfix"
    return "feature"


def branch_slug(node_id: str) -> str:
    """Fallback when Athena has no ``MHUB-NNNN`` shortlink."""
    slug = re.sub(r"[^A-Za-z0-9]", "", str(node_id)).lower()[:12] or "job"
    return f"feature/athena_{slug}"


def branch_name_for(shortlink: str, title: str, node_type: str, node_id: str) -> str:
    """``feature/mhub_71309``. The hyphen form ``MHUB-71309`` is a Jira key.

    GitLab then writes ``Closes MHUB-71309`` on the merge request. That
    closes a Jira issue, not the Athena shortlink.
    """
    code = parse_shortlink(shortlink)
    if not code:
        return branch_slug(node_id)
    safe = code.replace("-", "_").lower()
    return f"{branch_kind(title, node_type)}/{safe}"


def block_id() -> str:
    return uuid.uuid4().hex[:12]


def text_run(text: str) -> dict:
    return {"type": "text", "text": text, "styles": {}}


def link_run(href: str) -> dict:
    return {"type": "link", "href": href, "content": [text_run(href)]}


def blocknote_name(contents: list[list[dict]]) -> str:
    blocks = []
    for content in contents:
        blocks.append({
            "id": block_id(),
            "type": "paragraph",
            "props": {
                "textColor": "default",
                "backgroundColor": "default",
                "textAlignment": "left",
            },
            "content": content,
            "children": [],
        })
    return "<!--blocknote-->" + json.dumps(blocks, ensure_ascii=False)


def comment_name_with_mr(mr_url: str) -> str:
    return blocknote_name([[text_run(f"{MARKER} "), link_run(mr_url)]])


def comment_name_note(note: str) -> str:
    return blocknote_name([[text_run(f"{MARKER} {note}".strip())]])


def comment_name_analysis(decision: str, analysis: str) -> str:
    verdict = "结论：需要改代码。" if decision == "change" else "结论：不改代码，不建分支。"
    paragraphs = [[text_run(f"{MARKER} 分析。{verdict}")]]
    for block in re.split(r"\n\s*\n", analysis):
        line = " ".join(block.split())
        if line:
            paragraphs.append([text_run(line[:2000])])
    return blocknote_name(paragraphs[:8])


def redact(text: str) -> str:
    cleaned = text or ""
    cleaned = re.sub(r"glpat-[A-Za-z0-9\-_]+", "[redacted]", cleaned)
    cleaned = re.sub(r"cursor_[A-Za-z0-9]+", "[redacted]", cleaned)
    cleaned = re.sub(
        r"(?i)(private-token[\"']?\s*[:=]\s*)\S+",
        r"\1[redacted]",
        cleaned,
    )
    cleaned = re.sub(r"(?i)(GITLAB_TOKEN=)\S+", r"\1[redacted]", cleaned)
    cleaned = re.sub(r"(?i)(CURSOR_API_KEY=)\S+", r"\1[redacted]", cleaned)
    home = str(Path.home())
    if home:
        cleaned = cleaned.replace(home, "~")
    return cleaned[:500]


DECISION_RE = re.compile(r"^ATHENA_HOOK_DECISION:\s*(change|no_change)\s*$", re.IGNORECASE)


def issue_sections(title: str, body: str, reply: str) -> list[str]:
    sections = [
        "Issue title:",
        (title or "").strip()[:500],
        "",
        "Issue body:",
        (body or "").strip()[:16000],
    ]
    if reply.strip():
        sections.extend(["", "New reply that triggered this run:", reply.strip()[:8000]])
    return sections


def analyze_prompt(title: str, body: str, reply: str) -> str:
    return "\n".join([
        "You are analyzing a MiniHub issue. Do not modify any file.",
        "Do not commit. Do not push. Do not open a merge request.",
        "Write the analysis in Chinese: what is being asked, whether the repo needs a code change, and why.",
        "Do not invent a fix. A title with no description is not a code change.",
        "End with exactly one line, and no other text on that line:",
        "ATHENA_HOOK_DECISION: change",
        "or",
        "ATHENA_HOOK_DECISION: no_change",
        "",
        *issue_sections(title, body, reply),
    ])


def parse_analysis(text: str) -> tuple[str, str]:
    """Return ``change`` or ``no_change``, plus the analysis without the decision line."""
    decision = "no_change"
    kept: list[str] = []
    for line in (text or "").splitlines():
        match = DECISION_RE.match(line.strip())
        if match:
            decision = match.group(1).lower()
            continue
        kept.append(line)
    analysis = "\n".join(kept).strip()[:4000]
    if not analysis:
        analysis = "agent 没有给出分析。"
    return decision, analysis


def agent_prompt(title: str, body: str, reply: str) -> str:
    sections = [
        "You are fixing a MiniHub issue in this git worktree.",
        "Follow AGENTS.md and the matching skill under .agent/skills/ before editing.",
        "Edit a file only when the issue text describes that specific defect or code change.",
        "If the text does not name a change, do not modify any file.",
        "Do not invent a fix, a cleanup, or a wording change.",
        "Do not commit. Do not push. Do not open a merge request.",
        "Do not add Co-authored-by, Made-with, or any Cursor trailer.",
        "Do not write Wi-Fi passwords, tokens, or other secrets into the tree.",
        "",
        *issue_sections(title, body, reply),
        "",
        "If you edit code, leave the worktree dirty for the caller to commit.",
    ]
    return "\n".join(sections)


def commit_subject(title: str) -> str:
    line = " ".join((title or "athena issue").split())[:72] or "athena issue"
    if re.match(r"^(fix|feat|chore|docs|refactor|test)\b", line):
        return line
    return f"fix: {line}"


def mr_url_from_iid(iid: int | str) -> str:
    return f"{GITLAB_HOST}/{GITLAB_PROJECT}/-/merge_requests/{iid}"


class HookHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args: object) -> None:
        log.info("%s %s", self.address_string(), fmt % args)

    def do_GET(self) -> None:  # noqa: N802
        path = urllib.parse.urlsplit(self.path).path
        if path == "/health":
            self._send(200, {"ok": True})
            return
        if path == "/queue":
            self._send(200, {
                "queued": list(app.queued_ids),
                "running": app.running,
                "depth": app.queue.qsize(),
            })
            return
        self._send(404, {"error": "not found"})

    def do_POST(self) -> None:  # noqa: N802
        path = urllib.parse.urlsplit(self.path).path
        if path != "/hook":
            self._send(404, {"error": "not found"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._send(400, {"error": "bad content length"})
            return
        if length <= 0 or length > 1_000_000:
            self._send(400, {"error": "body is empty or too large"})
            return
        try:
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
        except (UnicodeError, json.JSONDecodeError):
            self._send(400, {"error": "body is not json"})
            return
        if not isinstance(payload, dict):
            self._send(400, {"error": "body must be an object"})
            return
        if isinstance(payload.get("database_path"), str):
            payload["database_path"] = normalize_db_path(payload["database_path"])
        error = validate_payload(payload)
        if error:
            log.info("rejected %s: %s", payload.get("node_id"), error)
            self._send(400, {"error": error})
            return
        node_id = str(payload["node_id"])
        if contains_marker(payload.get("title"), payload.get("body"), payload.get("reply")):
            remember(node_id)
            self._send(200, {"status": "skipped", "reason": "self", "node_id": node_id})
            return
        if already_seen(node_id):
            self._send(200, {"status": "duplicate", "node_id": node_id})
            return
        payload["node_id"] = node_id
        payload["issue_id"] = str(payload.get("issue_id") or node_id)
        payload["project_id"] = str(payload["project_id"])
        payload["database_path"] = str(payload["database_path"])
        payload["title"] = str(payload.get("title") or "")
        payload["body"] = str(payload.get("body") or "")
        payload["reply"] = str(payload.get("reply") or "")
        app.queued_ids.add(node_id)
        app.queue.put(payload)
        remember(node_id)
        log.info("queued %s %s", payload["kind"], node_id)
        self._send(202, {"status": "queued", "node_id": node_id})

    def _send(self, status: int, body: dict) -> None:
        raw = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)


def normalize_db_path(path: object) -> str:
    """Athena echoes ``_db_path`` as a server filesystem path.

    Callers pass the relative form ``project/proj_<uuid>.db``. Keep that
    suffix when the API returns ``/home/.../project/proj_<uuid>.db``.
    """
    text = str(path or "").strip()
    marker = "project/proj_"
    index = text.find(marker)
    if index >= 0:
        return text[index:]
    return text


def validate_payload(payload: dict) -> str:
    kind = payload.get("kind")
    if kind not in ("task", "comment"):
        return "kind must be task or comment"
    for field in ("node_id", "project_id", "database_path"):
        value = payload.get(field)
        if not isinstance(value, str) or not value.strip():
            return f"{field} is required"
    database_path = str(payload["database_path"])
    if database_path.startswith("/") or ".." in database_path:
        return "database_path is not a project database path"
    return ""


def ensure_athena_python() -> None:
    """Re-exec with the interpreter that ships the athena CLI, if needed.

    The IDF virtualenv and Homebrew can share one Python binary via symlinks
    while keeping different ``sys.path`` values. Compare the path we were
    started with, not its resolved target, so the re-exec still happens.
    """
    try:
        import athena  # noqa: F401
        return
    except ImportError:
        pass
    if os.environ.get("ATHENA_HOOK_PY") == "1":
        sys.exit("athena client is not importable")
    import shutil
    athena_bin = shutil.which("athena")
    if not athena_bin:
        sys.exit("athena client is not installed")
    first = Path(athena_bin).read_text(encoding="utf-8").splitlines()[0]
    if not first.startswith("#!"):
        sys.exit("athena client has no interpreter shebang")
    python = first[2:].strip()
    os.environ["ATHENA_HOOK_PY"] = "1"
    os.execv(python, [python, *sys.argv])


def git(args: list[str], cwd: Path) -> str:
    log.info("git %s", " ".join(args))
    proc = subprocess.run(
        ["git", *args],
        cwd=cwd,
        text=True,
        capture_output=True,
        env=git_env(),
        check=False,
    )
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout or f"git {args[0]} failed").strip()
        raise RuntimeError(redact(detail))
    return proc.stdout


def git_env() -> dict[str, str]:
    env = os.environ.copy()
    env["GIT_TERMINAL_PROMPT"] = "0"
    return env


def ref_exists(ref: str) -> bool:
    proc = subprocess.run(
        ["git", "show-ref", "--verify", "--quiet", ref],
        cwd=REPO,
        capture_output=True,
        check=False,
    )
    return proc.returncode == 0


def allocate_worktree(base: str) -> tuple[str, Path]:
    for n in range(1, 51):
        branch = base if n == 1 else f"{base}_{n}"
        tail = branch.split("/", 1)[1]
        if not re.fullmatch(r"(?:athena_[a-z0-9]+|[a-z]+_\d+)(?:_\d+)?", tail):
            raise RuntimeError(f"branch name is not an Athena short id: {branch}")
        dest = REPO.parent / f"esp-minihub_{branch.replace('/', '_')}"
        local = f"refs/heads/{branch}"
        remote = f"refs/remotes/origin/{branch}"
        if ref_exists(local) or ref_exists(remote) or dest.exists():
            continue
        return branch, dest
    raise RuntimeError("could not allocate a free worktree branch")


def create_worktree(base: str) -> tuple[str, Path]:
    """Detach at ``origin/master``. The named branch is created only after a diff exists."""
    git(["fetch", "origin", TARGET_BRANCH], cwd=REPO)
    branch, dest = allocate_worktree(base)
    git(["worktree", "add", "--detach", str(dest), f"origin/{TARGET_BRANCH}"], cwd=REPO)
    log.info("detached worktree for %s at %s", branch, dest)
    return branch, dest


def reset_worktree(dest: Path) -> None:
    """Drop edits made while the agent was only supposed to analyze."""
    git(["checkout", "--", "."], cwd=dest)
    git(["clean", "-fd"], cwd=dest)


def checked_out_branch(dest: Path) -> str:
    name = git(["rev-parse", "--abbrev-ref", "HEAD"], cwd=dest).strip()
    return "" if name == "HEAD" else name


def discard_worktree(dest: Path, branch: str = "") -> None:
    if dest.exists():
        git(["worktree", "remove", "--force", str(dest)], cwd=REPO)
    if branch and ref_exists(f"refs/heads/{branch}"):
        git(["branch", "-D", branch], cwd=REPO)


def attach_branch(dest: Path, branch: str) -> None:
    current = checked_out_branch(dest)
    if current == branch:
        return
    if current:
        raise RuntimeError(f"worktree is already on {current}")
    git(["switch", "-c", branch], cwd=dest)
    log.info("created branch %s", branch)


def has_uncommitted(dest: Path) -> bool:
    return bool(git(["status", "--porcelain"], cwd=dest).strip())


def _git_quiet(dest: Path, args: list[str]) -> int:
    proc = subprocess.run(
        ["git", *args],
        cwd=dest,
        capture_output=True,
        check=False,
    )
    return proc.returncode


def has_fix(dest: Path) -> bool:
    """True when the worktree differs from ``origin/master`` by a code change."""
    if _git_quiet(dest, ["diff", "--quiet", f"origin/{TARGET_BRANCH}", "HEAD"]) == 1:
        return True
    if _git_quiet(dest, ["diff", "--cached", "--quiet"]) == 1:
        return True
    if _git_quiet(dest, ["diff", "--quiet"]) == 1:
        return True
    others = git(["ls-files", "--others", "--exclude-standard"], cwd=dest).splitlines()
    for name in others:
        base = Path(name).name
        if base in DENY_NAMES or name.endswith(".pem"):
            continue
        if name.strip():
            return True
    return False


def commits_ahead(dest: Path) -> int:
    out = git(["rev-list", "--count", f"origin/{TARGET_BRANCH}..HEAD"], cwd=dest).strip()
    return int(out or "0")


def stage_changes(dest: Path) -> bool:
    git(["add", "-A"], cwd=dest)
    names = [line for line in git(["diff", "--cached", "--name-only"], cwd=dest).splitlines() if line]
    for name in names:
        base = Path(name).name
        if base in DENY_NAMES or name.endswith(".pem"):
            git(["restore", "--staged", "--", name], cwd=dest)
            log.info("left unstaged %s", name)
    proc = subprocess.run(
        ["git", "diff", "--cached", "--quiet"],
        cwd=dest,
        capture_output=True,
        check=False,
    )
    return proc.returncode == 1


def strip_cursor_trailer(dest: Path) -> None:
    message = git(["log", "-1", "--format=%B"], cwd=dest)
    if not any(mark in message for mark in CURSOR_TRAILER_MARKS):
        return
    kept = [
        line for line in message.splitlines()
        if not any(mark in line for mark in CURSOR_TRAILER_MARKS)
    ]
    git(["commit", "--amend", "-m", "\n".join(kept).strip() + "\n"], cwd=dest)


def commit_worktree(dest: Path, title: str, issue_id: str) -> None:
    subject = commit_subject(title)
    message = f"{subject}\n\nAthena-Issue: {issue_id}\n"
    git(["commit", "-m", message], cwd=dest)
    strip_cursor_trailer(dest)


def mr_url_from_push_output(output: str) -> str:
    matches = MR_URL_RE.findall(output or "")
    if not matches:
        return ""
    return mr_url_from_iid(matches[-1])


def mr_description(athena_url: str) -> str:
    """The merge request description is only the Athena node URL."""
    url = (athena_url or "").strip()
    if not re.fullmatch(r"https://[^/\s]+/w/[A-Za-z0-9_-]+", url):
        raise RuntimeError("Athena node URL is missing")
    return url


def push_branch(dest: Path, branch: str, title: str, athena_url: str) -> str:
    """Push over the SSH remote and open the merge request in the same push."""
    subject = commit_subject(title)[:240]
    args = [
        "push", "-u", "origin", branch,
        "-o", "merge_request.create",
        "-o", f"merge_request.target={TARGET_BRANCH}",
        "-o", f"merge_request.title={subject}",
        "-o", f"merge_request.description={mr_description(athena_url)}",
    ]
    log.info("git %s", " ".join(args))
    proc = subprocess.run(
        ["git", *args],
        cwd=dest,
        text=True,
        capture_output=True,
        env=git_env(),
        check=False,
    )
    output = f"{proc.stderr}\n{proc.stdout}"
    if proc.returncode != 0:
        raise RuntimeError(redact(output.strip() or "git push failed"))
    url = mr_url_from_push_output(output)
    if not url:
        raise RuntimeError(redact(output.strip() or "push succeeded without a merge request URL"))
    return url


def run_agent(dest: Path, prompt: str) -> str:
    key = os.environ.get("CURSOR_API_KEY", "").strip()
    if not key:
        raise RuntimeError("CURSOR_API_KEY is not set")
    try:
        from cursor_sdk import Agent, AgentOptions, CursorAgentError, LocalAgentOptions
    except ImportError as exc:
        raise RuntimeError("cursor_sdk is not installed") from exc
    model = os.environ.get("ATHENA_HOOK_MODEL", "composer-2.5").strip() or "composer-2.5"
    try:
        result = Agent.prompt(
            prompt,
            AgentOptions(
                api_key=key,
                model=model,
                local=LocalAgentOptions(cwd=str(dest)),
            ),
        )
    except CursorAgentError as exc:
        raise RuntimeError(f"agent did not start: {exc}") from exc
    status = str(getattr(result, "status", "") or "")
    detail = str(getattr(result, "result", "") or "")
    try:
        from cursor_sdk import close_default_client
        close_default_client()
    except Exception:
        pass
    if status == "error":
        raise RuntimeError(f"agent run failed: {redact(detail)}")
    return detail


def handle_job(payload: dict) -> None:
    issue_id = str(payload["issue_id"])
    project_id = str(payload["project_id"])
    database_path = str(payload["database_path"])
    title = str(payload.get("title") or "")
    body = str(payload.get("body") or "")
    reply = str(payload.get("reply") or "")
    branch = ""
    dest: Path | None = None
    pushed = False
    mr_url = ""
    try:
        fresh = asyncio_fetch_issue(project_id, issue_id, database_path)
        title = fresh["title"] or title
        body = fresh["body"] or body
        database_path = fresh["database_path"] or database_path
        payload["title"] = title
        payload["body"] = body
        payload["database_path"] = database_path
        if fresh.get("has_mr"):
            log.info("issue %s already has an MR reply", issue_id)
            return
        branch, dest = create_worktree(branch_name_for(
            fresh.get("shortlink", ""),
            title,
            fresh.get("type", ""),
            issue_id,
        ))
        analysis_text = run_agent(dest, analyze_prompt(title, body, reply))
        decision, analysis = parse_analysis(analysis_text)
        post_analysis(payload, decision, analysis)
        if decision != "change":
            stray = checked_out_branch(dest)
            discard_worktree(dest, stray)
            dest = None
            branch = ""
            log.info("analysis decided no_change for %s", issue_id)
            return
        reset_worktree(dest)
        try:
            run_agent(dest, agent_prompt(title, body, reply))
        except Exception as exc:
            log.warning("agent reported: %s", redact(str(exc)))
            if not has_fix(dest):
                raise
            log.info("agent left a diff; committing it")
        if has_uncommitted(dest):
            stage_changes(dest)
        if not has_fix(dest):
            stray = checked_out_branch(dest)
            discard_worktree(dest, stray)
            dest = None
            branch = ""
            note_athena(payload, "没有改到代码，未开 MR。")
            return
        attach_branch(dest, branch)
        if _git_quiet(dest, ["diff", "--cached", "--quiet"]) == 1:
            commit_worktree(dest, title, issue_id)
        strip_cursor_trailer(dest)
        mr_url = push_branch(dest, branch, title, fresh.get("weblink") or "")
        pushed = True
        comment_mr(payload, mr_url)
        try:
            maybe_start_progress(project_id, issue_id, database_path)
        except Exception:
            log.exception("status update failed for %s", issue_id)
        try:
            discard_worktree(dest, branch)
        except Exception:
            log.exception("worktree remove failed for %s", dest)
        dest = None
        log.info("replied %s", mr_url)
    except Exception as exc:
        log.exception("job failed for %s", issue_id)
        if dest is not None and not pushed:
            try:
                if dest.exists() and not has_fix(dest):
                    discard_worktree(dest, checked_out_branch(dest) or branch)
                else:
                    log.info("left worktree in place at %s", dest)
            except Exception:
                log.exception("cleanup failed for %s", dest)
        elif pushed and dest is not None:
            try:
                discard_worktree(dest, branch)
            except Exception:
                log.exception("cleanup after push failed for %s", dest)
        try:
            if mr_url:
                comment_mr(payload, mr_url)
            else:
                note_athena(payload, f"修复失败：{redact(str(exc))}")
        except Exception:
            log.exception("failure comment was not posted")
            if mr_url:
                try:
                    note_athena(payload, f"MR 已开，回贴链接失败：{mr_url}")
                except Exception:
                    log.exception("mr link comment was not posted")


def worker() -> None:
    while not app.stop.is_set():
        try:
            payload = app.queue.get(timeout=0.5)
        except queue.Empty:
            continue
        node_id = str(payload.get("node_id") or "")
        app.running = node_id
        try:
            handle_job(payload)
        finally:
            app.running = None
            app.queued_ids.discard(node_id)
            app.queue.task_done()


def asyncio_fetch_issue(project_id: str, issue_id: str, database_path: str) -> dict:
    import asyncio
    return asyncio.run(fetch_issue(project_id, issue_id, database_path))


async def fetch_issue(project_id: str, issue_id: str, database_path: str) -> dict:
    from uuid import UUID

    from athena import AthenaClient

    async with AthenaClient() as client:
        node = await client.get_node(UUID(project_id), UUID(issue_id), depth=1, db_path=database_path)
        if node_has_mr_reply(node):
            return {
                "title": plain_text(node.name),
                "body": plain_text(node.description),
                "database_path": normalize_db_path(node.db_path or database_path),
                "shortlink": "",
                "type": node.type or "",
                "has_mr": True,
                "weblink": "",
            }
        shortlink = ""
        weblink = ""
        try:
            link = await client.create_shortlink(node, db_path=normalize_db_path(node.db_path or database_path))
            shortlink = link.shortlink or ""
            weblink = link.weblink_url(client.base_url) or ""
        except Exception as exc:
            log.warning("shortlink unavailable for %s: %s", issue_id, redact(str(exc)))
        return {
            "title": plain_text(node.name),
            "body": plain_text(node.description),
            "database_path": normalize_db_path(node.db_path or database_path),
            "shortlink": shortlink,
            "type": node.type or "",
            "has_mr": False,
            "weblink": weblink,
        }


def note_athena(payload: dict, note: str) -> None:
    post_named_comment(payload, comment_name_note(note))


def post_analysis(payload: dict, decision: str, analysis: str) -> None:
    post_named_comment(payload, comment_name_analysis(decision, analysis))


def post_named_comment(payload: dict, name: str) -> None:
    import asyncio
    asyncio.run(post_comment(
        payload["project_id"],
        payload["issue_id"],
        payload["database_path"],
        name,
    ))


def comment_mr(payload: dict, mr_url: str) -> None:
    import asyncio
    asyncio.run(post_comment(
        payload["project_id"],
        payload["issue_id"],
        payload["database_path"],
        comment_name_with_mr(mr_url),
    ))


def maybe_start_progress(project_id: str, issue_id: str, database_path: str) -> None:
    import asyncio
    asyncio.run(start_progress(project_id, issue_id, database_path))


async def post_comment(project_id: str, issue_id: str, database_path: str, name: str) -> None:
    from uuid import UUID

    from athena import AthenaClient
    from athena.models import NodeCreate

    async with AthenaClient() as client:
        await client.create_node(
            UUID(project_id),
            NodeCreate(
                name=name,
                type="comment",
                description="",
                parent_node_id=UUID(issue_id),
            ),
            db_path=database_path,
        )


async def start_progress(project_id: str, issue_id: str, database_path: str) -> None:
    from uuid import UUID

    from athena import AthenaClient
    from athena.client import endpoints

    async with AthenaClient() as client:
        node = await client.get_node(UUID(project_id), UUID(issue_id), depth=1, db_path=database_path)
        status = (node.status or "").strip().upper()
        if status != "NOT STARTED":
            log.info("issue %s status is %s", issue_id, node.status)
            return
        db_path = normalize_db_path(node.db_path or database_path)
        await client._request(
            "PUT",
            endpoints.node_url(issue_id),
            json={
                "status": "In Progress",
                "schedule_data": json.dumps({"status": "in_progress"}),
                "_db_path": db_path,
            },
        )
        log.info("issue %s set to In Progress", issue_id)


async def resolve_task(client, project_id, database_path: str, node):
    from athena.client import NotFoundError

    current = node
    for _ in range(8):
        if (current.type or "").lower() == "task":
            return current
        parent_id = current.parent_node_id
        if parent_id is None:
            return None
        parent_db = normalize_db_path(current.db_path or database_path)
        try:
            current = await client.get_node(project_id, parent_id, depth=1, db_path=parent_db)
        except NotFoundError:
            return None
    return None


def make_payload(kind: str, trigger, issue, project_id, database_path: str) -> dict:
    reply = ""
    if kind == "comment":
        reply = plain_text(trigger.name)
        extra = plain_text(trigger.description)
        if extra and extra not in reply:
            reply = f"{reply}\n{extra}".strip()
    return {
        "kind": kind,
        "node_id": str(trigger.id),
        "issue_id": str(issue.id),
        "project_id": str(project_id),
        "database_path": normalize_db_path(issue.db_path or database_path),
        "title": plain_text(issue.name),
        "body": plain_text(issue.description),
        "reply": reply,
    }


async def collect_payloads() -> list[dict]:
    from athena import AthenaClient
    from athena.client import NotFoundError

    async with AthenaClient() as client:
        auth = await client.check_auth()
        if not auth.authenticated:
            raise RuntimeError("Athena is not authenticated; run athena login")
        project = await client.get_project_by_name(PROJECT_NAME)
        if project is None:
            raise RuntimeError(f"Athena project {PROJECT_NAME} was not found")
        database_path = project.database_path or client._db_path(project.id)
        tasks = await client.advanced_search(
            node_type="task",
            db_path=database_path,
            include_archived=False,
        )
        comments = await client.advanced_search(
            node_type="comment",
            db_path=database_path,
            include_archived=False,
        )
        with state_lock:
            state = load_state()
            seen = set(state["seen"])
            if not state["baselined"]:
                for node in [*tasks, *comments]:
                    seen.add(str(node.id))
                state["seen"] = sorted(seen)
                state["baselined"] = True
                save_state(state)
                log.info("baseline recorded %d nodes", len(seen))
                return []

        payloads: list[dict] = []
        skip_ids: list[str] = []
        for node in comments:
            node_id = str(node.id)
            if node_id in seen:
                continue
            if node.archived or contains_marker(node.name, node.description) or contains_mr_reply(node.name, node.description):
                skip_ids.append(node_id)
                continue
            try:
                issue = await resolve_task(client, project.id, node.db_path or database_path, node)
            except NotFoundError:
                skip_ids.append(node_id)
                continue
            if issue is None:
                log.info("comment %s has no parent task", node_id)
                skip_ids.append(node_id)
                continue
            if node_has_mr_reply(issue):
                log.info("skip comment %s, issue already has an MR reply", node_id)
                skip_ids.append(node_id)
                continue
            payloads.append(make_payload("comment", node, issue, project.id, database_path))
        for node in tasks:
            node_id = str(node.id)
            if node_id in seen:
                continue
            if node.archived or contains_marker(node.name, node.description) or contains_mr_reply(node.name, node.description):
                skip_ids.append(node_id)
                continue
            try:
                full = await client.get_node(
                    project.id,
                    node.id,
                    depth=1,
                    db_path=normalize_db_path(node.db_path or database_path),
                )
            except NotFoundError:
                skip_ids.append(node_id)
                continue
            if node_has_mr_reply(full):
                log.info("skip task %s, MR already replied", node_id)
                skip_ids.append(node_id)
                continue
            payloads.append(make_payload("task", full, full, project.id, database_path))
        if skip_ids:
            with state_lock:
                state = load_state()
                merged = set(state["seen"])
                merged.update(skip_ids)
                state["seen"] = sorted(merged)
                save_state(state)
        log.info("poll found %d new node(s)", len(payloads))
        return payloads


def post_local(payload: dict) -> dict:
    raw = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        f"http://{HOST}:{app.port}/hook",
        data=raw,
        method="POST",
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")
        raise RuntimeError(f"hook HTTP {exc.code}: {redact(detail)}") from exc


def poll_loop() -> None:
    import asyncio

    while not app.stop.is_set():
        try:
            payloads = asyncio.run(collect_payloads())
            for payload in payloads:
                try:
                    result = post_local(payload)
                    log.info("hook accepted %s: %s", payload["node_id"], result.get("status"))
                except Exception:
                    log.exception("post to hook failed for %s", payload.get("node_id"))
        except Exception:
            log.exception("poll failed")
        if app.stop.wait(app.interval):
            return


def run_once() -> int:
    import asyncio

    payloads = asyncio.run(collect_payloads())
    if payloads:
        ids = ", ".join(item["node_id"] for item in payloads)
        log.info("poll left %d node(s) unseen so the daemon can enqueue them: %s", len(payloads), ids)
    return 0


def serve(start_worker: bool, start_poller: bool) -> int:
    server = ThreadingHTTPServer((HOST, app.port), HookHandler)
    server.daemon_threads = True
    if start_worker:
        threading.Thread(target=worker, name="athena-hook-worker", daemon=True).start()
    if start_poller:
        threading.Thread(target=poll_loop, name="athena-hook-poller", daemon=True).start()
    log.info("listening on http://%s:%s/hook", HOST, app.port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log.info("stopping")
    finally:
        app.stop.set()
        server.server_close()
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Poll Athena MiniHub and fix new issues locally.")
    parser.add_argument("--hook-only", action="store_true", help="serve POST /hook and do not fix or poll")
    parser.add_argument("--once", action="store_true", help="poll one time and exit; the first run only baselines")
    parser.add_argument("--port", type=int, default=int(os.environ.get("ATHENA_HOOK_PORT", DEFAULT_PORT)))
    parser.add_argument(
        "--interval",
        type=int,
        default=int(os.environ.get("ATHENA_HOOK_INTERVAL", DEFAULT_INTERVAL)),
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    logging.getLogger("httpx").setLevel(logging.WARNING)
    logging.getLogger("httpcore").setLevel(logging.WARNING)
    args = parse_args(sys.argv[1:] if argv is None else argv)
    app.port = args.port
    app.interval = args.interval
    if args.once and args.hook_only:
        sys.exit("--once and --hook-only cannot be combined")
    if args.hook_only:
        app.persist_seen = False
        return serve(start_worker=False, start_poller=False)
    ensure_athena_python()
    if args.once:
        return run_once()
    return serve(start_worker=True, start_poller=True)


if __name__ == "__main__":
    sys.exit(main())
