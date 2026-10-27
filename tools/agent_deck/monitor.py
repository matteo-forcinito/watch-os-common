"""Local development mission control. Python standard library; no credentials read.

Codex/Claude transcripts are read-only, bounded, and never served as raw logs.
Only processes started through `run` have authoritative exit status.
"""
from __future__ import annotations
import argparse
import contextlib
import hashlib
import hmac
import http.server
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import sqlite3
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
import uuid

HERE = Path(__file__).resolve().parent
STATES = {"working", "needs_input", "done", "error", "idle", "unknown", "interrupted"}
CREATE_FLAGS = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0

def clean(value, limit=240):
    text = str(value or "")
    text = re.sub(r"(?i)(?:bearer\s+|sk-)[a-z0-9_\-.]{8,}", "[redacted]", text)
    text = re.sub(r"(?i)((?:token|password|secret|api[_-]?key)\s*[=:]\s*)[^\s,;]+", r"\1[redacted]", text)
    return "".join(c for c in text if c >= " " or c == "\n").encode("utf-8")[:limit].decode("utf-8", "ignore")

def bounded_json_lines(path, size=262144):
    try:
        with Path(path).open("rb") as f:
            end = f.seek(0, 2)
            f.seek(max(0, end-size))
            if end > size:
                f.readline()
            for line in f:
                try:
                    value = json.loads(line)
                    if isinstance(value, dict): yield value
                except (ValueError, UnicodeError): pass
    except OSError: return

def stamp(value):
    try:
        from datetime import datetime
        return datetime.fromisoformat(str(value).replace("Z", "+00:00")).timestamp()
    except (ValueError, TypeError): return 0

def quota_windows(limits):
    out = []
    if not isinstance(limits, dict): return out
    for key in ("primary", "secondary"):
        item = limits.get(key)
        if not isinstance(item, dict): continue
        used = item.get("used_percent", item.get("usedPercent"))
        if isinstance(used, (int, float)) and 0 <= used <= 100:
            out.append({"name": key, "remaining_percent": round(100-used, 1),
                        "resets_at": item.get("resets_at", item.get("resetsAt")),
                        "window_minutes": item.get("window_minutes", item.get("windowDurationMins"))})
    return out

def device_url(url):
    import ipaddress
    parsed=urllib.parse.urlsplit(str(url))
    address=ipaddress.ip_address(parsed.hostname)
    if parsed.scheme!='http' or not address.is_private or address.is_loopback or address.is_link_local or address.is_multicast or parsed.username or parsed.password or parsed.query or parsed.fragment or parsed.path not in ('','/'):
        raise ValueError('Indica l’indirizzo IP LAN del dispositivo')
    return str(url).rstrip('/')

def pair_device(url, code, target):
    url=device_url(url)
    code=str(code).replace(' ','')
    if not re.fullmatch(r'\d{6}',code):raise ValueError('Il codice deve contenere sei cifre')
    request=urllib.request.Request(url+'/api/pair',json.dumps({'code':code}).encode(),{'Content-Type':'application/json'})
    with urllib.request.urlopen(request,timeout=8) as response: data=json.load(response)
    token=data.get('token')
    if not isinstance(token,str) or not 16<=len(token)<=256:raise ValueError('Associazione non riuscita')
    target=Path(target);target.parent.mkdir(parents=True,exist_ok=True)
    temporary=target.with_suffix('.tmp');temporary.write_text(json.dumps({'url':url,'token':token}),encoding='utf-8');temporary.replace(target)

def session_events(path):
    # Keep the initial goal visible even after large tool output leaves the tail.
    try:
        if Path(path).stat().st_size>262144:
            with Path(path).open('rb') as stream:
                head=stream.read(131072)
            for line in head.splitlines()[:-1]:
                try:
                    item=json.loads(line)
                    if isinstance(item,dict):yield item
                except (ValueError,UnicodeError):pass
    except OSError:pass
    yield from bounded_json_lines(path)


def task_description(message):
    """Only a bounded user goal, excluding the Aetherion context envelope."""
    if not isinstance(message,str): return ''
    marker='AETHERION_CONTEXT_JSON\n'
    if marker in message:
        try:
            tail=message.split(marker,1)[1]
            _,end=json.JSONDecoder().raw_decode(tail)
            message=tail[end:].strip()
        except (ValueError,TypeError): return ''
    if not message or message.startswith(('<environment_context>','<permissions')):return ''
    return clean(message.split('\n\nLavora nel workspace isolato.',1)[0].strip(),240)


