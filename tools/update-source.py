#!/usr/bin/env python3
"""
Regenerate Henge's AltStore/SideStore source (repo.json at the top of the repository) from
the GitHub releases:

    python3 tools/update-source.py

Add a release's app version and build number to BUILDS below before running it: AltStore and
SideStore compare them to the installed app's CFBundleShortVersionString and CFBundleVersion.
The combined source at https://itslevii.dev/repo.json takes its Henge entry from this file
(the Husk repository's tools/update-altsource.py).
"""
import json
import os
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "repo.json")
RAW = "https://raw.githubusercontent.com/Leviidev/Henge/main"
KEEP = 10

# release tag -> (CFBundleShortVersionString, CFBundleVersion) of that release's app
BUILDS = {"0.3": ("0.3.0", "101"), "0.2": ("0.2.0", "100"), "0.1": ("0.1", "1")}


def gh(path):
    req = urllib.request.Request("https://api.github.com" + path,
                                 headers={"User-Agent": "henge-source", "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def versions():
    out = []
    for rel in gh("/repos/Leviidev/Henge/releases?per_page=30"):
        if rel.get("draft") or rel.get("prerelease"):
            continue
        ipa = next((a for a in rel["assets"] if a["name"].lower().endswith(".ipa")), None)
        if not ipa:
            continue
        tag = rel["tag_name"].lstrip("vV")
        if tag not in BUILDS:
            raise SystemExit(f"release {tag}: add its app version and build number to BUILDS first")
        version, build = BUILDS[tag]
        out.append({
            "version": version,
            "buildVersion": build,
            "date": rel["published_at"],
            "localizedDescription": ((rel.get("body") or "").strip() or f"Henge {tag}.")[:4000],
            "downloadURL": ipa["browser_download_url"],
            "size": ipa["size"],
            "minOSVersion": "17.0",
        })
        if len(out) >= KEEP:
            break
    return out


FIRST_SHOTS = ["macs.png", "trackpad.png", "install.png", "ipad.jpg"]  # the order the source shows them in


def screenshots():
    folder = os.path.join(ROOT, "Screenshots")
    names = [n for n in os.listdir(folder) if n.lower().endswith((".png", ".jpg", ".jpeg"))]
    names.sort(key=lambda n: (FIRST_SHOTS.index(n) if n in FIRST_SHOTS else len(FIRST_SHOTS), n))
    return [f"{RAW}/Screenshots/{n}" for n in names]


def main():
    icon = f"{RAW}/docs/icon.png"
    app = {
        "name": "Henge",
        "bundleIdentifier": "com.henge.app",
        "developerName": "Leviidev",
        "subtitle": "macOS on your iPhone and iPad.",
        "localizedDescription": (
            "Henge runs macOS on iPhone and iPad with its own emulator. It downloads Apple's restore image and "
            "installs macOS right on the device, no Mac needed. Safari and Discord run, and the clipboard and a "
            "shared folder work between macOS and the device. It's emulation, so it runs slower than a real Mac. "
            "Henge needs JIT (StikDebug, its built-in StikJIT on iOS 26 or later, TrollStore or a jailbreak), "
            "iOS 17 or later, an A14 or M1 chip or newer, and about 43 GB free."
        ),
        "iconURL": icon,
        "tintColor": "#2E6BFF",
        "category": "utilities",
        "screenshots": screenshots(),
        "versions": versions(),
        "appPermissions": {
            "entitlements": [
                "get-task-allow",
                "dynamic-codesigning",
                "com.apple.developer.kernel.increased-memory-limit",
                "com.apple.developer.kernel.extended-virtual-addressing",
            ],
            "privacy": {
                "NSLocalNetworkUsageDescription": "Henge uses the local network to pair with this iPhone for JIT, without a computer.",
                "NSCameraUsageDescription": "Henge can use the volume buttons as the trackpad's mouse buttons. Receiving those presses needs the camera active; no video is recorded, saved or shown.",
            },
        },
    }
    source = {
        "name": "Henge",
        "identifier": "dev.itslevii.henge",
        "subtitle": "macOS on your iPhone and iPad.",
        "description": "Henge's own source. Add it in SideStore or AltStore to install Henge and get its updates.",
        "iconURL": icon,
        "website": "https://github.com/Leviidev/Henge",
        "tintColor": "#2E6BFF",
        "featuredApps": [app["bundleIdentifier"]],
        "apps": [app],
        "news": [],
    }
    with open(OUT, "w") as f:
        f.write(json.dumps(source, indent=2) + "\n")
    print(f"wrote repo.json: Henge {', '.join(v['version'] for v in app['versions'])}")


if __name__ == "__main__":
    main()
