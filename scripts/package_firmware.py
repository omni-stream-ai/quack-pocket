#!/usr/bin/env python3
"""Package the verified V2 flash inputs without copying device or build caches."""
import argparse
import hashlib
import json
import os
import tempfile
import zipfile
from pathlib import Path


def package(build: Path, destination: Path):
    cache = (build / "CMakeCache.txt").read_text()
    for line in cache.splitlines():
        if line.startswith("OMNI_MUSIC_HIL:") and line.partition("=")[2].upper() not in ("", "OFF", "NO", "FALSE", "0"):
            raise ValueError("hardware acceptance images cannot be packaged as the daily firmware")
    args = json.loads((build / "flasher_args.json").read_text())
    if args["extra_esptool_args"]["chip"] != "esp32s3" or args["flash_settings"]["flash_size"] != "8MB":
        raise ValueError("expected an ESP32-S3 8 MB flash build")
    files = sorted(args["flash_files"].items(), key=lambda item: int(item[0], 16))
    if not all(section in args for section in ("bootloader", "partition-table", "app", "assets", "otadata")):
        raise ValueError("build is missing required flash sections")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as temp:
        stage = Path(temp.name)
    try:
        checksums = []
        with zipfile.ZipFile(stage, "w", zipfile.ZIP_DEFLATED) as out:
            for name in ["flash_args", "flasher_args.json", *(file for _, file in files)]:
                path = (build / name).resolve()
                if not path.is_relative_to(build.resolve()):
                    raise ValueError("flash input outside build directory")
                data = path.read_bytes()
                checksums.append(f"{hashlib.sha256(data).hexdigest()}  {name}\n")
                out.writestr(name, data)
            out.writestr("SHA256SUMS", "".join(checksums))
        os.replace(stage, destination)
    finally:
        stage.unlink(missing_ok=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path("build"))
    parser.add_argument("--output", type=Path, default=Path("releases/omni-epaper-v2.zip"))
    options = parser.parse_args()
    package(options.build, options.output)
    print(options.output)
