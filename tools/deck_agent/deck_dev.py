"""Git and CI for the Deck: what the watch shows while VS Code is in front.

Git: the repository comes from the VS Code window title ("file - folder - Visual
Studio Code"); its path from VS Code's recent folders, or from the Watch Deck
window (config "git_paths": {"folder": "C:/path"}). Every git command has a time
limit; nothing forces, resets or rewrites history.

CI: GitHub Actions through the REST API, token from `git credential fill` (the
same credential git uses). Repositories: config "ci_repos" ["owner/name", ...],
plus the GitHub remote of the repository open in VS Code.

All functions block: the agent calls them through asyncio.to_thread().
"""

from __future__ import annotations

import calendar
import json
import os
import re
import sqlite3
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

WINDOWS = sys.platform == "win32"
GIT_TIMEOUT = 8      # status, branches, local commands
NET_TIMEOUT = 45     # pull, push, fetch
BRANCHES_MAX = 12    # the watch list keeps 16 items: 3 status rows + branches
CI_MAX = 8
_NO_WINDOW = 0x08000000 if WINDOWS else 0  # CREATE_NO_WINDOW: no console flashing from the tray exe


def _run(args: list[str], cwd: str, timeout: float = GIT_TIMEOUT) -> subprocess.CompletedProcess:
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0", GCM_INTERACTIVE="never", LC_ALL="C")
    return subprocess.run(["git", "-c", "core.quotepath=off", *args], cwd=cwd, capture_output=True,
                          timeout=timeout, env=env, creationflags=_NO_WINDOW, encoding="utf-8", errors="replace")


def _short(text: str) -> str:
    """Last meaningful line of git's output, for a toast."""
    lines = [line.strip() for line in (text or "").splitlines() if line.strip() and not line.startswith("hint:")]
    return lines[-1] if lines else ""


# ------------------------------------------------------------------ repository from VS Code

def folder_from_title(title: str) -> str:
    """'main.c - watch-os-common - Visual Studio Code' -> 'watch-os-common'."""
    parts = [p.strip() for p in (title or "").split(" - ")]
    if len(parts) < 2 or "visual studio code" not in parts[-1].lower():
        return ""
    folder = parts[-2] if len(parts) >= 3 else parts[0]
    folder = re.sub(r"\s*\((Workspace|Area di lavoro)\)$", "", folder)
    folder = re.sub(r"\s*\[.*\]$", "", folder)  # [WSL: Ubuntu], [SSH: host]
    return folder.lstrip("● ").strip()


def _uri_path(uri: str) -> str:
    if not uri.startswith("file:///"):
        return ""
    path = urllib.parse.unquote(uri[len("file:///"):] if WINDOWS else uri[len("file://"):])
    return os.path.normpath(path)


def _vscode_folders() -> list[str]:
    """Folders VS Code opened recently: storage.json and the recently-opened list in state.vscdb."""
    if WINDOWS:
        base = os.path.join(os.environ.get("APPDATA", ""), "Code", "User", "globalStorage")
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Application Support/Code/User/globalStorage")
    else:
        base = os.path.expanduser("~/.config/Code/User/globalStorage")
    found: list[str] = []
    try:
        with open(os.path.join(base, "storage.json"), encoding="utf-8") as f:
            found += re.findall(r'"(file:///[^"]+)"', f.read())
    except OSError:
        pass
    try:  # read-only and immutable: never wait on VS Code's lock
        uri = "file:" + urllib.parse.quote(os.path.join(base, "state.vscdb").replace("\\", "/")) + "?mode=ro&immutable=1"
        with sqlite3.connect(uri, uri=True, timeout=1) as db:
            row = db.execute("SELECT value FROM ItemTable WHERE key='history.recentlyOpenedPathsList'").fetchone()
        if row:
            for entry in json.loads(row[0]).get("entries", []):
                found.append(entry.get("folderUri") or entry.get("workspace", {}).get("configPath", ""))
    except (sqlite3.Error, ValueError, AttributeError):
        pass
    paths = []
    for uri in found:
        path = _uri_path(uri)
        if path.endswith(".code-workspace"):
            path = os.path.dirname(path)
        if path and path not in paths and os.path.isdir(path):
            paths.append(path)
    return paths


def _git_root(path: str) -> str:
    try:
        r = _run(["rev-parse", "--show-toplevel"], path, 4)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return os.path.normpath(r.stdout.strip()) if r.returncode == 0 else ""


def _newest_child_repo(path: str) -> str:
    """A folder that holds several repositories (a workspace): the one touched last."""
    best, newest = "", 0.0
    try:
        for name in os.listdir(path):
            index = os.path.join(path, name, ".git", "index")
            if os.path.isfile(index) and os.path.getmtime(index) > newest:
                best, newest = os.path.join(path, name), os.path.getmtime(index)
    except OSError:
        pass
    return best


