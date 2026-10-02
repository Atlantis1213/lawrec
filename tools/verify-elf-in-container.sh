#!/usr/bin/env bash
# Inspect cross-compiled artifacts; never execute them. Keep SDK layout unchanged.
set -euo pipefail
[[ -f /.dockerenv ]] || { echo 'Docker is required'; exit 1; }

check_elf() {
    local file=$1 role=$2 work header sections section
    work=$(mktemp -d)
    header="$work/header"
    if ! readelf -h -l -W "$file" > "$header" 2> "$work/errors"; then
        cat "$work/errors" >&2; rm -rf "$work"; return 1
    fi
    if ! grep -q 'Class:.*ELF64' "$header" ||
        ! grep -q 'Data:.*little endian' "$header" ||
        ! grep -q 'Machine:.*RISC-V' "$header" ||
        ! grep -q 'Flags:.*double-float ABI' "$header" ||
        ! grep -q '^[[:space:]]*LOAD[[:space:]]' "$header"; then
        echo "Wrong architecture/ABI or no load segment: $file" >&2
        rm -rf "$work"; return 1
    fi
    case "$role" in
        big) grep -q 'Type:.*EXEC' "$header" ;;
        ui) grep -Eq 'Type:.*(EXEC|DYN)' "$header" ;;
        library) grep -q 'Type:.*DYN' "$header" ;;
        *) rm -rf "$work"; return 1 ;;
    esac || { echo "Wrong ELF type: $file" >&2; rm -rf "$work"; return 1; }
    if [[ -s "$work/errors" ]]; then
        # This SDK's linker script can mark .got as RELA with 8-byte entries.
        # Allow only that precisely identified historical diagnostic, not arbitrary
        # readelf errors. This is not proof of board loading or permission to fix it.
        if [[ "$role" != big ]] || [[ $(wc -l < "$work/errors") -ne 2 ]] ||
            ! grep -Eq '^readelf: Error: Section [0-9]+ has invalid sh_entsize of (8|0000000000000008)$' "$work/errors" ||
            ! grep -Eq '^readelf: Error: \(Using the expected size of (18|24) for the rest of this dump\)$' "$work/errors"; then
            cat "$work/errors" >&2; rm -rf "$work"; return 1
        fi
        section=$(sed -n 's/^readelf: Error: Section \([0-9]*\) has.*/\1/p' "$work/errors")
        sections="$work/sections"
        readelf -SW "$file" > "$sections" 2>/dev/null || { rm -rf "$work"; return 1; }
        grep -Eq "^[[:space:]]*\[[[:space:]]*$section\][[:space:]]+\.got[[:space:]]+RELA[[:space:]]" "$sections" || { rm -rf "$work"; return 1; }
        echo "KNOWN SDK WARNING: .got metadata; loader/board acceptance still required ($file)"
    fi
    printf 'PASS ELF64 little-endian RISC-V LP64D / %s: %s\n' "$role" "$file"
    rm -rf "$work"
}

if [[ "${1:-}" = --reject-host ]]; then
    if check_elf /bin/true ui; then exit 1; fi
    echo 'PASS ELF gate rejects the native host executable'
    exit 0
fi
[[ $# -eq 1 ]] || { echo 'Usage: verify-elf-in-container.sh PACKAGE_DIRECTORY'; exit 2; }
check_elf "$1/app/lawrec/ui/ui" ui
check_elf "$1/lawrec.elf" big
check_elf "$1/lib/liblvgl.so" library
check_elf "$1/lib/liblv_drivers.so" library
