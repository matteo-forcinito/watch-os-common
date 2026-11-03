"""Publish Remote on the stable Market channel.

Uploads the app images of the chosen boards (release remote-vVERSION of
watch-firmware-releases) and replaces only those boards' Remote entries in
market/catalog.json; other boards keep the version they have. Dry run by
default: --publish uploads and edits the catalogue.

Build first with <board repo>/tools/build_remote.ps1 -Board <board>, without
-Development: the checks below reject a build with the development console.
"""
import argparse, base64, hashlib, json, subprocess, urllib.error, urllib.request
from pathlib import Path

VERSION = "1.3.1"
TAG = f"remote-v{VERSION}"
REPO = "matteo-forcinito/watch-firmware-releases"
# Publish only boards verified on hardware with this version.
BOARDS = {  # Market board id -> board repo
    "amoled_watch": "ESP32-S3-Amoled-Touch-1.8",
    "amoled175_watch": "ESP32-S3-Touch-AMOLED-1.75",
    "knob_os": "ESP32-S3-Knob-Touch-LCD-1.8",
    "watcher_os": "SenseCap-Watcher-W1-A",
}
SUMMARY = "Mouse a gesti, tastiera, media, Deck per il PC; hub per più orologi."
COLOR = "45C8D8"
NOTES = ("Remote 1.3.1: air mouse a gesti come il mouse (tocco = clic, dito e polso fermi = destro, "
         "bordi per scorrimento, volume e luminosità; con \"Sempre attivo\" dito + polso trascina). "
         "Profili del Deck in un componente condiviso con Deck Bar.")

p = argparse.ArgumentParser()
p.add_argument("--publish", action="store_true")
p.add_argument("--boards", nargs="+", choices=BOARDS, default=["amoled_watch"])
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
prefix = f"https://github.com/{REPO}/releases/download/{TAG}/"

assets, entries = {}, []
for board in a.boards:
    app = root / BOARDS[board] / "external_apps/remote"
    binary = (app / "build-remote/remote.bin").read_bytes()
    icon = (app / "icon.png").read_bytes()
    assert binary[0] == 0xE9, f"{board}: not an app image"
    assert binary[80:112].split(b"\0", 1)[0] == b"remote", f"{board}: wrong project"
    assert binary[48:80].split(b"\0", 1)[0] == VERSION.encode(), f"{board}: wrong version"
    assert b"CONFIG_WATCH_DEV_CONSOLE=y" not in (app / "build-remote/sdkconfig").read_bytes(), f"{board}: console build"
    names = (f"Remote-{board}-{VERSION}.bin", f"Remote-{board}-{VERSION}.png")
    assets[names[0]], assets[names[1]] = binary, icon
    item = lambda data, name: {"size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "url": prefix + name}
    entries.append({"id": "Remote", "name": "Remote", "version": VERSION, "board": board, "project": "remote",
                    "summary": SUMMARY, "channel": "stable", "color": COLOR,
                    "binary": item(binary, names[0]), "icon": item(icon, names[1])})
    print("Verified", board, len(binary), hashlib.sha256(binary).hexdigest()[:12], flush=True)
if not a.publish:
    raise SystemExit(0)

r = subprocess.run(["git", "-c", "safe.directory=*", "credential", "fill"], input="protocol=https\nhost=github.com\n\n",
                   capture_output=True, text=True, check=True)
token = dict(x.split("=", 1) for x in r.stdout.splitlines() if "=" in x)["password"]
api = f"https://api.github.com/repos/{REPO}"


def req(url, method="GET", data=None, binary=False):
    assert url.startswith(("https://api.github.com/", "https://uploads.github.com/"))
    headers = {"Authorization": "Bearer " + token, "Accept": "application/vnd.github+json",
               "X-GitHub-Api-Version": "2022-11-28"}
    if data is not None:
        headers["Content-Type"] = "application/octet-stream" if binary else "application/json"
        if not binary:
            data = json.dumps(data).encode()
    with urllib.request.urlopen(urllib.request.Request(url, data=data, method=method, headers=headers), timeout=600) as response:
        return json.load(response)


try:
    release = req(api + "/releases/tags/" + TAG)
except urllib.error.HTTPError as e:
    if e.code != 404:
        raise
    release = req(api + "/releases", "POST", {"tag_name": TAG, "name": f"Remote {VERSION}", "draft": True,
                                              "prerelease": False, "body": NOTES})
uploaded = {x["name"]: x for x in req(api + f"/releases/{release['id']}/assets?per_page=100")}
for name, data in sorted(assets.items()):
    item = uploaded.get(name) or req(release["upload_url"].split("{")[0] + "?name=" + name, "POST", data, True)
    assert item["size"] == len(data) and item.get("digest") == "sha256:" + hashlib.sha256(data).hexdigest(), name
    print("Uploaded", name, flush=True)
req(api + f"/releases/{release['id']}", "PATCH", {"draft": False, "prerelease": False, "make_latest": "false"})

path = api + "/contents/market/catalog.json"
for attempt in range(5):
    old = req(path + "?ref=main")
    previous = json.loads(base64.b64decode(old["content"]))
    replaced = {(x["id"], x["board"]) for x in entries}
    merged = {"schema": 2, "apps": [x for x in previous["apps"] if (x["id"], x["board"]) not in replaced] + entries}
    encoded = json.dumps(merged, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"
    assert len(encoded) <= 48 * 1024, f"catalogue {len(encoded)} bytes: older firmware rejects it"
    try:
        result = req(path, "PUT", {"message": f"Remote {VERSION} on the stable channel", "branch": "main",
                                   "sha": old["sha"], "content": base64.b64encode(encoded).decode()})
        print("Catalogue", len(encoded), "bytes, commit", result["commit"]["sha"][:12])
        break
    except urllib.error.HTTPError as e:
        if e.code != 409:
            raise
else:
    raise SystemExit("catalogue changed five times while publishing")