_REPO_CACHE: dict[str, tuple[float, str]] = {}


def repo_for_title(title: str, config: dict) -> str:
    """Path of the git repository behind a VS Code window, '' when unknown."""
    folder = folder_from_title(title)
    if not folder:
        return ""
    cached = _REPO_CACHE.get(folder)
    if cached and time.monotonic() - cached[0] < 30:
        return cached[1]
    candidates = []
    manual = (config.get("git_paths") or {}).get(folder)
    if manual:
        candidates.append(manual)
    candidates += [p for p in _vscode_folders() if os.path.basename(p).lower() == folder.lower()]
    repo = ""
    for path in candidates:
        if os.path.isdir(path):
            repo = _git_root(path) or _newest_child_repo(path)
            if repo:
                break
    _REPO_CACHE[folder] = (time.monotonic(), repo)
    return repo


# ------------------------------------------------------------------ git state and actions

def status(repo: str) -> dict:
    """{'repo','branch','changed','untracked','ahead','behind','conflicts','stash','branches'}."""
    r = _run(["status", "--porcelain=v2", "--branch"], repo)
    if r.returncode != 0:
        raise RuntimeError(_short(r.stderr) or "git status non riuscito")
    state = {"repo": os.path.basename(repo), "branch": "", "changed": 0, "untracked": 0,
             "ahead": 0, "behind": 0, "conflicts": 0, "stash": 0, "branches": []}
    for line in r.stdout.splitlines():
        if line.startswith("# branch.head "):
            state["branch"] = line.split(" ", 2)[2]
        elif line.startswith("# branch.ab "):
            ahead, behind = line.split()[2:4]
            state["ahead"], state["behind"] = int(ahead), -int(behind)
        elif line.startswith("u "):
            state["conflicts"] += 1
        elif line.startswith(("1 ", "2 ")):
            state["changed"] += 1
        elif line.startswith("? "):
            state["untracked"] += 1
    if state["branch"] == "(detached)":
        state["branch"] = "HEAD staccato"
    stash = _run(["stash", "list"], repo)
    state["stash"] = len(stash.stdout.splitlines()) if stash.returncode == 0 else 0
    refs = _run(["for-each-ref", "--sort=-committerdate", f"--count={BRANCHES_MAX}",
                 "--format=%(refname:short)", "refs/heads"], repo)
    state["branches"] = refs.stdout.split() if refs.returncode == 0 else []
    return state


def _outcome(r: subprocess.CompletedProcess, ok: str) -> str:
    if r.returncode != 0:
        raise RuntimeError(_short(r.stderr) or _short(r.stdout) or "comando non riuscito")
    return ok


def pull(repo: str) -> str:
    r = _run(["pull", "--ff-only"], repo, NET_TIMEOUT)
    if r.returncode != 0:
        raise RuntimeError("Pull non lineare: unisci da VS Code" if "Not possible to fast-forward" in r.stderr
                           else _short(r.stderr) or "pull non riuscito")
    return "Già aggiornato" if "Already up to date" in r.stdout else "Pull fatto"


def push(repo: str) -> str:
    state = status(repo)
    if state["conflicts"]:
        raise RuntimeError("Conflitti aperti: niente push")
    if state["branch"] == "HEAD staccato":
        raise RuntimeError("HEAD staccato: niente push")
    if state["behind"]:
        raise RuntimeError(f"Indietro di {state['behind']}: fai prima Pull")
    args = ["push"]
    if _run(["rev-parse", "--abbrev-ref", "@{upstream}"], repo).returncode != 0:
        args = ["push", "-u", "origin", state["branch"]]  # first push of a new branch
    return _outcome(_run(args, repo, NET_TIMEOUT), "Push fatto")


def fetch(repo: str) -> str:
    return _outcome(_run(["fetch", "--prune"], repo, NET_TIMEOUT), "Fetch fatto")


def quick_message(repo: str) -> str:
    """'Update deck_agent.py, orbit.c (+3)' from the changed tracked files."""
    r = _run(["diff", "--name-only", "HEAD"], repo)
    names = [os.path.basename(n) for n in r.stdout.split()] if r.returncode == 0 else []
    if not names:
        return "Update"
    head = ", ".join(names[:2])
    return f"Update {head}" + (f" (+{len(names) - 2})" if len(names) > 2 else "")


def commit(repo: str, message: str = "") -> str:
    """Tracked files only (git commit -a): untracked files may be anything, they stay out."""
    state = status(repo)
    if state["conflicts"]:
        raise RuntimeError("Conflitti aperti: niente commit")
    if not state["changed"]:
        raise RuntimeError("Niente da salvare")
    text = message.strip() or quick_message(repo)
    _outcome(_run(["commit", "-a", "-m", text], repo), "")
    left = f" · {state['untracked']} nuovi esclusi" if state["untracked"] else ""
    return f"Commit: {text}"[:40] + left


