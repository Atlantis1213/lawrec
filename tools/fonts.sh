#!/usr/bin/env bash
set -euo pipefail
test -f /.dockerenv || { echo 'Docker required' >&2; exit 1; }
NODE=/opt/font-node
CONVERTER=/opt/font-converter/lv_font_conv/lv_font_conv.js
test "$("$NODE" -p "require('/opt/font-converter/lv_font_conv/package.json').version")" = 1.5.3
FONTS="$LVGL_ROOT/scripts/built_in_font"
# Include every non-ASCII code point in the actual page, not a hand-maintained list.
SYMBOLS=$("$NODE" -e "process.stdout.write([...new Set([...require('fs').readFileSync('little/ui/page.cpp','utf8')].filter(c=>c.codePointAt(0)>127))].sort((a,b)=>a.codePointAt(0)-b.codePointAt(0)).join(''))")
mkdir -p little/ui/fonts out
{
    "$NODE" --version
    sha256sum "$FONTS/SimSun.woff" "$FONTS/Montserrat-Medium.ttf"
    printf 'Chinese subset: %s\n' "$SYMBOLS"
} > out/ui-font-provenance.txt
for SIZE in 20 24 32; do
    "$NODE" "$CONVERTER" --no-compress --no-prefilter --no-kerning --bpp 4 --size "$SIZE" \
        --font "$FONTS/Montserrat-Medium.ttf" -r 0x20-0x7E \
        --font "$FONTS/SimSun.woff" --symbols "$SYMBOLS" --format lvgl \
        -o "little/ui/fonts/demo_font_cn_$SIZE.c"
done
echo 'Generated Chinese/ASCII font subsets; frozen adapter/fonts untouched'
