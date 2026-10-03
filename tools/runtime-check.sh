#!/usr/bin/env bash
set -euo pipefail
test "$#" = 1 || exit 2
BUNDLE=$1
for program in media_service demo_ui democtl; do
    queue=("$BUNDLE/bin/$program")
    declare -A visited=()
    imports="$BUNDLE/meta/$program.imports"
    exports="$BUNDLE/meta/$program.exports"
    : > "$imports"; : > "$exports"
    for ((index=0; index<${#queue[@]}; ++index)); do
        object=${queue[$index]}
        test -n "${visited[$object]:-}" && continue
        visited[$object]=1
        readelf -W --dyn-syms "$object" | awk '
            $7 == "UND" && $5 == "GLOBAL" { name=$8; gsub(/@@/, "@", name); print name }
        ' >> "$imports"
        readelf -W --dyn-syms "$object" | awk '
            $7 != "UND" && ($5 == "GLOBAL" || $5 == "WEAK" || $5 == "UNIQUE") {
                name=$8; gsub(/@@/, "@", name); print name;
                if ($8 ~ /@@/ || $8 !~ /@/) { sub(/@.*/, "", name); print name }
            }
        ' >> "$exports"
        while IFS= read -r dependency; do
            test -f "$BUNDLE/lib/$dependency" || { echo "Missing library $dependency for $program" >&2; exit 1; }
            queue+=("$BUNDLE/lib/$dependency")
        done < <(readelf -W -d "$object" | sed -n 's/.*(NEEDED).*\[\([^]]*\)\].*/\1/p')
    done
    LC_ALL=C sort -u "$imports" -o "$imports"
    LC_ALL=C sort -u "$exports" -o "$exports"
    LC_ALL=C comm -23 "$imports" "$exports" > "$BUNDLE/meta/$program.unresolved"
    if [ -s "$BUNDLE/meta/$program.unresolved" ]; then
        echo "Unresolved strong dynamic symbols/version requirements for $program:" >&2
        head -20 "$BUNDLE/meta/$program.unresolved" >&2
        exit 1
    fi
    echo "$program: per-program library closure + strong dynamic symbol/version names resolved (not execution)"
done
