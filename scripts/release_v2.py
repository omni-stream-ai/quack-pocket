#!/usr/bin/env python3
"""Build the V2 firmware and stage a signed release for the settings website."""
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

from package_firmware import package, publish_files, write_atomic
from package_update_site import package as package_site

ROOT = Path(__file__).resolve().parents[1]


def build_firmware():
    subprocess.run([ROOT / "scripts/fetch_waveshare_fonts.sh"], check=True, cwd=ROOT)
    idf = os.environ.get("IDF_PY") or shutil.which("idf.py")
    if idf:
        command = [idf, "build"]
    else:
        cache = ROOT / ".cache/idf-home"
        cache.mkdir(parents=True, exist_ok=True)
        command = [
            "docker", "run", "--rm", "--user", f"{os.getuid()}:{os.getgid()}",
            "-e", "HOME=/project/.cache/idf-home", "-v", f"{ROOT}:/project", "-w", "/project",
            "espressif/idf:v5.5.2", "idf.py", "build",
        ]
    subprocess.run(command, check=True, cwd=ROOT)


def release():
    releases = ROOT / "releases"
    releases.mkdir(exist_ok=True)
    key = Path(os.environ.get("OTA_SIGNING_KEY", "~/.local/share/quack-pocket/ota-signing.pem")).expanduser()
    if not key.is_file():
        raise SystemExit(f"Missing OTA signing key: {key}")
    with (releases / ".release.lock").open("a") as lock, tempfile.TemporaryDirectory(dir=releases) as tmp:
        fcntl.flock(lock, fcntl.LOCK_EX)
        build_firmware()
        stage = Path(tmp)
        usb = stage / "omni-epaper-v2.zip"
        site = stage / "quack-pocket-site.zip"
        ota = stage / "ota"
        package(ROOT / "build", usb)
        publish_files(usb, ota, key)
        package_site(ota, site)
        manifest = (ota / "manifest.json").read_bytes()
        meta = json.loads(manifest)
        name = meta["sha256"] + ".bin"
        image = (ota / name).read_bytes()
        ota_zip = stage / f"quack-pocket-ota-{meta['version']}.zip"
        with zipfile.ZipFile(ota_zip, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.write(ota / name, name)
            archive.write(ota / "manifest.json", "manifest.json")
        for artifact in (usb, ota_zip, site):
            os.replace(artifact, releases / artifact.name)
        for directory in (releases / "ota", ROOT / "web/ota"):
            directory.mkdir(exist_ok=True)
            write_atomic(directory / name, image)
            write_atomic(directory / "manifest.json", manifest)
        print(f"V{meta['version']} ready: web/ota/ and releases/{site.name}")


if __name__ == "__main__":
    release()
