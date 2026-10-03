#!/usr/bin/env bash
set -euo pipefail
# Resolve every DT_NEEDED from the new outputs or this exact SDK sysroot.
test "$#" = 3 || { echo 'Usage: runtime-libs.sh LINUX_BUILD SYSROOT BUNDLE' >&2; exit 2; }
BUILD=$1 SYSROOT=$2 BUNDLE=$3
mkdir -p "$BUNDLE/lib" "$BUNDLE/meta"
queue=("$BUILD/media_service" "$BUILD/demo_ui" "$BUILD/democtl")
declare -A copied
printf 'dependency\tsource\tsha256\n' > "$BUNDLE/meta/runtime.tsv"
for binary in "${queue[@]}"; do
    interpreter=$(readelf -W -l "$binary" | sed -n 's/.*Requesting program interpreter: \([^]]*\).*/\1/p')
    test "$interpreter" = /lib/ld-linux-riscv64xthead-lp64d.so.1 || { echo "Unexpected interpreter: $interpreter" >&2; exit 1; }
done
queue+=("$SYSROOT/lib/ld-linux-riscv64xthead-lp64d.so.1")
copy_library() {
    local name=$1 origin=$2 hash
    test -n "${copied[$name]:-}" && return
    copied[$name]=1
    cp -L "$origin" "$BUNDLE/lib/$name"
    hash=$(sha256sum "$BUNDLE/lib/$name"); hash=${hash%% *}
    printf '%s\t%s\t%s\n' "$name" "$(readlink -f "$origin")" "$hash" >> "$BUNDLE/meta/runtime.tsv"
    queue+=("$origin")
}
copy_library ld-linux-riscv64xthead-lp64d.so.1 "$SYSROOT/lib/ld-linux-riscv64xthead-lp64d.so.1"
for ((index=0; index<${#queue[@]}; ++index)); do
    object=${queue[$index]}
    # Version/symbol reports make the selected prebuilt ABI inspectable.
    readelf -W -d -V "$object" >> "$BUNDLE/meta/runtime-elf.txt"
    while IFS= read -r name; do
        [[ "$name" =~ ^[a-zA-Z0-9_.+-]+$ ]] || { echo "Unsafe DT_NEEDED: $name" >&2; exit 1; }
        test -n "${copied[$name]:-}" && continue
        origin=
        for directory in "$BUILD" "$SYSROOT/lib" "$SYSROOT/usr/lib"; do
            if [ -f "$directory/$name" ]; then origin="$directory/$name"; break; fi
        done
        test -n "$origin" || { echo "Unresolved DT_NEEDED: $name" >&2; exit 1; }
        copy_library "$name" "$origin"
    done < <(readelf -W -d "$object" | sed -n 's/.*(NEEDED).*\[\([^]]*\)\].*/\1/p')
done
for library in "$BUNDLE"/lib/*; do
    out/tests/elf_check "$library" linux
done
bash tools/runtime-check.sh "$BUNDLE"
echo 'Recursive DT_NEEDED closure and exact packaged loader collected; not runtime execution'
