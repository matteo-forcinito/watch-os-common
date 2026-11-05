"""Publish the AMOLED 1.75C app-bay builds of the external apps on the Market.

Every app of ESP32-S3-Touch-AMOLED-1.75C/external_apps with a build-bay image
(built with components/extapp_sdk/sdkconfig.bay, see docs/BAIA-APP.md) is
checked (app image, project, version), uploaded to one GitHub release and
listed in the amoled175c_watch catalogue with "sdk": 2, so the Market can
install it in any bay slot. Name, summary, colour and icon are reused from the
app's existing Market entry (this board first, then the others); a new app
needs NEW below. Dry run by default; --publish to do it.

    python tools/publish_bay_apps.py [--publish] [app ...]
"""
import argparse
import hashlib
import sys
import urllib.error
from datetime import date
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import market_catalog as mc  # noqa: E402

BOARD = "amoled175c_watch"
WRAPPERS = Path(__file__).resolve().parents[2] / "ESP32-S3-Touch-AMOLED-1.75C" / "external_apps"
ID = {"pockettank": "PocketTank"}   # folder -> Market id when it is not just capitalised
BUILD_DIR = {"astri": "build"}      # apps whose normal build is already the bay configuration
NEW = {  # apps never published before: their Market metadata and icon file
    "Astri": {"summary": "Il cielo sopra di te, offline: stelle, costellazioni, pianeti e Luna.",
              "color": "7FA8FF", "icon": WRAPPERS / "astri" / "icon.png"},
}
TAG = "bay-175c-" + date.today().strftime("%Y%m%d")
PREFIX = f"https://github.com/{mc.REPO}/releases/download/{TAG}/"
OTHER_BOARDS = ["amoled175_watch", "amoled_watch", "watcher_os", "knob_os", "guition4848_panel"]


def header_text(image: bytes, start: int, end: int) -> str:
    return image[start:end].split(b"\0", 1)[0].decode()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--publish", action="store_true")
    parser.add_argument("apps", nargs="*")
    args = parser.parse_args()

    req = mc.make_req(mc.token())
    catalogues = {}
    for board in [BOARD] + OTHER_BOARDS:
        apps, _ = mc.read(req, mc.board_path(board))
        catalogues[board] = apps or []

    def existing(app_id):
        for board in [BOARD] + OTHER_BOARDS:
            for entry in catalogues[board]:
                if entry["id"] == app_id and entry.get("channel") == "stable":
                    return entry
        return None

    folders = args.apps or sorted(p.name for p in WRAPPERS.iterdir() if (p / BUILD_DIR.get(p.name, "build-bay")).is_dir())
    entries, assets = [], {}
    for folder in folders:
        build = WRAPPERS / folder / BUILD_DIR.get(folder, "build-bay")   # never a non-bay build
        images = [p for p in build.glob("*.bin") if "ota_data" not in p.name and "flashed" not in p.name]
        if len(images) != 1:
            print(f"SKIP {folder}: {len(images)} images in {build.name}")
            continue
        image = images[0].read_bytes()
        project, version = header_text(image, 80, 112), header_text(image, 48, 80)
        assert image[0] == 0xE9, f"{folder}: not an app image"
        app_id = ID.get(folder, folder.capitalize())
        old = existing(app_id)
        if old is None and app_id not in NEW:
            print(f"SKIP {folder}: no Market entry to take name and icon from")
            continue
        if old is not None:
            name, summary, color, icon_item = old["name"], old["summary"], old.get("color", "7B6CFF"), old["icon"]
            source = old["board"]
        else:
            meta = NEW[app_id]
            icon = meta["icon"].read_bytes()
            icon_name = f"{app_id}-{BOARD}-{version}.png"
            assets[icon_name] = icon
            name, summary, color, source = app_id, meta["summary"], meta["color"], "nuova"
            icon_item = {"size": len(icon), "sha256": hashlib.sha256(icon).hexdigest(), "url": PREFIX + icon_name}
        bin_name = f"{app_id}-{BOARD}-{version}-bay.bin"
        assets[bin_name] = image
        entries.append({"id": app_id, "name": name, "version": version, "board": BOARD, "project": project,
                        "summary": summary, "channel": "stable", "color": color, "sdk": 2,
                        "binary": {"size": len(image), "sha256": hashlib.sha256(image).hexdigest(), "url": PREFIX + bin_name},
                        "icon": icon_item})
        print(f"{app_id:12} {version:8} {project:22} {len(image) / 1048576:5.2f} MB  metadati da {source}")

    if not args.publish or not entries:
        print(f"{len(entries)} app pronte. Dry run: aggiungi --publish per pubblicare.")
        return

    api = f"https://api.github.com/repos/{mc.REPO}"
    try:
        release = req(api + "/releases/tags/" + TAG)
    except urllib.error.HTTPError as e:
        if e.code != 404:
            raise
        release = req(api + "/releases", "POST", {"tag_name": TAG, "name": f"App per la baia 1.75C ({TAG[-8:]})",
                                                  "draft": True, "prerelease": False,
                                                  "body": "App esterne compilate per la baia app della AMOLED 1.75C "
                                                          "(slot oltre i 16 MB, extapp_sdk 2)."})
    uploaded = {x["name"]: x for x in req(api + f"/releases/{release['id']}/assets?per_page=100")}
    for name, data in sorted(assets.items()):
        item = uploaded.get(name) or req(release["upload_url"].split("{")[0] + "?name=" + name, "POST", data, True)
        assert item["size"] == len(data), name
        print("Caricato", name, flush=True)
    req(api + f"/releases/{release['id']}", "PATCH", {"draft": False, "prerelease": False, "make_latest": "false"})
    mc.publish(req, entries, f"App bay builds for {BOARD}")
    print("PUBBLICATO", release["html_url"])


if __name__ == "__main__":
    main()
