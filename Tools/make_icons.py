#!/usr/bin/env python3
"""Generate the iPhone and CarPlay app icons from Branding/logo-1024.png.

    python3 Tools/make_icons.py

Needs Pillow (pip install pillow). Icons are opaque (no alpha), as Apple
requires.
"""
import json
import os

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "Branding", "logo-1024.png")
ICONSET = os.path.join(ROOT, "App", "Resources", "Assets.xcassets", "AppIcon.appiconset")
LAUNCH = os.path.join(ROOT, "App", "Resources", "Assets.xcassets", "LaunchLogo.imageset")

# (idiom, size in points, scale)
ICONS = [
    ("iphone", 20, 2), ("iphone", 20, 3), ("iphone", 29, 2), ("iphone", 29, 3),
    ("iphone", 40, 2), ("iphone", 40, 3), ("iphone", 60, 2), ("iphone", 60, 3),
    ("car", 60, 2), ("car", 60, 3),
    ("ios-marketing", 1024, 1),
]


def main():
    src = Image.open(SRC).convert("RGB")
    os.makedirs(ICONSET, exist_ok=True)
    images = []
    for idiom, pt, scale in ICONS:
        px = pt * scale
        name = f"icon-{idiom}-{pt}@{scale}x.png"
        src.resize((px, px), Image.LANCZOS).save(os.path.join(ICONSET, name), optimize=True)
        images.append({"idiom": idiom, "size": f"{pt}x{pt}", "scale": f"{scale}x", "filename": name})
    with open(os.path.join(ICONSET, "Contents.json"), "w") as f:
        json.dump({"images": images, "info": {"version": 1, "author": "xcode"}}, f, indent=2)

    os.makedirs(LAUNCH, exist_ok=True)
    launch = []
    for scale in (1, 2, 3):
        name = f"launch-logo@{scale}x.png"
        src.resize((140 * scale, 140 * scale), Image.LANCZOS).save(os.path.join(LAUNCH, name), optimize=True)
        launch.append({"idiom": "universal", "scale": f"{scale}x", "filename": name})
    with open(os.path.join(LAUNCH, "Contents.json"), "w") as f:
        json.dump({"images": launch, "info": {"version": 1, "author": "xcode"}}, f, indent=2)
    with open(os.path.join(os.path.dirname(ICONSET), "Contents.json"), "w") as f:
        json.dump({"info": {"version": 1, "author": "xcode"}}, f, indent=2)
    print(f"wrote {len(images)} app icons and {len(launch)} launch images")


if __name__ == "__main__":
    main()