def stash(repo: str) -> str:
    return _outcome(_run(["stash", "push", "-m", "Watch Deck"], repo), "Modifiche messe da parte")


def pop(repo: str) -> str:
    return _outcome(_run(["stash", "pop"], repo), "Modifiche riprese")


def switch_to(repo: str, name: str) -> str:
    if name not in status(repo)["branches"]:
        raise RuntimeError("Branch non trovato")
    return _outcome(_run(["switch", name], repo), f"Ora su {name}")


# ------------------------------------------------------------------ GitHub Actions

_TOKEN: dict = {}


def _token() -> str:
    if time.monotonic() - _TOKEN.get("at", -1e9) < 3600:
        return _TOKEN.get("value", "")
    value = ""
    try:
        r = subprocess.run(["git", "-c", "safe.directory=*", "credential", "fill"],
                           input="protocol=https\nhost=github.com\n\n", capture_output=True, text=True,
                           timeout=15, creationflags=_NO_WINDOW,
                           env=dict(os.environ, GIT_TERMINAL_PROMPT="0", GCM_INTERACTIVE="never"))
        fields = dict(line.split("=", 1) for line in r.stdout.splitlines() if "=" in line)
        value = fields.get("password", "")
    except (OSError, subprocess.TimeoutExpired):
        pass
    # An empty answer (not logged in yet) is retried after a minute, not an hour.
    _TOKEN.update(at=time.monotonic() - (0 if value else 3540), value=value)
    return value


def _api(path: str, method: str = "GET") -> dict:
    headers = {"Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28",
               "User-Agent": "WatchDeck"}
    token = _token()
    if token:
        headers["Authorization"] = "Bearer " + token
    request = urllib.request.Request("https://api.github.com" + path, method=method, headers=headers,
                                     data=b"" if method == "POST" else None)
    with urllib.request.urlopen(request, timeout=15) as response:
        body = response.read()
    return json.loads(body) if body else {}


def github_slug(repo: str) -> str:
    """'owner/name' of the repository's GitHub remote, '' if it has none."""
    try:
        r = _run(["remote", "get-url", "origin"], repo, 4)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    match = re.search(r"github\.com[:/]([^/]+/[^/]+?)(?:\.git)?/?$", r.stdout.strip())
    return match.group(1) if match else ""


def _duration(run: dict) -> str:
    def stamp(text: str) -> float:
        return calendar.timegm(time.strptime(text, "%Y-%m-%dT%H:%M:%SZ")) if text else 0.0
    start = stamp(run.get("run_started_at") or run.get("created_at", ""))
    end = stamp(run.get("updated_at", "")) if run.get("status") == "completed" else time.time()
    seconds = max(0, int(end - start))
    return f"{seconds // 60}m{seconds % 60:02d}s" if seconds >= 60 else f"{seconds}s"


def ci_runs(slugs: list[str]) -> list[dict]:
    """Latest run of each repository: {'slug','id','state','branch','time','name'}.
    state: 'run' queued or in progress, 'ok', 'ko' failed, 'stop' cancelled/skipped, 'off' no runs or error."""
    runs = []
    for slug in slugs[:CI_MAX]:
        item = {"slug": slug, "id": 0, "state": "off", "branch": "", "time": "", "name": ""}
        try:
            data = _api(f"/repos/{slug}/actions/runs?per_page=1")
            latest = (data.get("workflow_runs") or [None])[0]
        except (urllib.error.URLError, OSError, ValueError):
            latest = None
        if latest:
            conclusion = latest.get("conclusion") or ""
            item.update(id=latest.get("id", 0), branch=latest.get("head_branch") or "", name=latest.get("name") or "",
                        time=_duration(latest),
                        state="run" if latest.get("status") != "completed"
                        else "ok" if conclusion == "success"
                        else "ko" if conclusion in ("failure", "timed_out", "startup_failure")
                        else "stop")
        runs.append(item)
    return runs


def ci_rerun(run: dict) -> str:
    if not run.get("id"):
        raise RuntimeError("Nessuna run da riavviare")
    if not _token():
        raise RuntimeError("Serve l'accesso a GitHub (git credential)")
    kind = "rerun-failed-jobs" if run.get("state") == "ko" else "rerun"
    try:
        _api(f"/repos/{run['slug']}/actions/runs/{run['id']}/{kind}", "POST")
    except urllib.error.HTTPError as error:
        raise RuntimeError(f"GitHub: errore {error.code}") from None
    return f"Riavviata {run['slug'].split('/')[-1]}"
