#!/usr/bin/env python3
"""Package the verified V2 flash inputs without copying device or build caches."""
import argparse
import hashlib
import json
import os
import re
import subprocess
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


def write_atomic(path: Path, data: bytes):
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as tmp:
        stage = Path(tmp.name)
    try:
        stage.write_bytes(data)
        stage.chmod(0o644)
        os.replace(stage, path)
    finally:
        stage.unlink(missing_ok=True)


def publish_files(usb_package: Path, directory: Path, key: Path):
    """Sign the exact application packaged for USB. Publish the manifest last."""
    with zipfile.ZipFile(usb_package) as archive:
        args = json.loads(archive.read("flasher_args.json"))
        image = archive.read(args["app"]["file"])
    if image[0:1] != b"\xe9" or image[32:36] != bytes.fromhex("3254cdab") or len(image) > 0x360000:
        raise ValueError("invalid V2 application image")
    version = image[48:80].split(b"\0", 1)[0].decode("ascii")
    project = image[80:112].split(b"\0", 1)[0]
    if project != b"omni_epaper" or not re.fullmatch(r"(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){2}", version):
        raise ValueError("expected omni_epaper with a numeric major.minor.patch version")
    public = subprocess.check_output(["openssl", "pkey", "-in", str(key), "-pubout"])
    expected = (Path(__file__).resolve().parents[1] / "main/boards/waveshare-s3-epaper-1.54/ota_public.pem").read_bytes()
    if public != expected or public not in image:
        raise ValueError("signing key does not match the firmware trust key")
    board = "waveshare-s3-epaper-1.54-v2"
    digest = hashlib.sha256(image).hexdigest()
    message = f"{board}\n{version}\n{len(image)}\n{digest}\n".encode("ascii")
    signature = subprocess.check_output(["openssl", "dgst", "-sha256", "-sign", str(key)], input=message)
    manifest = dict(board=board, version=version, size=len(image), sha256=digest, signature=signature.hex())
    directory.mkdir(parents=True, exist_ok=True)
    write_atomic(directory / (digest + ".bin"), image)
    write_atomic(directory / "manifest.json", (json.dumps(manifest, indent=2) + "\n").encode())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path("build"))
    parser.add_argument("--output", type=Path, default=Path("releases/omni-epaper-v2.zip"))
    parser.add_argument("--ota-dir", type=Path, help="also generate a signed static HTTPS release")
    parser.add_argument("--signing-key", type=Path, default=Path.home() / ".local/share/quack-pocket/ota-signing.pem")
    options = parser.parse_args()
    package(options.build, options.output)
    if options.ota_dir:
        publish_files(options.output, options.ota_dir, options.signing_key)
        print(options.ota_dir / "manifest.json")
    print(options.output)
