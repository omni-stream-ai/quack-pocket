"""Build the device copy of the settings page and its precompressed script."""
import gzip
import sys
from pathlib import Path

source, html, script = map(Path, sys.argv[1:])
before, javascript = source.read_bytes().split(b"<script>")
javascript, after = javascript.split(b"</script>")
html.write_bytes(before + b'<script src="/setup.js"></script>' + after)
script.write_bytes(gzip.compress(javascript, compresslevel=9, mtime=0))