def codex_activity(path, now=None):
    now = now or time.time()
    result = {"status": "unknown", "activity": "Stato non disponibile", "context_percent": None,
              "tokens": None, "token_usage": {}, "context_used": None,"context_window":None,"usage_observed_at":None,
              "task_description":"", "last_update":"", "step":"", "cost_usd":None,"cost_kind":"unavailable","quota": [], "quota_observed_at": None, "source": "codex_rollout", "observed_at": 0}
    pending = set()
    for event in session_events(path):
        payload = event.get("payload") or {}
        if not isinstance(payload, dict): continue
        when = stamp(event.get("timestamp"))
        result["observed_at"] = max(result["observed_at"], when)
        kind = payload.get("type", "")
        if event.get('type')=='turn_context':
            if payload.get('model'):result['model']=clean(payload['model'],80)
            if payload.get('effort'):result['effort']=clean(payload['effort'],24)
        if event.get("type") == "event_msg":
            if kind in ("task_started", "turn_started", "user_message"):
                result.update(status="working", activity="In elaborazione")
                if kind=='user_message':
                    description=task_description(payload.get('message',''))
                    if description:result['task_description']=description
            elif kind=='agent_message' and payload.get('phase')!='final_answer':
                result['last_update']=clean(payload.get('message',''),220)
            elif kind in ("task_complete", "turn_complete", "task_completed"):
                result.update(status="done", activity="Turno completato")
                pending.clear()
            elif kind in ("turn_aborted", "task_aborted"):
                result.update(status="interrupted", activity="Turno interrotto")
                pending.clear()
            elif kind == "token_count":
                info = payload.get("info") or {}
                usage = info.get("total_token_usage") or {}
                valid=lambda v:isinstance(v,int) and not isinstance(v,bool) and 0<=v<=9007199254740991
                result['token_usage']={k:usage[k] for k in ('input_tokens','cached_input_tokens','output_tokens','reasoning_output_tokens','total_tokens') if valid(usage.get(k))}
                result["tokens"] = result['token_usage'].get('total_tokens')
                result['usage_observed_at']=when
                last = info.get("last_token_usage") or {}
                window = info.get("model_context_window")
                context = last.get("input_tokens",last.get("total_tokens"))
                if isinstance(window, (int, float)) and window > 0 and isinstance(context, (int, float)):
                    result["context_percent"] = min(100, round(context/window*100, 1))
                    result.update(context_used=context,context_window=window)
                windows = quota_windows(payload.get("rate_limits"))
                if windows:
                    result["quota"] = windows
                    result["quota_observed_at"] = when
        if event.get('type')=='response_item' and kind=='message':
            content=payload.get('content') or []
            text='\n\n'.join(x.get('text','') for x in content if isinstance(x,dict) and isinstance(x.get('text'),str))
            if payload.get('role')=='user':
                description=task_description(text)
                if description:result['task_description']=description
            elif payload.get('role')=='assistant' and payload.get('phase')=='commentary':
                result['last_update']=clean(text,220)
        if event.get("type") == "response_item":
            if kind in ("function_call", "custom_tool_call"):
                name = str(payload.get("name", ""))
                pending.add(payload.get("call_id", ""))
                if "request_user_input" in name or name.endswith('aetherion_ask'):
                    result.update(status="needs_input", activity="Input richiesto: apri Codex per rispondere")
                    try:
                        args=payload.get("arguments",payload.get("input",{}))
                        if isinstance(args,str): args=json.loads(args[:16384])
                        questions=args.get("questions",[]) if isinstance(args,dict) else []
                        if isinstance(args,dict) and args.get('question'):result['activity']='Input: '+clean(args['question'],220)
                        if questions and isinstance(questions[0],dict):
                            question=questions[0].get("question",questions[0].get("title",""))
                            if question:result["activity"]="Input: "+clean(question,220)
                    except (ValueError,TypeError): pass
                else:
                    result.update(status="working", activity="Strumento: "+clean(name, 70))
            elif kind in ("function_call_output", "custom_tool_call_output"):
                pending.discard(payload.get("call_id", ""))
                if result["status"] != "needs_input":
                    result.update(status="working", activity="Elaborazione risultato")
            elif kind == "message" and payload.get("role") == "assistant" and payload.get("phase") == "final_answer":
                result.update(status="done", activity="Risposta pronta")
    age = now-result["observed_at"]
    if age > 180 and result["status"] == "working":
        result.update(status="unknown", activity="Nessun evento recente: esecuzione non confermata")
    result['step']=result['activity']
    if result['task_description']:
        result['activity']=clean(result['task_description'],120)+' | '+clean(result['step'],100)
    return result

def codex_sessions(home, now=None):
    now = now or time.time()
    databases = sorted(Path(home).glob("state_*.sqlite"), key=lambda p: int(re.search(r"(\d+)\.sqlite", p.name)[1]), reverse=True)
    if not databases: return [], "Nessun archivio Codex locale"
    try:
        with contextlib.closing(sqlite3.connect(databases[0].as_uri()+"?mode=ro", uri=True, timeout=.2)) as db:
            db.row_factory = sqlite3.Row
            cols = {row[1] for row in db.execute("pragma table_info(threads)")}
            names = [n for n in ("id", "rollout_path", "cwd", "title", "git_branch", "git_sha", "model", "updated_at", "archived") if n in cols]
            if not {"id", "rollout_path", "updated_at"} <= cols: return [], "Schema Codex non riconosciuto"
            rows = db.execute("select "+",".join(names)+" from threads where updated_at > ? order by updated_at desc limit 24", (int(now-86400),)).fetchall()
        out = []
        root = Path(home).resolve()
        for raw in rows:
            row = dict(raw)
            if row.get("archived"): continue
            path = Path(row["rollout_path"]).resolve()
            if not path.is_relative_to(root) or path.suffix != ".jsonl": continue
            item = codex_activity(path, now)
            item.update(id="codex:"+row["id"], session_id=row["id"], agent="codex", name=clean(item.get("task_description") or row.get("title") or "Codex", 100),
                        repo=clean(Path(row.get("cwd") or ".").name, 80), cwd=clean(row.get("cwd"), 300),
                        branch=clean(row.get("git_branch"), 100), commit=clean(row.get("git_sha"), 40), model=clean(item.get('model') or row.get("model"), 80),
                        resume="codex resume "+row["id"], capabilities=["observe", "resume_in_terminal"])
            out.append(item)
        return out, None
    except (sqlite3.Error, OSError, ValueError): return [], "Archivio Codex occupato o non leggibile"

