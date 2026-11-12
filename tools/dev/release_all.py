"""Release every firmware of the family with one version (Linux, macOS or Windows).

    python tools/dev/release_all.py status  [--version X.Y.Z]
    python tools/dev/release_all.py prepare --version X.Y.Z --notes NOTES.md [--only a,b] [--push]
    python tools/dev/release_all.py publish --version X.Y.Z [--only a,b] [--verify-only]

prepare  in each firmware: main pulled (fast-forward only), watch-os-common moved to
         origin/main of the library, release/notes/X.Y.Z.md copied, PROJECT_VER set,
         one commit "Release X.Y.Z: <first line of the notes>"; --push sends it to origin/main.
publish  CI products: tools/release_firmware.py (GitHub Actions build, verification,
         public release, OTA catalog, tag) all in parallel; local products (no CI workflow
         yet): production build here, then the same verification and publication.
         --verify-only builds and verifies everything without publishing: run it first, so
         a failing firmware never leaves the family on different versions. A later publish
         reuses the verified Actions runs.

Needs git, and for publish the official gh CLI logged in (gh auth login). Builds of local
products need ESP-IDF in the environment (export.sh). No token is read or stored here.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

COMMON = Path(__file__).resolve().parents[2]
ROOT = COMMON.parent
OWNER = "matteo-forcinito"
# product -> (repository folder, release mode): "ci" = release.yml on GitHub Actions
FIRMWARES = {
    "amoled": ("ESP32-S3-Amoled-Touch-1.8", "ci"),
    "amoled175": ("ESP32-S3-Touch-AMOLED-1.75", "ci"),
    "amoled175c": ("ESP32-S3-Touch-AMOLED-1.75C", "ci"),
    "knob": ("ESP32-S3-Knob-Touch-LCD-1.8", "ci"),
    "watcher": ("SenseCap-Watcher-W1-A", "ci"),
    "guition4848": ("ESP32-4848S040C-I", "ci"),
    "lcd349": ("ESP32-S3-Touch-LCD-3.49", "local"),
}
VERSION = re.compile(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?")
PROJECT_VER = re.compile(r'set\(PROJECT_VER "\d[^"]*"\)')


def git(repo, *args, check=True):
    r = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True)
    if check and r.returncode:
        raise SystemExit(f"{repo.name}: git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout.strip()


def selected(only):
    names = [n.strip() for n in (only or "").split(",") if n.strip()]
    unknown = [n for n in names if n not in FIRMWARES]
    if unknown:
        raise SystemExit(f"unknown products: {', '.join(unknown)} (valid: {', '.join(FIRMWARES)})")
    return {p: v for p, v in FIRMWARES.items() if not names or p in names}


def state_dir():
    d = Path(os.environ.get("RELEASE_STATE_DIR", Path.home() / ".cache/watch-release"))
    d.mkdir(parents=True, exist_ok=True)
    return d


def load_state(version):
    try:
        return json.loads((state_dir() / f"{version}.json").read_text())
    except (OSError, ValueError):
        return {}


def save_state(version, data):
    (state_dir() / f"{version}.json").write_text(json.dumps(data, indent=2))


def catalog(product):
    url = f"https://raw.githubusercontent.com/{OWNER}/watch-firmware-releases/main/{product}.json"
    try:
        with urllib.request.urlopen(url, timeout=8) as r:
            ch = json.load(r).get("channels", {})
        return {k: (v.get("version") if isinstance(v, dict) else v) for k, v in ch.items()}
    except Exception:
        return None


def summary_line(notes):
    first = next((l.strip() for l in Path(notes).read_text(encoding="utf-8").splitlines() if l.strip()), "")
    if not first or len(first) > 79:
        raise SystemExit("the first line of the notes is the watch summary: 1-79 characters")
    return first


# ------------------------------------------------------------------ status
def status(a):
    git(COMMON, "fetch", "-q", "origin", check=False)
    lib = git(COMMON, "rev-parse", "origin/main", check=False)
    st = load_state(a.version) if a.version else {}
    out = {"library": {"origin_main": lib[:7], "local": git(COMMON, "rev-parse", "--short", "HEAD", check=False),
                       "dirty": bool(git(COMMON, "status", "--porcelain", "--untracked-files=no", check=False))},
           "version": a.version, "products": []}
    for product, (folder, mode) in FIRMWARES.items():
        repo = ROOT / folder
        p = {"product": product, "repo": folder, "mode": mode, "present": (repo / ".git").exists()}
        if p["present"]:
            git(repo, "fetch", "-q", "origin", check=False)
            text = (repo / "CMakeLists.txt").read_text(encoding="utf-8")
            m = PROJECT_VER.search(text)
            p["project_ver"] = m.group(0).split('"')[1] if m else None
            sub = git(repo, "rev-parse", "HEAD:watch-os-common", check=False)
            p["os"] = sub[:7]
            p["os_latest"] = bool(lib) and sub == lib
            p["dirty"] = bool(git(repo, "status", "--porcelain", "--untracked-files=no", check=False))
            p["pushed"] = git(repo, "rev-parse", "HEAD", check=False) == git(repo, "rev-parse", "origin/main", check=False)
            p["notes"] = bool(a.version) and (repo / "release/notes" / f"{a.version}.md").is_file()
        p["catalog"] = catalog(product)
        p["run"] = st.get(product, {})
        out["products"].append(p)
    print(json.dumps(out))


# ------------------------------------------------------------------ prepare
def prepare(a):
    if not VERSION.fullmatch(a.version):
        raise SystemExit("version must be X.Y.Z or X.Y.Z-dev.N")
    summary = summary_line(a.notes)
    notes_text = Path(a.notes).read_text(encoding="utf-8")
    git(COMMON, "fetch", "-q", "origin")
    lib = git(COMMON, "rev-parse", "origin/main")
    print(f"library origin/main {lib[:7]}", flush=True)
    for product, (folder, _) in selected(a.only).items():
        repo = ROOT / folder
        print(f"== {product} ({folder})", flush=True)
        if not (repo / ".git").exists():
            raise SystemExit(f"{folder}: not cloned in {ROOT}")
        dirty = [l for l in git(repo, "status", "--porcelain", "--untracked-files=no").splitlines()
                 if not l.endswith(" watch-os-common")]
        if dirty:
            raise SystemExit(f"{folder}: uncommitted changes: {dirty[:3]}")
        sub = repo / "watch-os-common"
        if git(sub, "status", "--porcelain", "--untracked-files=no", check=False):
            raise SystemExit(f"{folder}: watch-os-common has local changes: commit them in the library first")
        git(repo, "checkout", "-q", "main")
        git(repo, "pull", "-q", "--ff-only")
        git(sub, "fetch", "-q", "origin")
        git(sub, "checkout", "-q", "--detach", lib)
        (repo / "release/notes").mkdir(parents=True, exist_ok=True)
        (repo / "release/notes" / f"{a.version}.md").write_text(notes_text, encoding="utf-8")
        cm = repo / "CMakeLists.txt"
        text = cm.read_text(encoding="utf-8")
        if not PROJECT_VER.search(text):
            raise SystemExit(f"{folder}: no set(PROJECT_VER \"...\") line")
        cm.write_text(PROJECT_VER.sub(f'set(PROJECT_VER "{a.version}")', text, count=1), encoding="utf-8")
        git(repo, "add", "watch-os-common", f"release/notes/{a.version}.md", "CMakeLists.txt")
        if git(repo, "diff", "--cached", "--name-only"):
            git(repo, "commit", "-q", "-m", f"Release {a.version}: {summary}")
            print(f"   committed {git(repo, 'rev-parse', '--short', 'HEAD')}", flush=True)
        else:
            print("   already prepared", flush=True)
        if a.push:
            git(repo, "push", "-q", "origin", "main")
            print("   pushed to origin/main", flush=True)
    print("PREPARED", flush=True)


# ------------------------------------------------------------------ publish
def publish(a):
    gh = a.gh
    if subprocess.run([gh, "auth", "status"], capture_output=True).returncode:
        raise SystemExit("gh is not logged in: run  gh auth login")
    tool = COMMON / "tools/release_firmware.py"
    out_dir = state_dir() / "output"
    logs = state_dir() / "logs"
    out_dir.mkdir(exist_ok=True)
    logs.mkdir(exist_ok=True)
    st = load_state(a.version)
    procs = {}
    products = selected(a.only)
    # nothing starts unless every selected firmware is prepared and pushed
    for product, (folder, _) in products.items():
        repo = ROOT / folder
        text = (repo / "CMakeLists.txt").read_text(encoding="utf-8")
        if f'set(PROJECT_VER "{a.version}")' not in text or not (repo / f"release/notes/{a.version}.md").is_file():
            raise SystemExit(f"{folder}: not prepared for {a.version} (run prepare)")
        git(repo, "fetch", "-q", "origin")
        if git(repo, "rev-parse", "HEAD") != git(repo, "rev-parse", "origin/main"):
            raise SystemExit(f"{folder}: HEAD is not origin/main (push it)")
    for product, (folder, mode) in products.items():
        repo = ROOT / folder
        args = [sys.executable, str(tool), "--repo", f"{OWNER}/{folder}", "--product", product,
                "--version", a.version, "--project", str(repo), "--gh", gh, "--output", str(out_dir)]
        if a.verify_only:
            args.append("--verify-only")
        if mode == "local":
            build = repo / "build"
            print(f"== {product}: production build in {build} (local product)", flush=True)
            env = dict(os.environ)
            env.pop("WATCH_LOCAL_QA_CONSOLE", None)
            env.pop("FW_VERSION", None)
            r = subprocess.run(["idf.py", "-B", str(build), "build"], cwd=repo, env=env)
            if r.returncode:
                raise SystemExit(f"{product}: production build failed")
            args += ["--local-build", str(build)]
        elif not a.verify_only and st.get(product, {}).get("verified_run"):
            args += ["--run-id", str(st[product]["verified_run"])]
        log = logs / f"{a.version}-{product}{'-verify' if a.verify_only else ''}.log"
        procs[product] = (subprocess.Popen(args, stdout=open(log, "w"), stderr=subprocess.STDOUT), log)
        print(f"== {product}: started ({'verify' if a.verify_only else 'publish'}) · log {log}", flush=True)
    failed = []
    pending = dict(procs)
    seen = {}
    while pending:
        time.sleep(5)
        for product, (proc, log) in list(pending.items()):
            text = log.read_text(errors="replace") if log.exists() else ""
            m = re.search(r"actions/runs/(\d+)", text)
            if m and seen.get(product) != m.group(1):
                seen[product] = m.group(1)
                print(f"   {product}: Actions run {m.group(1)}", flush=True)
            if proc.poll() is None:
                continue
            del pending[product]
            entry = st.setdefault(product, {})
            if m:
                entry["verified_run" if a.verify_only else "run"] = int(m.group(1))
            pub = re.search(r"PUBLISHED: (\S+)", text)
            if proc.returncode == 0:
                entry["verified" if a.verify_only else "published"] = pub.group(1) if pub else True
                print(f"OK {product}" + (f" {pub.group(1)}" if pub else " verified"), flush=True)
            else:
                failed.append(product)
                last = [l for l in text.splitlines() if l.strip()][-3:]
                entry["error"] = " | ".join(last)[:300]
                print(f"FAILED {product}: {' | '.join(last)}", flush=True)
            save_state(a.version, st)
    if failed:
        raise SystemExit(f"failed: {', '.join(failed)} — fix and rerun with --only {','.join(failed)}")
    print("ALL VERIFIED" if a.verify_only else "ALL PUBLISHED", flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="op", required=True)
    s = sub.add_parser("status")
    s.add_argument("--version")
    s = sub.add_parser("prepare")
    s.add_argument("--version", required=True)
    s.add_argument("--notes", required=True)
    s.add_argument("--only")
    s.add_argument("--push", action="store_true")
    s = sub.add_parser("publish")
    s.add_argument("--version", required=True)
    s.add_argument("--only")
    s.add_argument("--verify-only", action="store_true")
    s.add_argument("--gh", default="gh")
    a = p.parse_args()
    {"status": status, "prepare": prepare, "publish": publish}[a.op](a)


if __name__ == "__main__":
    main()
