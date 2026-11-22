#!/usr/bin/env python3
"""Bundle the shared settings site and an existing signed release for static hosting."""
import argparse
import hashlib
import json
import os
import re
import tempfile
import zipfile
from pathlib import Path

WEB = Path(__file__).resolve().parents[1] / "web"


def package(ota: Path, output: Path):
    manifest = (ota / "manifest.json").read_bytes()
    release = json.loads(manifest)
    digest = release["sha256"]
    if release["board"] != "waveshare-s3-epaper-1.54-v2" or not re.fullmatch("[0-9a-f]{64}", digest):
        raise ValueError("expected a Quack Pocket V2 release")
    image_name = digest + ".bin"
    image = (ota / image_name).read_bytes()
    if len(image) != release["size"] or hashlib.sha256(image).hexdigest() != digest:
        raise ValueError("release image differs from the signed manifest")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output.parent, delete=False) as temp:
        stage = Path(temp.name)
    try:
        with zipfile.ZipFile(stage, "w", zipfile.ZIP_DEFLATED) as archive:
            for name in ("index.html", "manifest.webmanifest", "sw.js"):
                archive.write(WEB / name, name)
            for icon in sorted((WEB / "icons").glob("*.png")):
                archive.write(icon, icon.relative_to(WEB))
            archive.writestr("ota/" + image_name, image)
            archive.writestr("ota/manifest.json", manifest)
        os.replace(stage, output)
    finally:
        stage.unlink(missing_ok=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ota-dir", type=Path, default=Path("releases/ota"))
    parser.add_argument("--output", type=Path, default=Path("releases/quack-pocket-site.zip"))
    options = parser.parse_args()
    package(options.ota_dir, options.output)
    print(options.output)
