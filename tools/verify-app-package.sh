#!/bin/sh
# Board-compatible integrity check only; never installs or starts anything.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
cd "$root"
test -z "$(find . -type l -print)" || { echo 'Refusing bundle symlinks' >&2; exit 1; }
files=$(find . -type f ! -name SHA256SUMS -print0 | tr -cd '\000' | wc -c)
test "$files" -eq "$(wc -l < SHA256SUMS)" || { echo 'Unlisted/missing bundle files' >&2; exit 1; }
for path in app/lawrec/ui/ui lawrec.elf lib/liblvgl.so lib/liblv_drivers.so etc/init.d/S99lawrec etc/init.d/S45wifi; do
    test -s "$path" && test -x "$path" || { echo "Missing/non-executable: $path" >&2; exit 1; }
    grep -Fqx "$(sha256sum "./$path")" SHA256SUMS || { echo "Unlisted or changed: $path" >&2; exit 1; }
done
for path in BUILD.txt source.tar.gz source/SOURCE.sha256 source/SOURCE.modes sdk-inputs/SDK.sha256; do
    test -s "$path"
    grep -Fqx "$(sha256sum "./$path")" SHA256SUMS || { echo "Unlisted or changed: $path" >&2; exit 1; }
done
sha256sum -c SHA256SUMS
(cd sdk-inputs && sha256sum -c SDK.sha256)
echo 'PASS application package integrity; hardware/kernel compatibility is not verified.'