# Claude Code hook events -> (status, activity prefix). Hooks are the real status;
# transcripts only fill in what hooks do not say (model, tokens, branch).
HOOK_STATES = {"SessionStart": ("idle", "Sessione avviata"), "UserPromptSubmit": ("working", "Richiesta"),
               "PreToolUse": ("working", "Strumento"), "PostToolUse": ("working", "Strumento"),
               "PermissionRequest": ("needs_input", "Permesso richiesto"), "Notification": ("needs_input", "Ti aspetta"),
               "Stop": ("done", "Turno completato"), "SessionEnd": ("idle", "Sessione chiusa"), "PreCompact": ("working", "Compattazione")}

HARNESS_PROMPT = re.compile(r"\s*(<(task-notification|agent-message|system-reminder|command-|local-command)|\[SYSTEM NOTIFICATION)")

def record_hook(directory, event, now=None):
    """Store one Claude Code hook payload as the session's latest state; never the transcript."""
    name, session = event.get("hook_event_name"), clean(event.get("session_id"), 80)
    if name not in HOOK_STATES or not re.fullmatch(r"[A-Za-z0-9_-]{1,80}", session): return None
    folder = Path(directory)/"claude-hooks"; folder.mkdir(parents=True, exist_ok=True)
    path = folder/(session+".json")
    try: state = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError): state = {}
    status, activity = HOOK_STATES[name]
    if name == "UserPromptSubmit":
        prompt = re.sub(r"</?pasted_content[^>]*>", "", str(event.get("prompt") or ""))
        # Task notifications and subagent hand-backs arrive as prompts but are not new requests.
        if not HARNESS_PROMPT.match(prompt):
            state["goal"] = clean(" ".join(prompt.split()), 300)
            state["subagents"] = []
    elif name in ("PreToolUse", "PostToolUse", "PermissionRequest"):
        tool = clean(event.get("tool_name"), 70)
        tool_input = event.get("tool_input") if isinstance(event.get("tool_input"), dict) else {}
        detail = tool_input.get("description") or tool_input.get("subagent_type") or ""
        activity += ": "+tool+(" - "+clean(detail, 120) if detail else "")
        if name == "PreToolUse" and tool in ("Agent", "Task"):
            state["subagents"] = (state.get("subagents") or [])[-7:]+[clean(detail or tool_input.get("subagent_type") or tool, 120)]
    elif name == "Notification":
        activity = clean(event.get("message"), 200) or activity
        if event.get("notification_type") not in (None, "", "permission_prompt", "idle_prompt", "elicitation_dialog"): status = state.get("status", "working")
    state.update(session_id=session, status=status, activity=clean(activity, 240), event=name, cwd=clean(event.get("cwd"), 300),
                 observed_at=now or time.time())
    tmp = path.with_suffix(".tmp"); tmp.write_text(json.dumps(state, ensure_ascii=False), encoding="utf-8"); os.replace(tmp, path)
    return state

def context_percent(usage, model):
    """Share of the context window held by the LAST assistant message (input + cache tokens); None when unknown. Never a cost."""
    keys = ("input_tokens", "cache_creation_input_tokens", "cache_read_input_tokens")
    if not isinstance(usage, dict) or not isinstance(usage.get("input_tokens"), (int, float)): return None
    used = sum(v for v in (usage.get(k, 0) for k in keys) if isinstance(v, (int, float)) and not isinstance(v, bool))
    # Transcripts rarely carry the "[1m]" suffix: usage above 200k proves the 1M window.
    window = 1000000 if "[1m]" in str(model).lower() or used > 200000 else 200000
    return round(min(100.0, 100.0*used/window), 1)

def claude_subagents(folder, parent, now):
    """One card per subagent transcript (<session>/subagents/agent-<id>.jsonl): own model, tokens, context and step."""
    out = []
    try: paths = sorted((p for p in Path(folder).glob("agent-*.jsonl") if p.stat().st_mtime > now-86400), key=lambda p: p.stat().st_mtime, reverse=True)[:12]
    except OSError: return out
    for path in paths:
        agent_id = clean(path.stem[6:], 40)
        try: meta = json.loads(path.with_suffix(".meta.json").read_text(encoding="utf-8"))
        except (OSError, ValueError): meta = {}
        kind = clean(meta.get("agentType") or "subagent", 60)
        item = dict(id="claude:"+parent["session_id"]+":"+agent_id, session_id=parent["session_id"], agent_id=agent_id, parent_id=parent["id"], agent="claude",
                    name=clean(meta.get("description") or kind, 100), agent_type=kind, repo=parent.get("repo", ""), cwd=parent.get("cwd", ""), branch=parent.get("branch", ""),
                    model="", status="unknown", activity="Avvio", context_percent=None, tokens=None, quota=[], source="claude_subagent",
                    observed_at=path.stat().st_mtime, capabilities=["observe"])
        for event in bounded_json_lines(path):
            msg = event.get("message") or {}
            if event.get("type") != "assistant" or not isinstance(msg, dict): continue
            # API errors (e.g. rate limits) are written as "<synthetic>" messages with zero usage.
            if msg.get("model") == "<synthetic>": item.update(status="unknown", activity="Interrotto: errore API"); continue
            item["model"] = clean(msg.get("model"), 80) or item["model"]
            tools = [c.get("name", "") for c in msg.get("content") or [] if isinstance(c, dict) and c.get("type") == "tool_use"]
            if (msg.get("stop_reason") in ("end_turn", "stop_sequence") and not tools) or "SubagentHandback" in tools: item.update(status="done", activity="Completato")
            else: item.update(status="working", activity="Strumento: "+clean(tools[-1], 70) if tools else "In elaborazione")
            usage = msg.get("usage") or {}
            values = [usage.get(k, 0) for k in ("input_tokens", "output_tokens", "cache_creation_input_tokens", "cache_read_input_tokens")]
            if all(isinstance(v, (int, float)) for v in values): item["tokens"] = sum(values)
            percent = context_percent(usage, item["model"])
            if percent is not None: item["context_percent"] = percent
        item["last_event_age"] = max(0, int(now-item["observed_at"]))
        # Background subagents write their transcript continuously; silence means it stopped.
        if item["status"] == "working" and item["last_event_age"] > 300: item.update(status="unknown", activity="Nessun evento recente")
        out.append(item)
    return out

