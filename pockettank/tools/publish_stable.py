"""Publish Pocket Tank on the stable Market channel.

Uploads the model (release pockettank-model-v4, downloaded by the app on first
start: firmware/main/pt_watch_os.h PT_MODEL_URL), then the app images of the
chosen boards (release pockettank-vVERSION) and replaces the Pocket Tank entries
of market/catalog.json. Dry run by default; --model-only uploads just the model.
"""
import argparse, base64, hashlib, json, subprocess, urllib.error, urllib.request
from pathlib import Path

VERSION = "0.3.8"
TAG = f"pockettank-v{VERSION}"
MODEL_TAG = "pockettank-model-v4"
MODEL_SHA256 = "50bed3e16a6b11f3496dde21d8784da9067f40ec9f0a77cf97f60a95acb80f6b"
REPO = "matteo-forcinito/watch-firmware-releases"
BOARDS = {  # Market board id -> (board repo, project)
    "amoled_watch": ("ESP32-S3-Amoled-Touch-1.8", "pockettank_amoled"),
    "amoled175c_watch": ("ESP32-S3-Touch-AMOLED-1.75C", "pockettank_amoled175c"),
    "amoled175_watch": ("ESP32-S3-Touch-AMOLED-1.75", "pockettank_amoled175"),
}
SUMMARY = "Acquario con pesci guidati da un'AI locale."  # the catalogue stays under 48 KB for older firmware
NOTES = ("Pocket Tank 0.3.8 (github.com/mediacutlet/pocket-tank, MIT) come app esterna: un modello da 14M "
         "parametri gira sull'orologio e decide cosa fanno i pesci, senza cloud. Al primo avvio scarica il "
         "modello (7,5 MB) con il Wi-Fi salvato nell'OS. BOOT torna al launcher.")

p = argparse.ArgumentParser()
p.add_argument("--publish", action="store_true")
p.add_argument("--model-only", action="store_true")
p.add_argument("--model", type=Path, required=True, help="model_q4.bin")
p.add_argument("--boards", nargs="+", choices=BOARDS, default=list(BOARDS))
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
model = a.model.read_bytes()
assert hashlib.sha256(model).hexdigest() == MODEL_SHA256, "model differs from the pinned upstream model"
icon = (root / "watch-os-common/pockettank/assets/icon.png").read_bytes()
prefix = f"https://github.com/{REPO}/releases/download/{TAG}/"

assets, entries = {}, []
for board in ([] if a.model_only else a.boards):
    folder, project = BOARDS[board]
    binary = (root / folder / "external_apps/pockettank/build" / f"{project}.bin").read_bytes()
    assert binary[0] == 0xE9, f"{project}: not an app image"
    assert binary[80:112].split(b"\0", 1)[0] == project.encode(), f"{project}: wrong project"
    assert binary[48:80].split(b"\0", 1)[0] == VERSION.encode(), f"{project}: wrong version"
    assert len(binary) <= 0x260000, f"{project}: the app overruns the model's first run"
    names = (f"PocketTank-{board}-{VERSION}.bin", f"PocketTank-{board}-{VERSION}.png")
    assets[names[0]], assets[names[1]] = binary, icon
    item = lambda data, name: {"size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "url": prefix + name}
    entries.append({"id": "PocketTank", "name": "Pocket Tank", "version": VERSION, "board": board, "project": project,
                    "summary": SUMMARY, "channel": "stable", "color": "46C8D8",
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
    with urllib.request.urlopen(urllib.request.Request(url, data=data, method=method, headers=headers), timeout=600) as response:
        return json.load(response)


def release_with(tag, title, body, files):
    try:
        release = req(api + "/releases/tags/" + tag)
    except urllib.error.HTTPError as e:
        if e.code != 404:
            raise
        release = req(api + "/releases", "POST", {"tag_name": tag, "name": title, "draft": True,
                                                  "prerelease": False, "body": body})
    uploaded = {x["name"]: x for x in req(api + f"/releases/{release['id']}/assets?per_page=100")}
    for name, data in sorted(files.items()):
        item = uploaded.get(name) or req(release["upload_url"].split("{")[0] + "?name=" + name, "POST", data, True)
        assert item["size"] == len(data) and item.get("digest") == "sha256:" + hashlib.sha256(data).hexdigest(), name
        print("Uploaded", name, flush=True)
    req(api + f"/releases/{release['id']}", "PATCH", {"draft": False, "prerelease": False, "make_latest": "false"})
    return release


release_with(MODEL_TAG, "Pocket Tank model v4", "pocket-tank 14.3M Q4 model, upstream 3df1049 (MIT).",
             {"model_q4.bin": model})
if a.model_only:
    raise SystemExit(0)
release = release_with(TAG, f"Pocket Tank {VERSION}", NOTES, assets)

path = api + "/contents/market/catalog.json"
for attempt in range(5):
    old = req(path + "?ref=main")
    previous = json.loads(base64.b64decode(old["content"]))
    preserved = [x for x in previous["apps"] if x["id"] != "PocketTank"]
    merged = {"schema": 2, "apps": preserved + entries}
    encoded = json.dumps(merged, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"
    assert len(encoded) <= 48 * 1024
    for board in {x["board"] for x in merged["apps"]}:
        assert sum(x["board"] == board for x in merged["apps"]) <= 32, board
    try:
        result = req(path, "PUT", {"message": f"Pocket Tank {VERSION} on the stable channel", "branch": "main",
                                   "sha": old["sha"], "content": base64.b64encode(encoded).decode()})
        break
    except urllib.error.HTTPError as e:
        if e.code != 409 or attempt == 4:
            raise
assert base64.b64decode(req(path + "?ref=main")["content"]) == encoded
print("PUBLISHED", release["html_url"], "catalog", result["commit"]["sha"][:10], "entries", len(merged["apps"]), flush=True)
