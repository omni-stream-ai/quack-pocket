#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
revision=2f287c01d763e44b4307c48af95044acee5c891e
destination=.cache/waveshare
mkdir -p "$destination"
if [[ -f "$destination/font_puhui_14_1.c" ]] &&
   [[ $(sha256sum "$destination/font_puhui_14_1.c" | cut -d' ' -f1) == 025b0d26b0da408664501bcc23c065a50cd9c1b46accdea4f7fbb60706891c0a ]]; then
    exit 0
fi
temporary=$(mktemp "$destination/font-XXXXXX")
trap 'rm -f "$temporary" "$temporary.converted"' EXIT

curl --fail --location --retry 3 \
    "https://raw.githubusercontent.com/waveshareteam/ESP32-S3-ePaper-1.54/${revision}/02_Example/XiaoZhi/78__xiaozhi-fonts/src/font_puhui_14_1.c" \
    -o "$temporary"
printf '%s  %s\n' '0684c8dbe08fe5ab374037b222fcc1376c28290146828baa67cf353ac465eac1' "$temporary" | sha256sum -c -
sed 's/\bfont_puhui_14_1\b/waveshare_font_puhui_14_1/g' "$temporary" > "$temporary.converted"
mv "$temporary.converted" "$destination/font_puhui_14_1.c"
