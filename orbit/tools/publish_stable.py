"""Publish Orbit on the stable Market channel.

Uploads the app images of the chosen boards (release orbit-vVERSION of
watch-firmware-releases) and replaces only those boards' Orbit entries in
the Market catalogues (tools/market_catalog.py); other boards keep the version they have. Dry run by
default: --publish uploads and edits the catalogue.

Build first: in <board repo>/external_apps/orbit run idf.py build (release sdkconfig, no
development console).
"""
import argparse, base64, hashlib, json, subprocess, sys, urllib.error, urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import market_catalog  # noqa: E402

VERSION = "2.0.0"
TAG = f"orbit-v{VERSION}"
REPO = "matteo-forcinito/watch-firmware-releases"
BOARDS = {  # Market board id -> board repo
    "amoled_watch": "ESP32-S3-Amoled-Touch-1.8",
    "amoled175_watch": "ESP32-S3-Touch-AMOLED-1.75",
    "amoled175c_watch": "ESP32-S3-Touch-AMOLED-1.75C",
    "knob_os": "ESP32-S3-Knob-Touch-LCD-1.8",
    "watcher_os": "SenseCap-Watcher-W1-A",
}
# Short summary and asset names: the shared catalogue (older firmware) is close to 48 KB.
SUMMARY = "Respiro, meditazioni guidate, suoni per dormire e concentrarsi."
COLOR = "9C7CFF"
NOTES = ("Orbit 2.0: pianeta fluido che respira sul tempo reale, ritmi 5-5, 4-4-4-4, 4-7-8 e 4-8, quattro "
         "meditazioni guidate, suoni generati (pioggia, onde, vento, rumore rosa, marrone, bianco) con "
         "spegnimento a tempo, toni guida anche a schermo spento, minuti degli ultimi 7 giorni.")

p = argparse.ArgumentParser()
p.add_argument("--publish", action="store_true")
p.add_argument("--boards", nargs="+", choices=BOARDS, default=list(BOARDS))
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
prefix = f"https://github.com/{REPO}/releases/download/{TAG}/"

assets, entries = {}, []
for board in a.boards:
    app = root / BOARDS[board] / "external_apps/orbit"
    binary = (app / "build/orbit.bin").read_bytes()
    icon = (app / "icon.png").read_bytes()
    assert binary[0] == 0xE9, f"{board}: not an app image"
    assert binary[80:112].split(b"\0", 1)[0] == b"orbit", f"{board}: wrong project"
    assert binary[48:80].split(b"\0", 1)[0] == VERSION.encode(), f"{board}: wrong version"
    assert b"CONFIG_WATCH_DEV_CONSOLE=y" not in (app / "sdkconfig").read_bytes(), f"{board}: console build"
    names = (f"{board}.bin", "icon.png")  # one icon for every board
    assert names[1] not in assets or assets[names[1]] == icon, f"{board}: icon differs"
    assets[names[0]], assets[names[1]] = binary, icon
    item = lambda data, name: {"size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "url": prefix + name}
    entries.append({"id": "Orbit", "name": "ORBIT", "version": VERSION, "board": board, "project": "orbit",
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
    release = req(api + "/releases", "POST", {"tag_name": TAG, "name": f"Orbit {VERSION}", "draft": True,
                                              "prerelease": False, "body": NOTES})
uploaded = {x["name"]: x for x in req(api + f"/releases/{release['id']}/assets?per_page=100")}
for name, data in sorted(assets.items()):
    item = uploaded.get(name) or req(release["upload_url"].split("{")[0] + "?name=" + name, "POST", data, True)
    assert item["size"] == len(data) and item.get("digest") == "sha256:" + hashlib.sha256(data).hexdigest(), name
    print("Uploaded", name, flush=True)
req(api + f"/releases/{release['id']}", "PATCH", {"draft": False, "prerelease": False, "make_latest": "false"})

market_catalog.publish(market_catalog.make_req(token), entries, f"Orbit {VERSION} on the stable channel")
print("PUBLISHED", release["html_url"], flush=True)
