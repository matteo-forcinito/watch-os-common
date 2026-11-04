"""Market catalogues on watch-firmware-releases: one per board, plus the shared one.

    market/<board>.json   read first by firmware 1.17.4 and later (only that board's apps,
                          up to 128 KB = MARKET_CATALOG_MAX)
    market/catalog.json   every board, read by older firmware: 48 KB at most. When an update
                          does not fit there, older firmware keeps the version it had.

Every publishing script calls publish() with its new entries, so the board catalogues
never fall behind the shared one.

    python market_catalog.py --split      create the board catalogues from catalog.json
    python market_catalog.py --status     sizes and entries per catalogue
"""
from __future__ import annotations

import argparse
import base64
import json
import subprocess
import urllib.error
import urllib.request

REPO = "matteo-forcinito/watch-firmware-releases"
API = f"https://api.github.com/repos/{REPO}"
SHARED = "market/catalog.json"
SHARED_MAX = 48 * 1024        # firmware before 1.17.4
BOARD_MAX = 128 * 1024        # MARKET_CATALOG_MAX
BOARD_ENTRIES_MAX = 32        # MARKET_MAX_APPS: entries one board can list


def token() -> str:
    r = subprocess.run(["git", "-c", "safe.directory=*", "credential", "fill"],
                       input="protocol=https\nhost=github.com\n\n", capture_output=True, text=True, check=True)
    return dict(x.split("=", 1) for x in r.stdout.splitlines() if "=" in x)["password"]


def make_req(tok: str):
    def req(url, method="GET", data=None, binary=False):
        assert url.startswith(("https://api.github.com/", "https://uploads.github.com/"))
        headers = {"Authorization": "Bearer " + tok, "Accept": "application/vnd.github+json",
                   "X-GitHub-Api-Version": "2022-11-28"}
        body = None
        if data is not None:
            headers["Content-Type"] = "application/octet-stream" if binary else "application/json"
            body = data if binary else json.dumps(data).encode()
        with urllib.request.urlopen(urllib.request.Request(url, body, headers, method=method), timeout=120) as r:
            raw = r.read()
        return json.loads(raw) if raw else {}
    return req


def board_path(board: str) -> str:
    return f"market/{board}.json"


def encode(apps: list) -> bytes:
    return json.dumps({"schema": 2, "apps": apps}, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"


def read(req, path: str):
    """(apps, sha) of a catalogue file, (None, None) when it does not exist."""
    try:
        old = req(f"{API}/contents/{path}?ref=main")
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None, None
        raise
    return json.loads(base64.b64decode(old["content"]))["apps"], old["sha"]


def write(req, path: str, apps: list, sha, message: str) -> str:
    body = {"message": message, "branch": "main", "content": base64.b64encode(encode(apps)).decode()}
    if sha:
        body["sha"] = sha
    return req(f"{API}/contents/{path}", "PUT", body)["commit"]["sha"]


def same(a: dict, b: dict) -> bool:
    return a["id"] == b["id"] and a["board"] == b["board"] and a.get("channel") == b.get("channel")


def merge(old: list, entries: list, drop) -> list:
    return [x for x in old if not drop(x) and not any(same(x, e) for e in entries)] + entries


def publish(req, entries: list, message: str, drop=lambda x: False) -> None:
    """Replaces `entries` (same id, board, channel) and removes the old entries `drop` selects,
    first in each board catalogue, then in the shared one if it still fits in 48 KB."""
    boards = sorted({e["board"] for e in entries})
    shared_apps, _ = read(req, SHARED)
    for board in boards:
        for attempt in range(5):
            old, sha = read(req, board_path(board))
            if old is None:  # first time: start from what the shared catalogue lists for this board
                old = [x for x in shared_apps if x["board"] == board]
            mine = [e for e in entries if e["board"] == board]
            apps = merge(old, mine, lambda x: x["board"] != board or drop(x))
            size = len(encode(apps))
            assert size <= BOARD_MAX, f"{board}: catalogue {size} bytes > {BOARD_MAX}"
            assert len(apps) <= BOARD_ENTRIES_MAX, f"{board}: {len(apps)} entries > {BOARD_ENTRIES_MAX}"
            try:
                commit = write(req, board_path(board), apps, sha, f"{message} ({board})")
                print(f"Catalogo {board}: {len(apps)} app, {size} byte, commit {commit[:10]}", flush=True)
                break
            except urllib.error.HTTPError as e:
                if e.code != 409:
                    raise
        else:
            raise SystemExit(f"{board}: catalogue changed five times while publishing")
    for attempt in range(5):
        old, sha = read(req, SHARED)
        apps = merge(old, entries, drop)
        size = len(encode(apps))
        if size > SHARED_MAX:
            print(f"Catalogo comune: {size} byte > 48 KB, non aggiornato (i firmware prima di 1.17.4 "
                  "restano alla versione precedente)", flush=True)
            return
        try:
            commit = write(req, SHARED, apps, sha, message)
            print(f"Catalogo comune: {len(apps)} app, {size} byte, commit {commit[:10]}", flush=True)
            return
        except urllib.error.HTTPError as e:
            if e.code != 409:
                raise
    raise SystemExit("shared catalogue changed five times while publishing")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--split", action="store_true", help="create missing board catalogues from catalog.json")
    p.add_argument("--status", action="store_true")
    a = p.parse_args()
    req = make_req(token())
    shared, _ = read(req, SHARED)
    print(f"{SHARED}: {len(shared)} app, {len(encode(shared))} byte")
    for board in sorted({x["board"] for x in shared}):
        apps, sha = read(req, board_path(board))
        if apps is None and a.split:
            apps = [x for x in shared if x["board"] == board]
            commit = write(req, board_path(board), apps, None, f"Market catalogue for {board}")
            print(f"Creato {board_path(board)}: {len(apps)} app, commit {commit[:10]}")
        elif apps is None:
            print(f"{board_path(board)}: manca")
        else:
            print(f"{board_path(board)}: {len(apps)} app, {len(encode(apps))} byte")


if __name__ == "__main__":
    main()
