#!/bin/sh
# Explicit manual deployment; do not kill a running recorder or auto-start UI.
set -eu
if [ "$#" -ne 2 ]; then
    echo 'Usage: install-media-candidate.sh UI_SHA256 BIG_SHA256' >&2
    exit 2
fi
if pidof ui >/dev/null 2>&1; then
    echo 'UI is running; stop recording and exit UI before installing.' >&2
    exit 1
fi
grep -q ' /sharefs ' /proc/mounts
test -s /tmp/lawrec-media-ui
test -s /sharefs/lawrec-media-20260930.elf
test "$(sha256sum /tmp/lawrec-media-ui | cut -d ' ' -f 1)" = "$1"
test "$(sha256sum /sharefs/lawrec-media-20260930.elf | cut -d ' ' -f 1)" = "$2"
app=/app/lawrec/ui/ui
test -x "$app"
if [ ! -e "$app.before-media-20260930" ]; then
    cp -p "$app" "$app.before-media-20260930"
fi
cp /tmp/lawrec-media-ui "$app.new"
chmod 755 "$app.new"
mv "$app.new" "$app"
mkdir -p /sharefs/lawrec_records
sync
sha256sum "$app" /sharefs/lawrec-media-20260930.elf
echo 'Installed; autostart unchanged. Start the matching big-core ELF manually first.'