def claude_sessions(home, now=None, hooks=None):
    now = now or time.time()
    root = Path(home)/"projects"
    if not root.exists(): return [], "Claude Code: nessuna sessione locale"
    paths = []
    try:
        for folder in root.iterdir():
            if not folder.is_dir(): continue
            for p in folder.glob("*.jsonl"):
                if p.stat().st_mtime > now-86400: paths.append(p)
    except OSError: return [], "Archivio Claude non leggibile"
    out = []
    for path in sorted(paths, key=lambda p: p.stat().st_mtime, reverse=True)[:24]:
        item = dict(id="claude:"+path.stem, session_id=path.stem, agent="claude", name="Claude Code", repo="", cwd="", branch="", model="",
                    status="unknown", activity="Stato non disponibile", context_percent=None, tokens=None, last_event_age=None, quota=[], source="claude_transcript",
                    observed_at=path.stat().st_mtime, resume="claude --resume "+path.stem, capabilities=["observe", "resume_in_terminal"])
        for event in bounded_json_lines(path):
            item["cwd"] = clean(event.get("cwd") or item["cwd"], 300)
            item["branch"] = clean(event.get("gitBranch") or item["branch"], 100)
            msg = event.get("message") or {}
            if not isinstance(msg, dict): continue
            if event.get("type") == "assistant":
                item["model"] = clean(msg.get("model"), 80)
                if msg.get("stop_reason") in ("end_turn", "stop_sequence"):
                    item.update(status="done", activity="Turno completato")
                else: item.update(status="working", activity="In elaborazione")
                for content in msg.get("content") or []:
                    if isinstance(content, dict) and content.get("type") == "tool_use":
                        tool = content.get("name", "")
                        item.update(status="needs_input" if tool == "AskUserQuestion" else "working", activity="Strumento: "+clean(tool, 70))
                usage = msg.get("usage") or {}
                values = [usage.get(k, 0) for k in ("input_tokens", "output_tokens", "cache_creation_input_tokens", "cache_read_input_tokens")]
                if all(isinstance(v, (int, float)) for v in values): item["tokens"] = sum(values)
                percent = context_percent(usage, item["model"])
                if percent is not None: item["context_percent"] = percent
        hook = None
        if hooks:
            try: hook = json.loads((Path(hooks)/"claude-hooks"/(path.stem+".json")).read_text(encoding="utf-8"))
            except (OSError, ValueError): pass
        if hook and hook.get("observed_at", 0) >= item["observed_at"]-5:
            item.update(status=hook.get("status", item["status"]), activity=hook.get("activity") or item["activity"],
                        goal=hook.get("goal", ""), subagents=hook.get("subagents") or [], cwd=hook.get("cwd") or item["cwd"],
                        observed_at=max(item["observed_at"], hook["observed_at"]), source="claude_hooks")
        item["repo"] = clean(Path(item["cwd"]).name, 80)
        item["last_event_age"] = max(0, int(now-item["observed_at"]))
        stale = item["status"] == "working" and item.get("source") != "claude_hooks"
        if now-item["observed_at"] > 180 and stale: item.update(status="unknown", activity="Nessun evento recente")
        out.append(item)
        out.extend(claude_subagents(path.parent/path.stem/"subagents", item, now))
    return out, None if out else "Claude Code: nessuna sessione nelle ultime 24 ore"

SIM_SESSIONS = (("sim-alpha", "Aggiorna la documentazione", "repo-alpha", 12.5, 25000), ("sim-beta", "Correggi il test instabile", "repo-beta", 48.0, 96000),
                ("sim-gamma", "Prepara la nota di rilascio", "repo-gamma", 81.5, 163000))
SIM_CYCLE = ("working", "needs_input", "done")

