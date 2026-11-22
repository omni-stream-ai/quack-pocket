#!/usr/bin/env python3
"""Create the device's music index on a mounted FAT32 card."""

import argparse
import os
from pathlib import Path
import struct
import tempfile

HEADER = struct.Struct("<4sI")
OFFSET = struct.Struct("<I")
MAX_PATH_BYTES = 1023


def write_index(card: Path) -> int:
    if not card.is_dir():
        raise ValueError(f"not a directory: {card}")

    paths = []
    for path in card.rglob("*"):
        if path.suffix.lower() != ".mp3" or path.is_symlink() or not path.is_file() or path.stat().st_size < 10:
            continue
        relative = path.relative_to(card).as_posix()
        encoded = relative.encode("utf-8")
        if len(encoded) > MAX_PATH_BYTES or any(ord(char) < 32 for char in relative):
            raise ValueError(f"unsupported MP3 path: {relative!r}")
        paths.append(encoded)
    paths.sort()

    offset = HEADER.size + OFFSET.size * (len(paths) + 1)
    if offset + sum(map(len, paths)) > 0x7FFFFFFF:
        raise ValueError("music index exceeds the device file offset limit")

    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=card, prefix="omni-", delete=False) as output:
            temporary = Path(output.name)
            output.write(HEADER.pack(b"OMI1", len(paths)))
            output.write(OFFSET.pack(offset))
            for path in paths:
                offset += len(path)
                output.write(OFFSET.pack(offset))
            for path in paths:
                output.write(path)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, card / "omni.idx")
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return len(paths)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("card", type=Path)
    args = parser.parse_args()
    try:
        count = write_index(args.card)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Indexed {count} MP3 files")
