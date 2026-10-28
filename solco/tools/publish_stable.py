"""Publish Solco on the stable Market channel for every board.

Takes the build-stable images of the six wrappers, checks project name and
version in each image header, uploads them to a GitHub release and replaces
the Solco entries (any channel) of market/catalog.json. Dry run by default.
"""
import argparse, base64, hashlib, json, subprocess, urllib.error, urllib.request
from pathlib import Path

VERSION = "0.2.0"
TAG = f"solco-v{VERSION}"
REPO = "matteo-forcinito/watch-firmware-releases"
BOARDS = {  # Market board id -> (wrapper folder, project)
    "amoled_watch": ("ESP32-S3-Amoled-Touch-1.8", "solco_amoled"),
    "knob_os": ("ESP32-S3-Knob-Touch-LCD-1.8", "solco_knob"),
    "watcher_os": ("SenseCap-Watcher-W1-A", "solco_watcher"),
    "amoled175_watch": ("ESP32-S3-Touch-AMOLED-1.75", "solco_amoled175"),
    "amoled175c_watch": ("ESP32-S3-Touch-AMOLED-1.75C", "solco_amoled175c"),
    "guition4848_panel": ("ESP32-4848S040C-I", "solco_guition"),
}
SUMMARY = "DJ e Studio con kit multi-device."  # the catalogue stays under 48 KB for older firmware
NOTES = ("Solco 0.2.0. Nuova interfaccia Neon Noir con volume nel menu MASTER (tasto BOOT), "
         "velocita 0.5/1/1.5/2x nei due versi, pitch fine +-8%, loop shift, hot cue 1-4, waveform a 3 bande. "
         "Kit multi-device dalla pagina KIT: CREA KIT / UNISCITI, audio condiviso via ESP-NOW con chiavi ECOS, "
         "uscite TUTTI / SOLO HUB / SEGUI MIX. Latenza di uscita ridotta. Preascolto in cuffia e Knob non ancora "
         "ottimizzati. Richiede Watch OS 1.17.3.")

p = argparse.ArgumentParser()
p.add_argument("--publish", action="store_true")
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
icon = (root / "watch-os-common/solco/assets/icon.png").read_bytes()
prefix = f"https://github.com/{REPO}/releases/download/{TAG}/"

assets, entries = {}, []
for board, (folder, project) in BOARDS.items():
    binary = (root / folder / "external_apps/solco/build-stable" / f"{project}.bin").read_bytes()
    assert binary[0] == 0xE9, f"{project}: not an app image"
    assert binary[80:112].split(b"\0", 1)[0] == project.encode(), f"{project}: wrong project"
    assert binary[48:80].split(b"\0", 1)[0] == VERSION.encode(), f"{project}: wrong version"
    names = (f"Solco-{board}-{VERSION}.bin", f"Solco-{board}-{VERSION}.png")
    assets[names[0]], assets[names[1]] = binary, icon
    item = lambda data, name: {"size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "url": prefix + name}
    entries.append({"id": "Solco", "name": "Solco", "version": VERSION, "board": board, "project": project,
                    "summary": SUMMARY, "channel": "stable", "color": "69E4CE",
                    "binary": item(binary, names[0]), "icon": item(icon, names[1])})
    print("Verified", board, project, len(binary), flush=True)
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
    with urllib.request.urlopen(urllib.request.Request(url, data=data, method=method, headers=headers), timeout=120) as response:
        return json.load(response)


try:
    release = req(api + "/releases/tags/" + TAG)
except urllib.error.HTTPError as e:
    if e.code != 404:
        raise
    release = req(api + "/releases", "POST", {"tag_name": TAG, "name": f"Solco {VERSION}", "draft": True,
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
    preserved = [x for x in previous["apps"] if x["id"] != "Solco"]
    merged = {"schema": 2, "apps": preserved + entries}
    encoded = json.dumps(merged, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"
    assert len(encoded) <= 48 * 1024
    for board in {x["board"] for x in merged["apps"]}:
        assert sum(x["board"] == board for x in merged["apps"]) <= 32, board
    try:
        result = req(path, "PUT", {"message": f"Solco {VERSION} on the stable channel", "branch": "main",
                                   "sha": old["sha"], "content": base64.b64encode(encoded).decode()})
        break
    except urllib.error.HTTPError as e:
        if e.code != 409 or attempt == 4:
            raise
assert base64.b64decode(req(path + "?ref=main")["content"]) == encoded
print("PUBLISHED", release["html_url"], "catalog", result["commit"]["sha"][:10], "entries", len(merged["apps"]), flush=True)