def sim_write(directory, step, now=None):
    """Write synthetic hook states (working -> needs_input -> done, one phase apart per session) into a SEPARATE directory."""
    directory = Path(directory)
    if directory.resolve() == (Path.home()/".agent-deck").resolve(): raise ValueError("Il simulatore non scrive nella cartella reale")
    # Any non-simulated session file means this is a real hooks directory (e.g. a custom --data).
    if any(not p.name.startswith("sim-") for p in (directory/"claude-hooks").glob("*.json")): raise ValueError("Il simulatore non scrive in una cartella con sessioni reali")
    out = []
    for i, (sid, goal, repo, ctx, tokens) in enumerate(SIM_SESSIONS):
        phase = SIM_CYCLE[(step+i) % 3]
        if phase == "working": events = [dict(hook_event_name="UserPromptSubmit", prompt=goal), dict(hook_event_name="PreToolUse", tool_name="Bash", tool_input=dict(description="Simulazione"))]
        elif phase == "needs_input": events = [dict(hook_event_name="PermissionRequest", tool_name="Bash")]
        else: events = [dict(hook_event_name="Stop")]
        for e in events: state = record_hook(directory, dict(e, session_id=sid, cwd="C:/sim/"+repo), now)
        state.update(goal=goal, simulated=True, model="claude-sim", context_percent=min(100.0, ctx+step), tokens=tokens+step*1000)
        path = directory/"claude-hooks"/(sid+".json"); path.write_text(json.dumps(state, ensure_ascii=False), encoding="utf-8")
        out.append(state)
    return out

def sim_sessions(directory, now=None):
    """Agents built from simulator hook files; only reads the given directory."""
    now = now or time.time(); out = []
    for path in sorted((Path(directory)/"claude-hooks").glob("sim-*.json"))[:8]:
        try: h = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError): continue
        out.append(dict(id="claude:"+h["session_id"], session_id=h["session_id"], agent="claude", name="Claude Code (simulato)", repo=Path(h.get("cwd") or "").name, cwd=h.get("cwd", ""), branch="main",
                        model=h.get("model", ""), status=h.get("status", "unknown"), activity=h.get("activity", ""), goal=h.get("goal", ""), subagents=[], context_percent=h.get("context_percent"),
                        tokens=h.get("tokens"), last_event_age=max(0, int(now-h.get("observed_at", now))), quota=[], source="simulator", observed_at=h.get("observed_at", now), capabilities=["observe"]))
    return out

def run_sim(directory, interval=4.0, steps=0):
    step = 0
    while not steps or step < steps:
        sim_write(directory, step); step += 1
        if not steps or step < steps: time.sleep(interval)

class Store:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.path = self.directory/"activity.sqlite"
        with self.db() as db:
            db.execute("create table if not exists jobs (id text primary key, data text not null)")
    @contextlib.contextmanager
    def db(self):
        db = sqlite3.connect(self.path, timeout=5)
        try:
            with db: yield db
        finally: db.close()
    def put(self, item):
        with self.db() as db: db.execute("insert or replace into jobs values (?,?)", (item["id"], json.dumps(item)))
    def all(self):
        with self.db() as db: return [json.loads(row[0]) for row in db.execute("select data from jobs order by rowid desc limit 80")]
    def get(self, key): return next((x for x in self.all() if x["id"] == key), None)

def git(path, *args):
    try:
        return subprocess.check_output(["git", "-C", str(path), *args], stderr=subprocess.DEVNULL, timeout=4,
                                       creationflags=CREATE_FLAGS).decode("utf-8", "replace").strip()
    except (OSError, subprocess.SubprocessError): return ""

def repository(path):
    branch = git(path, "branch", "--show-current")
    head = git(path, "rev-parse", "HEAD")
    if not head: return None
    status = git(path, "status", "--porcelain", "--untracked-files=no")
    return dict(name=Path(path).name, cwd=str(path), branch=branch or "detached", commit=head,
                dirty=bool(status), changed_files=len(status.splitlines()), source="git", observed_at=time.time())

def process_ids():
    """Only names/PIDs; never collect command lines or environment variables."""
    if os.name != "nt": return []
    script = "Get-CimInstance Win32_Process | Where-Object {$_.Name -in @('ninja.exe','cmake.exe','cc1.exe','xtensa-esp32s3-elf-gcc.exe')} | Select-Object Name,ProcessId,ParentProcessId | ConvertTo-Json -Compress"
    try:
        raw = subprocess.check_output(["powershell", "-NoProfile", "-Command", script], timeout=8, creationflags=CREATE_FLAGS)
        data = json.loads(raw or b"[]")
        return data if isinstance(data, list) else [data]
    except (OSError, ValueError, subprocess.SubprocessError): return []

def process_alive(pid):
    # On Windows os.kill(pid, 0) is NOT the POSIX existence probe: never use it.
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        api = ctypes.WinDLL("kernel32", use_last_error=True)
        api.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        api.OpenProcess.restype = wintypes.HANDLE
        api.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        api.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = api.OpenProcess(0x1000, False, int(pid))  # QUERY_LIMITED_INFORMATION
        if not handle:
            return ctypes.get_last_error() == 5  # Access denied is not proof of exit.
        try:
            code = wintypes.DWORD()
            return not api.GetExitCodeProcess(handle, ctypes.byref(code)) or code.value == 259
        finally: api.CloseHandle(handle)
    try: os.kill(pid, 0); return True
    except PermissionError: return True
    except ProcessLookupError: return False

def apply_owned_terminal_states(agents, directory):
    """Optional read-only owner state corrects rollouts killed before a final event."""
    if not directory:return
    path=Path(directory).resolve()/'control.sqlite'
    try:
        with contextlib.closing(sqlite3.connect(path.as_uri()+'?mode=ro',uri=True,timeout=.2)) as db:
            owned={}
            for row in db.execute("SELECT body FROM entities WHERE kind='chat_threads'"):
                item=json.loads(row[0]);remote=item.get('remote_thread_id')
                if remote:owned[remote]=item
        for agent in agents:
            item=owned.get(agent.get('session_id'))
            if not item:continue
            state={'succeeded':'done','failed':'error','interrupted':'interrupted'}.get(item.get('status'))
            if not state:continue
            agent.update(status=state,owner_thread_id=item['id'],owner_status=item['status'])
            agent['step']={'done':'Turno concluso','error':'Turno fallito: apri la conversazione','interrupted':'Turno interrotto'}[state]
            agent['activity']=clean(agent.get('task_description'),120)+' | '+agent['step']
    except (OSError,sqlite3.Error,ValueError,TypeError):return


def agent_groups(sessions):
    """One provider agent, with separate sessions retained for history/metrics."""
    groups={};result=[]
    for item in sessions:
        if item.get('source')=='claude_subagent':
            # A subagent is its own worker: own card, named by its role, while active or just finished.
            if item.get('status') in ('working','needs_input') or (item.get('last_event_age') or 0)<1800:
                result.append(dict(item,name=clean(item.get('name') or item.get('agent_type'),100),activity=clean((item.get('agent_type') or 'subagent')+' | '+(item.get('activity') or ''),220)))
            continue
        groups.setdefault(item.get('agent') or item.get('id'),[]).append(item)
    rank={'needs_input':0,'working':1,'error':2,'unknown':3}
    for provider,items in groups.items():
        ordered=sorted(items,key=lambda x:(rank.get(x.get('status'),4),-x.get('observed_at',0)))
        selected=ordered[0];active=sum(x.get('status') in ('working','needs_input') for x in items)
        group=dict(selected,id='agent:'+str(provider),name={'codex':'Codex','claude':'Claude'}.get(provider,provider),
                   session_count=len(items),active_sessions=active,representative_session_id=selected.get('session_id'))
        group['activity']=clean(str(len(items))+' sessioni, '+str(active)+' attive | '+(selected.get('activity') or ''),220)
        result.append(group)
    return result


class Monitor:
    def __init__(self, args):
        self.args = args
        self.store = Store(args.data)
        self.lock = threading.Lock()
        self.snapshot = {}
        self.repo_at = 0
        self.repos = []
        self.process_at = 0
        self.processes = []
        self.stop = threading.Event()
        self.delivery = "Non configurato"
        self.quota_at = 0
        self.quota_error = None
        if not getattr(args,'device_config',None) and (self.store.directory/'device.json').exists():
            args.device_config=str(self.store.directory/'device.json')
    def collect(self):
        now = time.time()
        agents, error = codex_sessions(Path(os.environ.get("CODEX_HOME", Path.home()/".codex")), now)
        claude, cerr = claude_sessions(Path(os.environ.get("CLAUDE_CONFIG_DIR", Path.home()/".claude")), now, self.store.directory)
        agents.extend(claude)
        if getattr(self.args, "sim_dir", None): agents.extend(sim_sessions(self.args.sim_dir, now))
        apply_owned_terminal_states(agents,getattr(self.args,"aetherion_data",None))
        quota_file=self.store.directory/'account_quota.json'
        if getattr(self.args,'live_quota',False) and now-self.quota_at>120:
            self.quota_at=now
            try:
                from codex_quota import read_quota
                quota=read_quota()
                temporary=quota_file.with_suffix('.tmp');temporary.write_text(json.dumps(quota),encoding='utf-8');temporary.replace(quota_file)
                self.quota_error=None
            except Exception:self.quota_error='Quota Codex non aggiornata: resta visibile l’ultima rilevazione'
        try:account_quota=json.loads(quota_file.read_text(encoding='utf-8'))
        except (OSError,ValueError):account_quota=None
        if now-self.repo_at > 60:
            workspace = Path(self.args.workspace).resolve()
            roots = [workspace]+[p for p in workspace.iterdir() if p.is_dir() and (p/".git").exists()][:32]
            self.repos = [item for p in roots if (item := repository(p))]
            self.repo_at = now
        if now-self.process_at > 15:
            self.processes = process_ids()
            self.process_at = now
        jobs = self.store.all()
        # A process crash is not a successful build. Stale tracked PIDs become interrupted.
        for job in jobs:
            if job["status"] == "working" and job.get("pid"):
                if not process_alive(job["pid"]):
                    job.update(status="interrupted", activity="Processo terminato senza esito registrato", observed_at=now)
        result = dict(schema=1, generated_at=now, agents=agents[:24], jobs=jobs[:32], repositories=self.repos,
                      processes=self.processes[:32], sources={"codex": error or "Archivio locale in sola lettura", "claude": cerr or "Trascrizioni locali in sola lettura"},
                      delivery=self.delivery, session_allowance={"remaining_sessions": None, "reason": "I provider non espongono un numero fisso di sessioni residue. Vengono mostrati solo quote e reset osservati."})
        result.update(account_quota=account_quota,quota_error=self.quota_error,agent_groups=agent_groups(agents[:24]))
        with self.lock: self.snapshot = result
        return result
    def compact(self, full):
        # Device has a deliberately bounded RAM budget; full detail stays on PC.
        cards = []
        for item in sorted(agent_groups(full["agents"])+[j for j in full["jobs"] if j.get("id")!="aetherion-codex-architect"], key=lambda x: (x["status"] != "needs_input", x["status"] != "working", -x.get("observed_at", 0)))[:16]:
            card={k: item.get(k) for k in ("id", "agent", "name", "status", "activity", "task_description", "last_update", "step", "session_count", "active_sessions", "representative_session_id", "repo", "branch", "commit", "model", "context_percent", "context_used","context_window","tokens","observed_at", "quota", "resume")}
            usage=item.get('token_usage') or {}
            for source,target in [('input_tokens','tokens_input'),('cached_input_tokens','tokens_cached'),('output_tokens','tokens_output')]:
                if source in usage:card[target]=usage[source]
            cards.append(card)
        quota=full.get('account_quota') or {}
        if quota.get('windows'):
            cards.insert(0,dict(id='codex-account-quota',agent='codex',name='Quota Codex',status='idle',activity='Account CLI corrente: ultima rilevazione',
                                repo='Account CLI locale',observed_at=quota.get('observed_at',0),quota=quota['windows'],context_percent=None))
            cards=cards[:16]
        result = dict(schema=1, generated_at=full["generated_at"], cards=cards, total=len(agent_groups(full["agents"]))+sum(j.get("id")!="aetherion-codex-architect" for j in full["jobs"])+(1 if quota.get('windows') else 0), total_sessions=len(full["agents"]))
        raw = json.dumps(result, ensure_ascii=False, separators=(",", ":")).encode()
        while len(raw) > 24000 and result["cards"]:
            result["cards"].pop()
            raw = json.dumps(result, ensure_ascii=False, separators=(",", ":")).encode()
        return raw
    def push(self, snapshot):
        if not self.args.device_config: return
        try:
            config = json.loads(Path(self.args.device_config).read_text(encoding="utf-8"))
            url = device_url(config["url"])
            request = urllib.request.Request(url+"/agentdeck/api/snapshot", self.compact(snapshot),
                                             {"X-Key": config["token"], "Content-Type": "application/json"})
            with urllib.request.urlopen(request, timeout=3) as response:
                if response.status != 200: raise ValueError("Risposta dispositivo")
            self.delivery = "Collegato"
        except Exception: self.delivery = "Non raggiungibile: apri Agent Deck sul dispositivo"
    def loop(self):
        while not self.stop.is_set():
            try: self.push(self.collect())
            except Exception:
                with self.lock: self.snapshot = dict(schema=1, generated_at=time.time(), agents=[], jobs=[], repositories=[], processes=[], error="Raccolta dati non riuscita: riprovo")
            self.stop.wait(self.args.interval)

def serve(args):
    monitor = Monitor(args)
    csrf = secrets.token_urlsafe(32)
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args): pass
        def send(self, status, raw, mime="application/json"):
            self.send_response(status)
            self.send_header("Content-Type", mime)
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers(); self.wfile.write(raw)
        def allowed(self):
            expected = "127.0.0.1:"+str(args.port)
            origin = self.headers.get("Origin")
            return self.headers.get("Host") == expected and (not origin or origin == "http://"+expected)
        def do_GET(self):
            if not self.allowed(): return self.send(403, b'{}')
            if self.path == "/":
                page = (HERE/"dashboard.html").read_text(encoding="utf-8").replace("__CSRF__", csrf)
                return self.send(200, page.encode(), "text/html; charset=utf-8")
            if self.path == "/api/state":
                if not hmac.compare_digest(self.headers.get("X-Deck-Key", ""), csrf): return self.send(401, b'{}')
                with monitor.lock: raw = json.dumps(monitor.snapshot, ensure_ascii=False).encode()
                return self.send(200, raw)
            return self.send(404, b'{}')
        def do_POST(self):
            if not self.allowed() or not hmac.compare_digest(self.headers.get("X-Deck-Key", ""), csrf): return self.send(403, b'{}')
            if self.path=='/api/device/pair':
                try:
                    size=int(self.headers.get('Content-Length','0'))
                    if not 0<size<=1024:raise ValueError('Dimensione richiesta non valida')
                    body=json.loads(self.rfile.read(size));target=monitor.store.directory/'device.json'
                    pair_device(body.get('url',''),body.get('code',''),target)
                    monitor.args.device_config=str(target)
                    return self.send(200,b'{"ok":true}')
                except Exception:return self.send(400,json.dumps({'error':'Associazione non riuscita. Verifica IP LAN e codice sul dispositivo.'}).encode())
            # Dashboard cannot execute shell commands, stop foreign processes or approve prompts.
            return self.send(405, b'{"error":"Usa la sessione originale o il comando CLI"}')
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    threading.Thread(target=monitor.loop, daemon=True).start()
    print(f"Agent Deck: http://127.0.0.1:{args.port}/", flush=True)
    try: server.serve_forever()
    except KeyboardInterrupt: pass
    finally: monitor.stop.set(); server.server_close()

def tracked_run(args):
    command = args.command
    if command and command[0] == "--": command = command[1:]
    if not command: raise SystemExit("Indica un comando dopo --")
    store = Store(args.data)
    key = str(uuid.uuid4())
    cwd = str(Path(args.cwd).resolve())
    repo = repository(cwd) or {}
    log = store.directory/(key+".log")
    item = dict(id=key, agent=args.agent, name=clean(args.title, 100), kind=args.kind, status="working", activity="Avvio processo",
                cwd=cwd, repo=Path(cwd).name, branch=repo.get("branch", ""), commit=repo.get("commit", ""), source="tracked_process",
                observed_at=time.time(), started_at=time.time(), log_id=key, pid=None, exit_code=None, notes="", resume="", capabilities=["observe"])
    store.put(item)
    with log.open("wb") as output:
        try:
            process = subprocess.Popen(command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT, creationflags=CREATE_FLAGS)
            item.update(pid=process.pid, activity="Processo in esecuzione");store.put(item)
            print("JOB "+key, flush=True)
            code = process.wait()
        except KeyboardInterrupt:
            process.terminate(); process.wait(); code = 130
        except OSError:
            code = 127
    latest = store.get(key) or item
    latest.update(status="done" if code == 0 else "error", activity="Completato" if code == 0 else "Errore: consulta il log locale",
                  exit_code=code, ended_at=time.time(), observed_at=time.time())
    store.put(latest)
    print("EXIT "+str(code), flush=True)
    return code

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", default=str(Path.home()/".agent-deck"))
    sub = parser.add_subparsers(dest="op", required=True)
    p = sub.add_parser("serve");p.add_argument("--workspace", default=str(Path.cwd()));p.add_argument("--port", type=int, default=9784)
    p.add_argument('--aetherion-data',help='Optional local Aetherion owner-state directory, read-only')
    p.add_argument("--interval", type=float, default=5);p.add_argument("--device-config")
    p.add_argument("--sim-dir", help="Mostra anche le sessioni simulate scritte da 'sim' in questa cartella")
    p.add_argument("--live-quota",action='store_true',help='Legge le quote Codex via App Server ogni due minuti')
    p = sub.add_parser("run");p.add_argument("--title", required=True);p.add_argument("--agent", choices=["codex", "claude", "manual"], default="codex")
    p.add_argument("--kind", choices=["build", "test", "release", "process"], default="build");p.add_argument("--cwd", default=str(Path.cwd()));p.add_argument("command", nargs=argparse.REMAINDER)
    p = sub.add_parser("event");p.add_argument("--id", required=True);p.add_argument("--title", required=True);p.add_argument("--agent", choices=["codex", "claude", "manual"], default="codex")
    p.add_argument("--status", choices=sorted(STATES), required=True);p.add_argument("--activity", default="");p.add_argument("--notes", default="");p.add_argument("--resume", default="");p.add_argument("--cwd", default=str(Path.cwd()))
    p = sub.add_parser("sim", help="Scrive stati hook sintetici (working/needs_input/done) in una cartella separata");p.add_argument("--dir", default=str(Path.home()/".agent-deck-sim"))
    p.add_argument("--interval", type=float, default=4);p.add_argument("--steps", type=int, default=0, help="0 = fino a Ctrl+C")
    sub.add_parser("hook", help="Claude Code hook: legge il payload da stdin, non stampa nulla")
    p = sub.add_parser("snapshot");p.add_argument("--workspace", default=str(Path.cwd()))
    sub.add_parser('quota',help='Legge e salva solo percentuali e reset Codex, senza credenziali')
    p=sub.add_parser('pair');p.add_argument('--url',required=True);p.add_argument('--code',required=True)
    args = parser.parse_args()
    if args.op == "serve":
        if not 1 <= args.interval <= 120: parser.error("Intervallo consentito: 1-120 secondi")
        serve(args)
    elif args.op == "run": return tracked_run(args)
    elif args.op == "event":
        repo = repository(args.cwd) or {}; store = Store(args.data); old = store.get(args.id) or {}
        old.update(id=args.id, name=clean(args.title, 100), agent=args.agent, status=args.status, activity=clean(args.activity), notes=clean(args.notes, 2000),
                   resume=clean(args.resume, 300), cwd=str(Path(args.cwd).resolve()), repo=Path(args.cwd).name, branch=repo.get("branch", ""), commit=repo.get("commit", ""),
                   source="explicit_event", observed_at=time.time(), capabilities=["observe", "resume_in_terminal"])
        store.put(old);print("EVENT RECORDED")
    elif args.op == "hook":
        # Never block or fail the Claude session: any problem is silently dropped.
        try: record_hook(args.data, json.loads(sys.stdin.buffer.read(1048576) or b"{}"))
        except Exception: pass
    elif args.op == "sim":
        try: run_sim(args.dir, args.interval, args.steps)
        except KeyboardInterrupt: pass
        except ValueError as exc: parser.error(str(exc))
    elif args.op == "snapshot":
        print(json.dumps(Monitor(args).collect(), ensure_ascii=False, indent=2))
    elif args.op == 'quota':
        from codex_quota import read_quota
        result=read_quota();store=Store(args.data);(store.directory/'account_quota.json').write_text(json.dumps(result),encoding='utf-8')
        print('Quote Codex aggiornate:',len(result['windows']),'finestre')
    elif args.op == 'pair':
        store=Store(args.data);pair_device(args.url,args.code,store.directory/'device.json');print('Dispositivo associato. Token conservato solo nel file privato del monitor.')
    return 0

if __name__ == "__main__": sys.exit(main())
